// The carried-frame grid changing size on ONE machine (D3D12 WARP), as it does when the layout or GlobalScale
// changes: a half grid, then an unequal grid with odd extents, then native. Before each of them a carried frame
// whose colour is infinite fills every texel of toneAcc with NaN; each grid frame must still come out as colour +
// the constant residual everywhere, so nothing outside the corner it wrote itself is read, at any weight.
#include "temporal_d3d12_fixture.h"
#include "core/temporal/machine.h"

#include <algorithm>
#include <cstdio>
#include <iostream>
#include <limits>

using namespace ofps::core::temporal;
using namespace temporal_d3d12;

namespace {
int failures = 0;

void Check(bool condition, const char *message)
{
    if (condition) return;
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
}
} // namespace

int TestTemporalGridSwitch(ID3D12Device *device, ID3D12CommandQueue *queue, const char *shaderDirectory)
{
    ofps::core::gpu::Shaders shaders;
    Check(LoadShaders(shaderDirectory, shaders, true), "grid switch: shaders load");
    Recording r;
    Check(r.Create(device), "grid switch: fixtures allocate");
    if (failures) return failures;
    constexpr float kInf = std::numeric_limits<float>::infinity();
    const auto color = r.Paint(device, kColor, {0.4f, 0.4f, 0.4f, 1.0f}, {}, false);
    const auto poison = r.Paint(device, kColor, {kInf, kInf, kInf, 1.0f}, {}, false);
    const auto depth = r.Paint(device, DXGI_FORMAT_R32_FLOAT, {0.5f, 0, 0, 0}, {}, false);
    const auto motion = r.Paint(device, DXGI_FORMAT_R16G16_FLOAT, {0, 0, 0, 0}, {}, false);
    const auto fresh = r.Paint(device, kColor, {0.525f, 0.525f, 0.525f, 1.0f}, {}, false);
    Machine machine;
    char error[256]{};
    Check(color && poison && depth && motion && fresh &&
              machine.Initialize(device, shaders, kW, kH, kColor, kColor, kW, kH, DXGI_FORMAT_R32_FLOAT, kW, kH, error, sizeof(error)),
          "grid switch: machine initializes");
    if (!machine.Ready()) { std::cerr << error << '\n'; return failures; }
    machine.SetUsePoint(r.Point(), 0);

    FrameInputs in{};
    in.color = color.Get(); in.depth = depth.Get(); in.motion = motion.Get();
    in.colorView = kColor; in.depthView = DXGI_FORMAT_R32_FLOAT; in.motionView = DXGI_FORMAT_R16G16_FLOAT;
    in.colorRect = in.depthRect = in.motionRect = {0, 0, kW, kH};
    in.hostInputState = in.depthState = in.motionState = kRead;
    in.history = false;
    machine.RecordResidual(r.list.Get(), in, fresh.Get(), kRead); // the residual is 0.125 everywhere from here on
    // [2k] the poisoning frame before step k, [2k + 1] step k.
    const struct { std::uint32_t gridW, gridH; } steps[] = {{kW / 2, kH / 2}, {171, 45}, {0, 0}};
    std::vector<ComPtr<ID3D12Resource>> outputs;
    for (const auto &step : steps) {
        for (const bool poisoning : {true, false}) {
            outputs.push_back(Texture(device, kColor, D3D12_RESOURCE_FLAG_NONE));
            FrameInputs frame = in;
            if (poisoning) frame.color = poison.Get();
            frame.gridWidth = poisoning ? 0 : step.gridW;
            frame.gridHeight = poisoning ? 0 : step.gridH;
            machine.RecordAccumulate(r.list.Get(), frame);
            machine.RecordReproject(r.list.Get(), frame, outputs.back().Get(), D3D12_RESOURCE_STATE_COMMON, 0, 0);
        }
    }
    Check(!machine.TakeExhausted(), "grid switch: the passes record within the descriptor ring");
    const std::vector<Picture> pictures = Execute(device, queue, r, outputs);
    Check(pictures.size() == outputs.size(), "grid switch: the GPU completes the passes and the frames read back");
    if (pictures.size() != outputs.size()) return failures;

    const float expected = Half(0x3666) + 0.125f; // fp16 0.4 + the residual
    for (std::size_t k = 0; k < std::size(steps); ++k) {
        std::size_t poisoned = 0;
        float worst = 0.0f;
        for (std::size_t t = 0; t < pictures[2 * k].size(); ++t) {
            if (t % 4 == 3) continue;
            poisoned += std::isnan(pictures[2 * k][t]) ? 1 : 0;
            const float value = pictures[2 * k + 1][t];
            worst = std::isfinite(value) ? std::max(worst, std::abs(value - expected)) : std::numeric_limits<float>::infinity();
        }
        std::printf("grid switch: step %zu (%ux%u) after a frame with %zu NaN channels: max |frame - expected| %.3g\n", k,
                    steps[k].gridW, steps[k].gridH, poisoned, worst);
        Check(poisoned == static_cast<std::size_t>(kW) * kH * 3, "grid switch: the poisoning frame left NaN in every texel of toneAcc");
        Check(worst <= 2.0e-3f, "grid switch: a frame read a texel an earlier frame left in toneAcc");
    }
    return failures;
}
