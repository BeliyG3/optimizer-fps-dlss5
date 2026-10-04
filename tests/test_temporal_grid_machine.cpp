// The carried-frame grid through Machine::RecordReproject on D3D12 WARP (FrameInputs::gridWidth/gridHeight set
// directly, as the add-on's caller does):
//   - off path: no grid, a grid larger than the frame, and a machine without the grid pipelines all write the same
//     bytes;
//   - a grid of the native size stays within fp16 tolerance of the native route (max/mean printed);
//   - a half grid takes the grid route and stays close to the native route on average (the grid's texels stand for
//     the right native positions), and after a native frame left a large addition in every texel of toneAcc,
//     a still grey frame with a constant residual comes out as colour + that residual (nothing stale is read).
// Switching sizes on one machine: test_temporal_grid_switch.cpp.
#include "temporal_d3d12_fixture.h"
#include "core/temporal/machine.h"

#include <algorithm>
#include <cstdio>
#include <iostream>

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

struct Scene {
    ComPtr<ID3D12Resource> color, depth, motion, fresh[2];
};

FrameInputs Inputs(const Scene &scene)
{
    FrameInputs in{};
    in.color = scene.color.Get(); in.depth = scene.depth.Get(); in.motion = scene.motion.Get();
    in.colorView = kColor; in.depthView = DXGI_FORMAT_R32_FLOAT; in.motionView = DXGI_FORMAT_R16G16_FLOAT;
    in.colorRect = in.depthRect = in.motionRect = {0, 0, kW, kH};
    in.hostInputState = in.depthState = in.motionState = kRead;
    return in;
}

// Two full passes, each followed by a carried frame; the first carried frame is native, the second runs on `grid`
// and goes to `output`.
void RecordRun(Machine &machine, ID3D12GraphicsCommandList *list, const Scene &scene, bool history, std::uint32_t gridW,
               std::uint32_t gridH, ID3D12Resource *output)
{
    FrameInputs in = Inputs(scene);
    in.history = history;
    for (int pass = 0; pass < 2; ++pass) {
        in.gridWidth = pass == 1 ? gridW : 0;
        in.gridHeight = pass == 1 ? gridH : 0;
        machine.RecordResidual(list, in, scene.fresh[pass].Get(), kRead);
        machine.RecordAccumulate(list, in);
        machine.RecordReproject(list, in, pass == 1 ? output : nullptr, D3D12_RESOURCE_STATE_COMMON, 0, 0);
    }
}

struct Diff { double max = 0.0, mean = 0.0; bool finite = true; };
Diff Compare(const Picture &a, const Picture &b)
{
    Diff d;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const double e = std::abs(double(a[i]) - double(b[i]));
        d.finite = d.finite && std::isfinite(a[i]) && std::isfinite(b[i]);
        d.max = std::max(d.max, e);
        d.mean += e / double(a.size());
    }
    return d;
}
} // namespace

int TestTemporalGridMachine(ID3D12Device *device, ID3D12CommandQueue *queue, const char *shaderDirectory)
{
    ofps::core::gpu::Shaders withGrid, withoutGrid;
    Check(LoadShaders(shaderDirectory, withGrid, true) && LoadShaders(shaderDirectory, withoutGrid, false), "grid machine: shaders load");
    Recording r;
    Check(r.Create(device), "grid machine: fixtures allocate");
    if (failures) return failures;

    // A patch that moves over a background (structure for the grid to change), and a still grey frame whose first
    // pass adds 8 and whose second adds 0.125 everywhere.
    Scene moving, still;
    moving.color = r.Paint(device, kColor, {0.4f, 0.3f, 0.2f, 1.0f}, {0.8f, 0.1f, 0.6f, 1.0f}, true);
    moving.depth = r.Paint(device, DXGI_FORMAT_R32_FLOAT, {0.5f, 0, 0, 0}, {0.25f, 0, 0, 0}, true);
    moving.motion = r.Paint(device, DXGI_FORMAT_R16G16_FLOAT, {0, 0, 0, 0}, {1.5f, -0.5f, 0, 0}, true);
    moving.fresh[0] = r.Paint(device, kColor, {0.5f, 0.3f, 0.1f, 1.0f}, {0.9f, 0.2f, 0.5f, 1.0f}, true);
    moving.fresh[1] = r.Paint(device, kColor, {0.45f, 0.35f, 0.15f, 1.0f}, {0.7f, 0.25f, 0.65f, 1.0f}, true);
    still.color = r.Paint(device, kColor, {0.4f, 0.4f, 0.4f, 1.0f}, {}, false);
    still.depth = r.Paint(device, DXGI_FORMAT_R32_FLOAT, {0.5f, 0, 0, 0}, {}, false);
    still.motion = r.Paint(device, DXGI_FORMAT_R16G16_FLOAT, {0, 0, 0, 0}, {}, false);
    still.fresh[0] = r.Paint(device, kColor, {8.4f, 8.4f, 8.4f, 1.0f}, {}, false);
    still.fresh[1] = r.Paint(device, kColor, {0.525f, 0.525f, 0.525f, 1.0f}, {}, false);

    const struct { const ofps::core::gpu::Shaders *shaders; const Scene *scene; std::uint32_t gridW, gridH; } runs[] = {
        {&withGrid, &moving, 0, 0},           // 0: native
        {&withoutGrid, &moving, kW / 2, kH / 2}, // 1: grid asked for, pipelines missing
        {&withGrid, &moving, kW * 2, kH},     // 2: a grid wider than the frame
        {&withGrid, &moving, kW, kH},         // 3: a grid of the native size
        {&withGrid, &moving, kW / 2, kH / 2}, // 4: half grid
        {&withGrid, &still, kW / 2, kH / 2},  // 5: half grid after a frame that left 8 everywhere
        {&withGrid, &still, 0, 0}};           // 6: the same, native
    constexpr std::size_t kRuns = std::size(runs);
    std::vector<ComPtr<ID3D12Resource>> outputs(kRuns);
    std::vector<Machine> machines(kRuns);
    for (std::size_t i = 0; i < kRuns; ++i) {
        outputs[i] = Texture(device, kColor, D3D12_RESOURCE_FLAG_NONE);
        char error[256]{};
        Check(outputs[i] && machines[i].Initialize(device, *runs[i].shaders, kW, kH, kColor, kColor, kW, kH, DXGI_FORMAT_R32_FLOAT, kW, kH,
                                                   error, sizeof(error)),
              "grid machine: machine initializes");
        if (!machines[i].Ready()) { std::cerr << error << '\n'; return failures; }
        machines[i].SetUsePoint(r.Point(), 0);
        RecordRun(machines[i], r.list.Get(), *runs[i].scene, runs[i].scene == &moving, runs[i].gridW, runs[i].gridH, outputs[i].Get());
        Check(!machines[i].TakeExhausted(), "grid machine: the passes record within the descriptor ring");
    }
    const std::vector<Picture> pictures = Execute(device, queue, r, outputs);
    Check(pictures.size() == kRuns, "grid machine: the GPU completes the passes and the frames read back");
    if (pictures.size() != kRuns) return failures;

    Check(SameBits(pictures[0], pictures[1]), "grid machine: without the grid pipelines the frame is the native one");
    Check(SameBits(pictures[0], pictures[2]), "grid machine: a grid wider than the frame is ignored");
    Check(!SameBits(pictures[0], pictures[4]), "grid machine: a half grid takes the grid route");
    const Diff same = Compare(pictures[0], pictures[3]), half = Compare(pictures[0], pictures[4]), still5 = Compare(pictures[5], pictures[6]);
    std::printf("grid machine: native-size grid vs native max %.3g mean %.3g; half grid vs native max %.3g mean %.3g; "
                "still half vs native max %.3g\n", same.max, same.mean, half.max, half.mean, still5.max);
    Check(same.finite && same.max <= 2.0e-3, "grid machine: a native-size grid stays within fp16 tolerance of the native route");
    // On average the half grid follows the native route (0.0011 here; a reprojection dispatched over the native extent
    // instead of the grid's, i.e. grid texels at the wrong native positions, gave 0.04).
    Check(half.finite && half.mean <= 0.005, "grid machine: the half grid follows the native route on average");
    Check(still5.finite && still5.max <= 2.0e-3, "grid machine: the half grid reads nothing a previous frame left outside its corner");
    float worst = 0.0f;
    for (std::size_t t = 0; t < pictures[5].size(); ++t)
        if (t % 4 != 3) worst = std::max(worst, std::abs(pictures[5][t] - (Half(0x3666) + 0.125f))); // fp16 0.4 + 0.125
    Check(worst <= 2.0e-3f, "grid machine: a still grey frame on the half grid is colour + the constant residual");
    return failures;
}
