// Machine::RecordRaw (core/temporal/raw_snapshot.h): where it takes the plain copy, the copy leaves byte for byte what
// the format-converting reprojection (RecordReproject with debugVis 3, the route RecordRaw always took before) leaves,
// for every bit pattern of the format, except the payload bits of a half-float NaN: there both are NaN, and the bits
// after the exponent may differ (counted and printed). Where it keeps the pass, the pass is needed.
//   - the rule itself (RawCopyReproduces), no device;
//   - on D3D12 WARP: a colour sub-rect at an offset inside a larger texture into a target at an offset, every
//     admitted format (and typeless resources with that view), against the pass over the same inputs;
//   - controls: an sRGB view and a ratio-domain host keep the pass, and there it does not write the colour's bytes.
// Usage: ofps_temporal_raw_tests [<shader dir>]
#include "temporal_d3d12_fixture.h"
#include "texel_gpu.h"
#include "core/temporal/machine.h"
#include "core/temporal/raw_snapshot.h"

#include <iostream>
#include <string>
#include <utility>
#include <vector>

using namespace ofps::core::temporal;
using texel_gpu::ComPtr;
using texel_gpu::Desc;
using texel_gpu::Gpu;

namespace {
int failures = 0;

void Check(bool condition, const std::string &message)
{
    if (condition) return;
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
}

std::string Name(DXGI_FORMAT format) { return std::to_string(static_cast<int>(format)); }

constexpr std::uint32_t kNativeW = 256, kNativeH = 128; // the machine's extent
constexpr std::uint32_t kColorW = kNativeW + 21, kColorH = kNativeH + 9, kRectX = 13, kRectY = 6;
constexpr std::uint32_t kTargetW = kNativeW + 7, kTargetH = kNativeH + 5, kDstX = 3, kDstY = 2;
constexpr D3D12_RESOURCE_STATES kRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

RawCopyCase Admitted()
{
    RawCopyCase c;
    c.color = Desc(DXGI_FORMAT_R16G16B16A16_FLOAT, kColorW, kColorH);
    c.target = Desc(DXGI_FORMAT_R16G16B16A16_FLOAT, kTargetW, kTargetH);
    c.colorView = c.machineView = DXGI_FORMAT_R16G16B16A16_FLOAT;
    c.rectX = kRectX; c.rectY = kRectY; c.rectW = kNativeW; c.rectH = kNativeH;
    c.nativeW = kNativeW; c.nativeH = kNativeH;
    return c;
}

void TestRule()
{
    Check(RawCopyReproduces(Admitted()), "rule: RGBA16F colour, its own view, a native-size sub-rect");
    auto c = Admitted(); c.colorSubresource = 0;
    Check(RawCopyReproduces(c), "rule: subresource 0 named explicitly");
    for (const auto &[view, typeless] : {std::pair{DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_TYPELESS},
                                         std::pair{DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_TYPELESS},
                                         std::pair{DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_R10G10B10A2_TYPELESS},
                                         std::pair{DXGI_FORMAT_R16G16B16A16_UNORM, DXGI_FORMAT_R16G16B16A16_TYPELESS}}) {
        c = Admitted(); c.color.Format = c.target.Format = typeless; c.colorView = c.machineView = view;
        Check(RawCopyReproduces(c), "rule: typeless colour and target with an exact view " + Name(view));
    }
    c = Admitted(); c.ratioScale = 0.01f;
    Check(!RawCopyReproduces(c), "rule: a ratio-domain (linear HDR) host keeps the pass: it stores the colour log-encoded");
    c = Admitted(); c.color.Format = c.target.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
    c.colorView = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; c.machineView = DXGI_FORMAT_R8G8B8A8_UNORM;
    Check(!RawCopyReproduces(c), "rule: an sRGB colour view keeps the pass (it decodes)");
    c = Admitted(); c.color.Format = DXGI_FORMAT_R16G16B16A16_TYPELESS;
    Check(!RawCopyReproduces(c), "rule: colour and target resources of different formats keep the pass");
    for (const DXGI_FORMAT view : {DXGI_FORMAT_R11G11B10_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_B8G8R8X8_UNORM,
                                   DXGI_FORMAT_R8G8B8A8_SNORM}) {
        c = Admitted(); c.color.Format = c.target.Format = c.colorView = c.machineView = view;
        Check(!RawCopyReproduces(c), "rule: a view without an exact round trip keeps the pass " + Name(view));
    }
    c = Admitted(); c.colorView = DXGI_FORMAT_R16G16B16A16_UNORM;
    Check(!RawCopyReproduces(c), "rule: a colour view other than the machine's keeps the pass");
    c = Admitted(); c.rectW = kNativeW - 1;
    Check(!RawCopyReproduces(c), "rule: a sub-rect of another size than the machine's keeps the pass (it resamples)");
    c = Admitted(); c.rectX = kColorW - kNativeW + 1;
    Check(!RawCopyReproduces(c), "rule: a sub-rect reaching past the texture keeps the pass");
    c = Admitted(); c.colorSubresource = 1;
    Check(!RawCopyReproduces(c), "rule: another subresource keeps the pass (its view reads the first one)");
    c = Admitted(); c.color.DepthOrArraySize = 2;
    Check(!RawCopyReproduces(c), "rule: a texture array keeps the pass");
    c = Admitted(); c.color.SampleDesc.Count = 4;
    Check(!RawCopyReproduces(c), "rule: a multisampled colour keeps the pass");
}

// Every texel's bits from a fixed generator. Half floats count up instead, so the 16-bit channels go through every
// pattern, the NaNs (exponent 31, mantissa != 0) included.
std::vector<std::uint8_t> Pattern(std::uint32_t w, std::uint32_t h, std::uint32_t stride, bool half, std::uint32_t seed)
{
    std::vector<std::uint8_t> texels(static_cast<std::size_t>(w) * h * stride);
    std::uint32_t state = seed;
    for (std::size_t i = 0; i < texels.size(); i += 2) {
        state = state * 1664525u + 1013904223u;
        const std::uint16_t v = half ? static_cast<std::uint16_t>(i / 2 + seed) : static_cast<std::uint16_t>(state >> 16);
        std::memcpy(texels.data() + i, &v, sizeof(v));
    }
    return texels;
}

bool HalfNaN(std::uint16_t v) { return (v & 0x7c00u) == 0x7c00u && (v & 0x03ffu) != 0; }

// `a` and `b` (`count` bytes) are equal; for half floats a 16-bit channel may also differ when both are NaN with the
// same sign and exponent, i.e. in the payload bits only. Counts those channels into `nanPayloads`.
bool SameExceptNaNPayload(const std::uint8_t *a, const std::uint8_t *b, std::size_t count, bool half, std::size_t &nanPayloads)
{
    if (std::memcmp(a, b, count) == 0) return true;
    if (!half) return false;
    for (std::size_t i = 0; i + 1 < count; i += 2) {
        std::uint16_t x, y;
        std::memcpy(&x, a + i, 2);
        std::memcpy(&y, b + i, 2);
        if (x == y) continue;
        if (!HalfNaN(x) || !HalfNaN(y) || (x & 0xfc00u) != (y & 0xfc00u)) return false;
        ++nanPayloads;
    }
    return true;
}

struct Route {
    RawRoute taken = RawRoute::None; // what RecordRaw reported
    std::vector<std::uint8_t> raw, pass, color; // the whole targets and the whole colour texture
};

// The colour (`resource`, read as `view`) through RecordRaw into one target and through the debugVis-3 reprojection
// into another; both targets start from the same pattern.
Route Snapshot(Gpu &gpu, const ofps::core::gpu::Shaders &shaders, DXGI_FORMAT resource, DXGI_FORMAT view, DXGI_FORMAT machineView,
               float ratioScale, std::uint32_t stride, bool half)
{
    Route route;
    auto color = gpu.Texture(resource, kColorW, kColorH);
    auto depth = gpu.Texture(DXGI_FORMAT_R32_FLOAT, kNativeW, kNativeH), motion = gpu.Texture(DXGI_FORMAT_R16G16_FLOAT, kNativeW, kNativeH);
    auto raw = gpu.Texture(resource, kTargetW, kTargetH), pass = gpu.Texture(resource, kTargetW, kTargetH);
    Machine machine;
    char error[256]{};
    if (!color || !depth || !motion || !raw || !pass ||
        !machine.Initialize(gpu.device.Get(), shaders, kNativeW, kNativeH, resource, machineView, kNativeW, kNativeH,
                            DXGI_FORMAT_R32_FLOAT, kNativeW, kNativeH, error, sizeof(error))) {
        Check(false, "snapshot " + Name(resource) + "/" + Name(view) + ": the temporal machine initializes (" + error + ")");
        return route;
    }
    gpu.keep.clear();
    const std::vector<std::uint8_t> zeros(std::size_t(kNativeW) * kNativeH * 4, 0);
    const auto targetStart = Pattern(kTargetW, kTargetH, stride, half, 77u);
    if (!gpu.Begin() || !gpu.Upload(color.Get(), Pattern(kColorW, kColorH, stride, half, 5u), stride, kRead) ||
        !gpu.Upload(depth.Get(), zeros, 4, kRead) || !gpu.Upload(motion.Get(), zeros, 4, kRead) ||
        !gpu.Upload(raw.Get(), targetStart, stride, D3D12_RESOURCE_STATE_COMMON) ||
        !gpu.Upload(pass.Get(), targetStart, stride, D3D12_RESOURCE_STATE_COMMON)) {
        Check(false, "snapshot: the inputs upload");
        return route;
    }
    OfpsFencePoint point{};
    point.size = sizeof(point);
    point.fence = gpu.fence.Get();
    point.value = gpu.value + 1; // what Run() signals
    machine.SetUsePoint(point, 0);
    FrameInputs in{};
    in.color = color.Get(); in.depth = depth.Get(); in.motion = motion.Get();
    in.colorView = view; in.depthView = DXGI_FORMAT_R32_FLOAT; in.motionView = DXGI_FORMAT_R16G16_FLOAT;
    in.colorRect = {kRectX, kRectY, kNativeW, kNativeH};
    in.depthRect = in.motionRect = {0, 0, kNativeW, kNativeH};
    in.hostInputState = in.depthState = in.motionState = kRead;
    in.ratioScale = ratioScale;
    route.taken = machine.RecordRaw(gpu.list.Get(), in, raw.Get(), D3D12_RESOURCE_STATE_COMMON, kDstX, kDstY);
    FrameInputs vis = in;
    vis.debugVis = 3; // what RecordRaw recorded before the copy existed
    machine.RecordReproject(gpu.list.Get(), vis, pass.Get(), D3D12_RESOURCE_STATE_COMMON, kDstX, kDstY);
    Check(!machine.TakeExhausted(), "snapshot: the passes record within the descriptor ring");
    const auto rawBack = gpu.Capture(raw.Get(), D3D12_RESOURCE_STATE_COMMON);
    const auto passBack = gpu.Capture(pass.Get(), D3D12_RESOURCE_STATE_COMMON);
    const auto colorBack = gpu.Capture(color.Get(), kRead);
    if (!gpu.Run()) {
        Check(false, "snapshot: the GPU completes the list");
        return route;
    }
    route.raw = gpu.Bytes(rawBack.Get(), raw.Get(), stride);
    route.pass = gpu.Bytes(passBack.Get(), pass.Get(), stride);
    route.color = gpu.Bytes(colorBack.Get(), color.Get(), stride);
    return route;
}

// The reprojection wrote the colour's own bytes into its box (what a copy leaves), NaN payloads aside.
bool PassHoldsColour(const Route &r, std::uint32_t stride, bool half)
{
    if (r.pass.empty() || r.color.empty()) return false;
    std::size_t nanPayloads = 0;
    for (std::uint32_t y = 0; y < kNativeH; ++y)
        if (!SameExceptNaNPayload(r.pass.data() + (std::size_t(y + kDstY) * kTargetW + kDstX) * stride,
                                  r.color.data() + (std::size_t(y + kRectY) * kColorW + kRectX) * stride,
                                  std::size_t(kNativeW) * stride, half, nanPayloads))
            return false;
    return true;
}

void TestRoutes(Gpu &gpu, const ofps::core::gpu::Shaders &shaders)
{
    struct Case { DXGI_FORMAT resource, view; std::uint32_t stride; bool half; };
    const Case admitted[] = {{DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT, 8, true},
                             {DXGI_FORMAT_R16G16B16A16_TYPELESS, DXGI_FORMAT_R16G16B16A16_FLOAT, 8, true},
                             {DXGI_FORMAT_R16G16B16A16_UNORM, DXGI_FORMAT_R16G16B16A16_UNORM, 8, false},
                             {DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_R10G10B10A2_UNORM, 4, false},
                             {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM, 4, false},
                             {DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM, 4, false},
                             {DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM, 4, false}};
    for (const Case &c : admitted) {
        const Route r = Snapshot(gpu, shaders, c.resource, c.view, c.view, 0.0f, c.stride, c.half);
        if (r.raw.empty()) continue;
        const std::string name = Name(c.resource) + "/" + Name(c.view);
        std::size_t nanPayloads = 0;
        Check(r.taken == RawRoute::Copy, "route " + name + ": RecordRaw takes the copy");
        Check(r.raw.size() == r.pass.size() && SameExceptNaNPayload(r.raw.data(), r.pass.data(), r.raw.size(), c.half, nanPayloads),
              "route " + name + ": the copy leaves every byte the debugVis-3 reprojection leaves, NaN payload bits aside");
        Check(PassHoldsColour(r, c.stride, c.half), "route " + name + ": the reprojection itself wrote the colour's own bytes");
        if (c.half) std::cout << "  route " << name << ": " << nanPayloads << " NaN channels with another payload than the copy's\n";
    }
    // Controls: RecordRaw keeps the pass, and there a copy would have written other bytes.
    const Route srgb = Snapshot(gpu, shaders, DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
                                DXGI_FORMAT_R8G8B8A8_UNORM, 0.0f, 4, false);
    Check(srgb.taken == RawRoute::Pass && !srgb.raw.empty() && srgb.raw == srgb.pass && !PassHoldsColour(srgb, 4, false),
          "control: an sRGB view keeps the reprojection, which decodes (a copy would not)");
    const Route ratio = Snapshot(gpu, shaders, DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT,
                                 DXGI_FORMAT_R16G16B16A16_FLOAT, 0.01f, 8, true);
    Check(ratio.taken == RawRoute::Pass && !ratio.raw.empty() && ratio.raw == ratio.pass && !PassHoldsColour(ratio, 8, true),
          "control: a ratio-domain host keeps the reprojection, which stores log(1 + c / s) (a copy would not)");
}

// With the descriptors run out the pass writes nothing, and so does the copy: RecordRaw reports neither. A use point no
// fence ever reaches makes the 65th debugVis-3 reprojection find no free fallback table (after a 100 ms wait). The list
// is closed unexecuted.
void TestExhaustedRoute(Gpu &gpu, const ofps::core::gpu::Shaders &shaders)
{
    auto color = gpu.Texture(DXGI_FORMAT_R16G16B16A16_FLOAT, kColorW, kColorH), target = gpu.Texture(DXGI_FORMAT_R16G16B16A16_FLOAT, kTargetW, kTargetH);
    auto depth = gpu.Texture(DXGI_FORMAT_R32_FLOAT, kNativeW, kNativeH), motion = gpu.Texture(DXGI_FORMAT_R16G16_FLOAT, kNativeW, kNativeH);
    ComPtr<ID3D12Fence> never;
    Machine machine;
    char error[256]{};
    if (!color || !target || !depth || !motion || FAILED(gpu.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&never))) ||
        !machine.Initialize(gpu.device.Get(), shaders, kNativeW, kNativeH, DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT,
                            kNativeW, kNativeH, DXGI_FORMAT_R32_FLOAT, kNativeW, kNativeH, error, sizeof(error)) || !gpu.Begin()) {
        Check(false, std::string("exhausted: the machine and its inputs (") + error + ")");
        return;
    }
    OfpsFencePoint point{};
    point.size = sizeof(point);
    point.fence = never.Get();
    point.value = 1;
    machine.SetUsePoint(point, 0);
    FrameInputs in{};
    in.color = color.Get(); in.depth = depth.Get(); in.motion = motion.Get();
    in.colorView = DXGI_FORMAT_R16G16B16A16_FLOAT; in.depthView = DXGI_FORMAT_R32_FLOAT; in.motionView = DXGI_FORMAT_R16G16_FLOAT;
    in.colorRect = {kRectX, kRectY, kNativeW, kNativeH};
    in.depthRect = in.motionRect = {0, 0, kNativeW, kNativeH};
    in.hostInputState = in.depthState = in.motionState = kRead;
    FrameInputs vis = in;
    vis.debugVis = 3;
    for (int i = 0; i < 65; ++i) machine.RecordReproject(gpu.list.Get(), vis, nullptr, D3D12_RESOURCE_STATE_COMMON, 0, 0);
    Check(machine.RecordRaw(gpu.list.Get(), in, target.Get(), D3D12_RESOURCE_STATE_COMMON, kDstX, kDstY) == RawRoute::None,
          "exhausted: RecordRaw reports no route when the copy is suppressed");
    Check(machine.TakeExhausted(), "exhausted: the fallback tables ran out");
    gpu.list->Close();
}

void RunDevice(const char *shaderDirectory)
{
    Gpu gpu;
    Check(gpu.Create(), "a D3D12 WARP device, queue and list");
    ofps::core::gpu::Shaders shaders;
    Check(temporal_d3d12::LoadShaders(shaderDirectory, shaders, false), "the temporal shaders load");
    if (failures) return;
    TestRoutes(gpu, shaders);
    TestExhaustedRoute(gpu, shaders);
    for (const std::string &error : gpu.DebugErrors()) Check(false, "D3D12 debug layer: " + error);
}
} // namespace

int main(int argc, char **argv)
{
    TestRule();
    if (argc > 1) RunDevice(argv[1]);
    if (failures == 0) std::cout << "PASS: temporal raw snapshot\n";
    return failures == 0 ? 0 : 1;
}
