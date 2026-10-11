// DebugTemporalCarryProbe (id 52, int 0..7) and DebugTemporalDumpAccFrom (id 53, int 0..1000000): read-only diagnostic
// ints of the Diagnostics group for the bench attribution of the carry under camera motion (phase 3 step 0).
//   - the rows, their ini keys and the core's clamp (9 -> 7);
//   - core/temporal/carry_probe.h: the oracle chain only in temporal mode 1, the other bits and the dump window;
//   - D3D12 WARP with the debug layer: two frames of uniform motion (3.5, -2) then (7, -4) on a 64x36 motion texture over
//     a static scene. With the oracle the chain is the second frame's vector (acc = mv), also x mvScale x motion sign
//     (mvScaleX 2, sign -1: (-14, 4)); without it the chained sum.
//   - bit 1 (accept all) on WARP: a disocclusion (the residual's frame at depth 0.2 on the left half, 0.8 on the right;
//     now 0.8 everywhere; chain 0; residual 0.5) is colour + 0.5 everywhere with it, the left half keeps the colour
//     without it; a pixel whose source is off screen (chain (-40, 0)) is still rejected with it.
// Usage: ofps_temporal_carry_probe_tests <shader dir>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "temporal_background_driver.h"
#include "temporal_chain_fixture.h"
#include "core/temporal/carry_probe.h"

#include <dxgi1_4.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <vector>

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

static_assert(!OracleChainActive(0, kProbeOracle) && !OracleChainActive(3, kProbeOracle) && OracleChainActive(1, kProbeOracle));
static_assert(!OracleChainActive(1, kProbeAcceptAll | kProbeBilinear) && OracleChainActive(1, 7));

void TestSettings()
{
    using ofps::core::DiagnosticsConfig;
    Check(DiagnosticIntRow(OFPS_SET_DEBUG_TEMPORAL_CARRY_PROBE, "DebugTemporalCarryProbe", 7.0f),
          "DebugTemporalCarryProbe is a read-only diagnostic int 0..7, 0 by default");
    Check(DiagnosticIntRow(OFPS_SET_DEBUG_TEMPORAL_DUMP_ACC_FROM, "DebugTemporalDumpAccFrom", 1000000.0f),
          "DebugTemporalDumpAccFrom is a read-only diagnostic int 0..1000000, 0 by default");
    Check(LoadedDiagnostic("DebugTemporalCarryProbe", 5, &DiagnosticsConfig::temporalCarryProbe) == 5 &&
              LoadedDiagnostic("DebugTemporalCarryProbe", 9, &DiagnosticsConfig::temporalCarryProbe) == 7 &&
              LoadedDiagnostic("DebugTemporalCarryProbe", -3, &DiagnosticsConfig::temporalCarryProbe) == 0,
          "DebugTemporalCarryProbe reaches the core from the ini, clamped to 0..7");
    Check(LoadedDiagnostic("DebugTemporalDumpAccFrom", 600, &DiagnosticsConfig::temporalDumpAccFrom) == 600 &&
              LoadedDiagnostic("DebugTemporalDumpAccFrom", 2000000, &DiagnosticsConfig::temporalDumpAccFrom) == 1000000,
          "DebugTemporalDumpAccFrom reaches the core from the ini, clamped to 0..1000000");
}

void TestApply()
{
    FrameInputs t;
    ApplyCarryProbe(t, 1, 0, 0);
    Check(!t.oracleChain && !t.acceptAll && t.refine && t.expectedDepth && t.residualCatmullRom && t.dumpAccFrom == 0,
          "probe 0 leaves the inputs as they were");
    ApplyCarryProbe(t, 1, kProbeOracle, 0);
    Check(t.oracleChain && !t.refine && !t.expectedDepth && t.residualCatmullRom, "the oracle chain skips Refine and the expectation");
    FrameInputs b;
    ApplyCarryProbe(b, 3, 7, -5);
    Check(!b.oracleChain && b.refine && b.expectedDepth && b.acceptAll && !b.residualCatmullRom && b.dumpAccFrom == 0,
          "mode 3 ignores the oracle; bits 1 and 2 still read; a negative window is 0");
    ApplyCarryProbe(b, 0, kProbeOracle, 640);
    Check(!b.oracleChain && b.dumpAccFrom == 640, "mode 0 ignores the oracle; the dump window passes through");
    FrameInputs n;
    ApplyCarryProbe(n, kModeNoProbe, 7, 640);
    Check(!n.oracleChain && !n.acceptAll && n.refine && n.expectedDepth && n.residualCatmullRom && n.dumpAccFrom == 0,
          "kModeNoProbe (fallback, spread) leaves the inputs as they were");
}

// Every texel of a read-back chain equals (x, y) within 1e-3.
bool Uniform(const unsigned char *data, std::uint32_t slot, float x, float y)
{
    for (std::uint32_t t = 0; t < kMw * kMh; ++t) {
        float ax, ay;
        AccAt(data, slot, t % kMw, t / kMw, ax, ay);
        if (std::fabs(ax - x) > 1e-3f || std::fabs(ay - y) > 1e-3f) {
            std::fprintf(stderr, "  texel (%u, %u): (%g, %g), expected (%g, %g)\n", t % kMw, t / kMw, ax, ay, x, y);
            return false;
        }
    }
    return true;
}

// A kW x kH texture of `left` on x < kW/2 and `right` beyond, resting readable.
ComPtr<ID3D12Resource> Halves(ID3D12Device *device, Recording &r, DXGI_FORMAT format, const float (&left)[4], const float (&right)[4])
{
    const ComPtr<ID3D12Resource> texture = r.Paint(device, format, right, right, false);
    if (!texture) return nullptr;
    auto rtv = r.heap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(r.painted - 1) * device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    const D3D12_RECT half{0, 0, static_cast<LONG>(kW / 2), static_cast<LONG>(kH)};
    BarrierExternal(r.list.Get(), texture.Get(), kRead, D3D12_RESOURCE_STATE_RENDER_TARGET);
    r.list->ClearRenderTargetView(rtv, left, 1, &half);
    BarrierExternal(r.list.Get(), texture.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, kRead);
    return texture;
}

// The largest |red - want| over the columns [x0, x1) of a picture.
float Deviation(const Picture &picture, std::uint32_t x0, std::uint32_t x1, float want)
{
    float worst = 0.0f;
    for (std::uint32_t y = 0; y < kH; ++y)
        for (std::uint32_t x = x0; x < x1; ++x) worst = std::max(worst, std::fabs(picture[(static_cast<std::size_t>(y) * kW + x) * 4] - want));
    return worst;
}

int TestAcceptAll(ID3D12Device *device, ID3D12CommandQueue *queue, const char *shaderDirectory)
{
    ofps::core::gpu::Shaders shaders;
    Recording r;
    Check(LoadShaders(shaderDirectory, shaders, true) && LoadSkyShaders(shaderDirectory, shaders) && r.Create(device),
          "accept all: the shaders load and the fixtures allocate");
    if (failures) return failures;
    const float grey[4] = {0.25f, 0.25f, 0.25f, 1.0f}, answer[4] = {0.75f, 0.75f, 0.75f, 1.0f}; // residual 0.5
    const float closer[4] = {0.2f, 0, 0, 0}, farther[4] = {0.8f, 0, 0, 0}, still[4] = {0, 0, 0, 0}, left[4] = {-40.0f, 0, 0, 0};
    const ComPtr<ID3D12Resource> color = r.Paint(device, kColor, grey, grey, false), fresh = r.Paint(device, kColor, answer, answer, false);
    const ComPtr<ID3D12Resource> then = Halves(device, r, DXGI_FORMAT_R32_FLOAT, closer, farther);
    const ComPtr<ID3D12Resource> now = r.Paint(device, DXGI_FORMAT_R32_FLOAT, farther, farther, false);
    const ComPtr<ID3D12Resource> motion[2] = {r.Paint(device, DXGI_FORMAT_R16G16_FLOAT, still, still, false),
                                              r.Paint(device, DXGI_FORMAT_R16G16_FLOAT, left, left, false)};
    Check(color && fresh && then && now && motion[0] && motion[1], "accept all: the scene paints");
    if (failures) return failures;
    // [0] the disocclusion with accept all, [1] without, [2] the chain (-40, 0) with accept all.
    std::vector<ComPtr<ID3D12Resource>> outputs(3);
    Machine machines[3];
    for (std::uint32_t i = 0; i < 3; ++i) {
        char error[256]{};
        outputs[i] = Texture(device, kColor, D3D12_RESOURCE_FLAG_NONE);
        Check(outputs[i] && machines[i].Initialize(device, shaders, kW, kH, kColor, kColor, kW, kH, DXGI_FORMAT_R32_FLOAT, kW, kH,
                                                   error, sizeof(error)),
              "accept all: the machine initializes");
        if (!machines[i].Ready()) { std::cerr << error << '\n'; return failures; }
        machines[i].SetUsePoint(r.Point(), 0);
        FrameInputs in{};
        in.color = color.Get(); in.depth = i < 2 ? then.Get() : now.Get(); in.motion = motion[i < 2 ? 0 : 1].Get();
        in.colorView = kColor; in.depthView = DXGI_FORMAT_R32_FLOAT; in.motionView = DXGI_FORMAT_R16G16_FLOAT;
        in.colorRect = in.depthRect = in.motionRect = {0, 0, kW, kH};
        in.hostInputState = in.depthState = in.motionState = kRead;
        // The plain reprojection: no fill, compose, history, registration, expectation or sky to soften the decision.
        in.holeFill = in.compose = in.history = in.refine = in.expectedDepth = in.skyField = false;
        ApplyCarryProbe(in, 1, i == 1 ? 0 : kProbeAcceptAll, 0);
        Check(in.acceptAll == (i != 1), "accept all: bit 1 sets the input");
        machines[i].RecordResidual(r.list.Get(), in, fresh.Get(), kRead);
        in.depth = now.Get();
        machines[i].RecordAccumulate(r.list.Get(), in);
        machines[i].RecordReproject(r.list.Get(), in, outputs[i].Get(), D3D12_RESOURCE_STATE_COMMON, 0, 0);
        Check(!machines[i].TakeExhausted(), "accept all: the sequence records within the descriptor ring");
    }
    const std::vector<Picture> pictures = Execute(device, queue, r, outputs);
    Check(pictures.size() == 3, "accept all: the GPU completes the passes and the frames read back");
    if (pictures.size() != 3) return failures;
    const float all = Deviation(pictures[0], 0, kW, 0.75f), off = Deviation(pictures[1], 0, kW / 2 - 8, 0.25f);
    const float rejected = Deviation(pictures[2], 0, 40, 0.25f), accepted = Deviation(pictures[2], 40, kW, 0.75f);
    std::printf("accept all: disocclusion colour + 0.5 within %.3g (off: the left half keeps the colour within %.3g); "
                "off-screen source keeps the colour within %.3g, the rest colour + 0.5 within %.3g\n", all, off, rejected, accepted);
    Check(all <= 1e-3f, "accept all: the disoccluded left half takes the residual (colour + 0.5)");
    Check(off <= 1e-3f, "accept all: without bit 1 the disoccluded left half keeps the colour");
    Check(rejected <= 1e-3f && accepted <= 1e-3f, "accept all: a pixel whose source is off screen is still rejected");
    return failures;
}

int TestOracle(ID3D12Device *device, ID3D12CommandQueue *queue, const char *shaderDirectory)
{
    ofps::core::gpu::Shaders shaders;
    Check(LoadShaders(shaderDirectory, shaders, true) && LoadSkyShaders(shaderDirectory, shaders), "carry probe: the shaders load");
    Recording r;
    Check(r.Create(device), "carry probe: fixtures allocate");
    if (failures) return failures;
    const float grey[4] = {0.375f, 0.5f, 0.625f, 1.0f}, answer[4] = {0.5f, 0.4375f, 0.625f, 1.0f}, depthValue[4] = {0.5f, 0, 0, 0};
    const ComPtr<ID3D12Resource> color = r.Paint(device, kColor, grey, {}, false);
    const ComPtr<ID3D12Resource> depth = r.Paint(device, DXGI_FORMAT_R32_FLOAT, depthValue, {}, false); // static depth
    const ComPtr<ID3D12Resource> fresh = r.Paint(device, kColor, answer, {}, false);
    const ComPtr<ID3D12Resource> motion[2] = {Motion(device, r, 3.5f, -2.0f), Motion(device, r, 7.0f, -4.0f)};
    const ComPtr<ID3D12Resource> readback = AccReadback(device, 3);
    Check(color && depth && fresh && motion[0] && motion[1] && readback, "carry probe: the scene paints");
    if (failures) return failures;

    Machine machines[3]; // [0] oracle chain, [1] today's chain, [2] oracle chain with mvScaleX 2 and motion sign -1
    for (std::uint32_t i = 0; i < 3; ++i) {
        char error[256]{};
        Check(machines[i].Initialize(device, shaders, kW, kH, kColor, kColor, kMw, kMh, DXGI_FORMAT_R32_FLOAT, kW, kH, error,
                                     sizeof(error)),
              "carry probe: the machine initializes");
        if (!machines[i].Ready()) { std::cerr << error << '\n'; return failures; }
        machines[i].SetUsePoint(r.Point(), 0);
        FrameInputs in{};
        in.color = color.Get(); in.depth = depth.Get();
        in.colorView = kColor; in.depthView = DXGI_FORMAT_R32_FLOAT; in.motionView = DXGI_FORMAT_R16G16_FLOAT;
        in.colorRect = in.depthRect = {0, 0, kW, kH};
        in.motionRect = {0, 0, kMw, kMh};
        in.hostInputState = in.depthState = in.motionState = kRead;
        in.refine = false; // the chain's sum stays exact without the registration
        if (i == 2) { in.mvScaleX = 2.0f; in.motionSign = -1.0f; }
        if (i != 1) ApplyCarryProbe(in, 1, kProbeOracle, 0);
        Check(in.oracleChain == (i != 1), "carry probe: the oracle is on in mode 1 only when asked");
        in.motion = motion[0].Get();
        machines[i].RecordResidual(r.list.Get(), in, fresh.Get(), kRead);
        machines[i].RecordAccumulate(r.list.Get(), in);
        in.motion = motion[1].Get();
        machines[i].RecordAccumulate(r.list.Get(), in);
        Check(!machines[i].TakeExhausted(), "carry probe: the sequence records within the descriptor ring");
        ReadAcc(r.list.Get(), machines[i].Acc(), readback.Get(), i);
    }
    const unsigned char *bytes = RunAndMap(device, queue, r, readback.Get(), 3);
    Check(bytes != nullptr, "carry probe: the GPU completes the passes and the chains read back");
    if (failures) return failures;
    Check(Uniform(bytes, 0, 7.0f, -4.0f), "carry probe: the oracle chain is the second frame's vector (x mvScale x sign), not the sum");
    Check(Uniform(bytes, 1, 10.5f, -6.0f), "carry probe: without the oracle the chain is the sum (10.5, -6) on a static depth");
    Check(Uniform(bytes, 2, -14.0f, 4.0f), "carry probe: the oracle chain is scaled by mvScale and the motion sign ((7, -4) -> (-14, 4))");
    const D3D12_RANGE none{0, 0};
    readback->Unmap(0, &none);
    return failures;
}
} // namespace

int main(int argc, char **argv)
{
    if (argc != 2) {
        std::cerr << "usage: ofps_temporal_carry_probe_tests <shader dir>\n";
        return 1;
    }
    TestSettings();
    TestApply();
    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter> warp;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    D3D12_COMMAND_QUEUE_DESC desc{};
    desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || FAILED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))) ||
        FAILED(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))) ||
        FAILED(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue)))) {
        std::cerr << "FAIL: a D3D12 WARP device and queue\n";
        return 1;
    }
    int result = TestOracle(device.Get(), queue.Get(), argv[1]);
    result += TestAcceptAll(device.Get(), queue.Get(), argv[1]);
    ComPtr<ID3D12InfoQueue> info;
    if (debug && SUCCEEDED(device.As(&info))) {
        for (UINT64 i = 0, n = info->GetNumStoredMessages(); i < n; ++i) {
            SIZE_T size = 0;
            if (FAILED(info->GetMessage(i, nullptr, &size))) continue;
            std::vector<char> storage(size);
            auto *message = reinterpret_cast<D3D12_MESSAGE *>(storage.data());
            if (SUCCEEDED(info->GetMessage(i, message, &size)) && message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) {
                std::cerr << "FAIL: D3D12 debug layer: " << message->pDescription << '\n';
                ++result;
            }
        }
    }
    if (result == 0) std::cout << "PASS: DebugTemporalCarryProbe rows, gating, the oracle chain and accept all on WARP\n";
    return result == 0 ? 0 : 1;
}
