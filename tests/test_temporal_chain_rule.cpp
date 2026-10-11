// DebugTemporalChainRule (id 54, int 0..4): when the carry's main chain starts afresh (PSAccumulate's link test, phase 3
// step 1 of the camera-motion carry), and DebugTemporalCarryChainRule (id 55, int 0..6, steps 1b/1c): a second chain by
// that rule for the reprojection alone (core/temporal/carry_chain.cpp; both chains in one pass each since step 1c, rules
// 5/6 and the merge's byte identity in test_temporal_carry_rules.cpp).
//   - the rows, their ini keys and the core's clamp (9 -> 4, 9 -> 6);
//   - D3D12 WARP with the debug layer: 64x36 motion texels over a 256x128 frame, a static camera and the chain (2, 0) per
//     frame, two frames after a full pass (the second link is tested; colour tolerance 0.04, depth tolerance w = 0.1).
//     The residual's frame, at the chains' ends: the left half is a swaying blade (25 % brighter, depth mismatch 0.5 w);
//     D an 8x8 disocclusion (mismatch 3 w); I an isolated texel and B an 8x8 block (brighter, mismatch 1.5 w).
//     The chain's x after the second frame (2 = restarted, 4 = inherited):
//                     blade  D  I  B
//       rule 0          2    2  2  2
//       rule 1          4    4  4  4
//       rule 2          4    2  2  2
//       rule 3          4    2  4  2    (5x5 vote of rule 2's LinkFail mask)
//       rule 4          4    2  4  4
//     Rule 3 without the LinkFail binary runs as rule 2; the oracle chain (DebugTemporalCarryProbe 1) over rule 2 is
//     (2, 0) everywhere. The rest of the right half inherits (4, 0) under every rule.
//   - the carry chain: without a carry rule CarryAcc() is Acc(); with one, Acc() keeps the main rule's chain and
//     CarryAcc() follows the carry rule's row above (both chains (2, 0) under the oracle). The model's vectors
//     (ModelMotion, a full pass after the carried frames) are byte-identical to the run without a carry chain, and a
//     carried frame's reprojection (compose route) to the run whose main chain has the carry's rule. Also: rule 0 creates
//     no carry chain; a full pass (RecordResidual) drops it (CarryAcc() is Acc() again); a carry rule turned on after the
//     first frame continues the main chain's value (its link tested by the carry rule, not restarted); and with Refine and
//     the expectation on (the shipped configuration) the carry chain is byte for byte the main chain of a run with its rule.
// Usage: ofps_temporal_chain_rule_tests <shader dir>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "temporal_chain_fixture.h"
#include "core/temporal/carry_probe.h"
#include "core/temporal/machine.h"

#include <dxgi1_4.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>
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

void TestSettings()
{
    using ofps::core::DiagnosticsConfig;
    Check(DiagnosticIntRow(OFPS_SET_DEBUG_TEMPORAL_CHAIN_RULE, "DebugTemporalChainRule", 4.0f),
          "DebugTemporalChainRule is a read-only diagnostic int 0..4, 0 by default");
    Check(LoadedDiagnostic("DebugTemporalChainRule", 3, &DiagnosticsConfig::temporalChainRule) == 3 &&
              LoadedDiagnostic("DebugTemporalChainRule", 9, &DiagnosticsConfig::temporalChainRule) == 4 &&
              LoadedDiagnostic("DebugTemporalChainRule", -3, &DiagnosticsConfig::temporalChainRule) == 0,
          "DebugTemporalChainRule reaches the core from the ini, clamped to 0..4");
    Check(DiagnosticIntRow(OFPS_SET_DEBUG_TEMPORAL_CARRY_CHAIN_RULE, "DebugTemporalCarryChainRule", 6.0f),
          "DebugTemporalCarryChainRule is a read-only diagnostic int 0..6, 0 by default");
    Check(LoadedDiagnostic("DebugTemporalCarryChainRule", 2, &DiagnosticsConfig::temporalCarryChainRule) == 2 &&
              LoadedDiagnostic("DebugTemporalCarryChainRule", 6, &DiagnosticsConfig::temporalCarryChainRule) == 6 &&
              LoadedDiagnostic("DebugTemporalCarryChainRule", 9, &DiagnosticsConfig::temporalCarryChainRule) == 6 &&
              LoadedDiagnostic("DebugTemporalCarryChainRule", -3, &DiagnosticsConfig::temporalCarryChainRule) == 0,
          "DebugTemporalCarryChainRule reaches the core from the ini, clamped to 0..6");
}

struct Patch {
    D3D12_RECT rect;
    float value;
};

// A kW x kH texture of `base` with `patches` cleared over it (grey for colour formats), resting readable.
ComPtr<ID3D12Resource> Painted(ID3D12Device *device, Recording &r, DXGI_FORMAT format, float base, std::initializer_list<Patch> patches)
{
    const float b[4] = {base, base, base, 1.0f};
    const ComPtr<ID3D12Resource> texture = r.Paint(device, format, b, b, false);
    if (!texture) return nullptr;
    auto rtv = r.heap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(r.painted - 1) * device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    BarrierExternal(r.list.Get(), texture.Get(), kRead, D3D12_RESOURCE_STATE_RENDER_TARGET);
    for (const Patch &p : patches) {
        const float v[4] = {p.value, p.value, p.value, 1.0f};
        r.list->ClearRenderTargetView(rtv, v, 1, &p.rect);
    }
    BarrierExternal(r.list.Get(), texture.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, kRead);
    return texture;
}

// The native pixels the tests of motion texels [x0, x1] x [y0, y1] read in the residual's frame after two frames: the
// chain's end is (4x + 18, (y + 0.5) * 128 / 36), the depth test takes the best of the four texels around it, the
// colour test the texel under it. Those of the neighbours outside the range stay out.
D3D12_RECT Ends(int x0, int y0, int x1, int y1)
{
    const auto row = [](int y, float offset) {
        return static_cast<LONG>(std::floor((static_cast<float>(y) + 0.5f) * static_cast<float>(kH) / kMh + offset));
    };
    return {4 * x0 + 16, row(y0, -0.5f), 4 * x1 + 20, row(y1, 0.5f) + 1};
}

enum Region { kBlade, kD, kI, kB, kRest };

Region RegionOf(std::uint32_t x, std::uint32_t y)
{
    if (x < kMw / 2) return kBlade;
    if (x >= 36 && x <= 43 && y >= 2 && y <= 9) return kD;
    if (x == 50 && y == 20) return kI;
    if (x >= 36 && x <= 43 && y >= 22 && y <= 29) return kB;
    return kRest;
}

struct Run {
    const char *name;
    int rule;
    bool linkFail, oracle;
    float x[5]; // the chain's x per Region
    int carry = 0;
    float carryX[5] = {}; // the carry chain's x per Region (with a carry rule; CarryAcc() is Acc() without one)
    bool live = false;    // Refine and the expectation on: no expected rows, the chains are compared byte for byte
    bool late = false;    // the carry rule only from the second frame (the first frame accumulates without it)
};

constexpr Run kRuns[] = {
    {"rule 0", 0, true, false, {2, 2, 2, 2, 4}},
    {"rule 1", 1, true, false, {4, 4, 4, 4, 4}},
    {"rule 2", 2, true, false, {4, 2, 2, 2, 4}},
    {"rule 3", 3, true, false, {4, 2, 4, 2, 4}},
    {"rule 4", 4, true, false, {4, 2, 4, 4, 4}},
    {"rule 3 without LinkFail", 3, false, false, {4, 2, 2, 2, 4}},
    {"rule 2 under the oracle", 2, true, true, {2, 2, 2, 2, 2}},
    {"rule 0, carry 2", 0, true, false, {2, 2, 2, 2, 4}, 2, {4, 2, 2, 2, 4}},
    {"rule 0, carry 3", 0, true, false, {2, 2, 2, 2, 4}, 3, {4, 2, 4, 2, 4}},
    {"rule 3, carry 2", 3, true, false, {4, 2, 4, 2, 4}, 2, {4, 2, 2, 2, 4}},
    {"rule 0, carry 2 under the oracle", 0, true, true, {2, 2, 2, 2, 2}, 2, {2, 2, 2, 2, 2}},
    {"rule 3, carry 3", 3, true, false, {4, 2, 4, 2, 4}, 3, {4, 2, 4, 2, 4}},
    {"rule 0, carry 2 from the second frame", 0, true, false, {2, 2, 2, 2, 4}, 2, {4, 2, 2, 2, 4}, false, true},
    {"live: rule 0", 0, true, false, {}, 0, {}, true},
    {"live: rule 2", 2, true, false, {}, 0, {}, true},
    {"live: rule 0, carry 2", 0, true, false, {}, 2, {}, true},
};
constexpr std::uint32_t kRunCount = sizeof(kRuns) / sizeof(kRuns[0]);
constexpr std::uint32_t kRule0 = 0, kRule2 = 2, kCarry2 = 7; // the runs whose model vectors and frames are compared
constexpr std::uint32_t kLive0 = 13, kLive2 = 14, kLiveCarry2 = 15;

// A carried frame's output (kColor, kW x kH, resting in COMMON) into slot `slot` of a picture readback buffer.
constexpr std::uint32_t kPicturePitch = kW * 8, kPictureSlot = kPicturePitch * kH;
void ReadPicture(ID3D12GraphicsCommandList *list, ID3D12Resource *picture, ID3D12Resource *readback, std::uint32_t slot)
{
    BarrierExternal(list, picture, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
    dst.pResource = readback;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Offset = static_cast<UINT64>(slot) * kPictureSlot;
    dst.PlacedFootprint.Footprint = {kColor, kW, kH, 1, kPicturePitch};
    src.pResource = picture;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    BarrierExternal(list, picture, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
}

// The texels of chain `slot` that differ from `want` (x per Region, y 0); slot k of the chain readback is run k's Acc(),
// kRunCount + k its CarryAcc(), 2 kRunCount + k its ModelMv().
std::uint32_t Wrong(const unsigned char *bytes, std::uint32_t slot, const char *name, const float (&want)[5])
{
    std::uint32_t wrong = 0;
    for (std::uint32_t y = 0; y < kMh; ++y)
        for (std::uint32_t x = 0; x < kMw; ++x) {
            float ax, ay;
            AccAt(bytes, slot, x, y, ax, ay);
            const float w = want[RegionOf(x, y)];
            if (std::fabs(ax - w) <= 1e-3f && std::fabs(ay) <= 1e-3f) continue;
            if (wrong++ < 4) std::fprintf(stderr, "  %s, texel (%u, %u): (%g, %g), expected (%g, 0)\n", name, x, y, ax, ay, w);
        }
    return wrong;
}

int TestRules(ID3D12Device *device, ID3D12CommandQueue *queue, const char *shaderDirectory)
{
    ofps::core::gpu::Shaders shaders[2]; // [0] without LinkFail (and without a carry chain), [1] with it and the pair passes
    for (auto &s : shaders) Check(LoadShaders(shaderDirectory, s, true), "chain rule: the shaders load");
    Check(LoadChainPasses(shaderDirectory, shaders[1]), "chain rule: temporal_LinkFail_cs.dxbc and the pair passes load");
    Recording r;
    Check(r.Create(device), "chain rule: the fixtures allocate");
    if (failures) return failures;
    const D3D12_RECT blade{16, 0, 144, static_cast<LONG>(kH)}, d = Ends(36, 2, 43, 9), i = Ends(50, 20, 50, 20), b = Ends(36, 22, 43, 29);
    const ComPtr<ID3D12Resource> colourThen = Painted(device, r, kColor, 0.5f, {{blade, 0.625f}, {i, 0.625f}, {b, 0.625f}});
    const ComPtr<ID3D12Resource> depthThen =
        Painted(device, r, DXGI_FORMAT_R32_FLOAT, 0.5f, {{blade, 0.475f}, {d, 0.35f}, {i, 0.425f}, {b, 0.425f}});
    const ComPtr<ID3D12Resource> colourNow = Painted(device, r, kColor, 0.5f, {}), depthNow = Painted(device, r, DXGI_FORMAT_R32_FLOAT, 0.5f, {});
    const ComPtr<ID3D12Resource> fresh = Painted(device, r, kColor, 0.75f, {}), motion = Motion(device, r, 2.0f, 0.0f);
    const ComPtr<ID3D12Resource> readback = AccReadback(device, 3 * kRunCount);
    const ComPtr<ID3D12Resource> pictures = AccReadback(device, (3 * kPictureSlot + kSlot - 1) / kSlot); // kRule0, kRule2, kCarry2
    Check(colourThen && depthThen && colourNow && depthNow && fresh && motion && readback && pictures, "chain rule: the scene paints");
    if (failures) return failures;

    Machine machines[kRunCount];
    std::vector<ComPtr<ID3D12Resource>> outputs; // alive until the list has run
    for (std::uint32_t k = 0; k < kRunCount; ++k) {
        const Run &run = kRuns[k];
        char error[256]{};
        Check(machines[k].Initialize(device, shaders[run.linkFail ? 1 : 0], kW, kH, kColor, kColor, kMw, kMh, DXGI_FORMAT_R32_FLOAT,
                                     kW, kH, error, sizeof(error)),
              "chain rule: the machine initializes");
        if (!machines[k].Ready()) { std::cerr << error << '\n'; return failures; }
        machines[k].SetUsePoint(r.Point(), 0);
        FrameInputs in{};
        in.color = colourThen.Get(); in.depth = depthThen.Get(); in.motion = motion.Get();
        in.colorView = kColor; in.depthView = DXGI_FORMAT_R32_FLOAT; in.motionView = DXGI_FORMAT_R16G16_FLOAT;
        in.colorRect = in.depthRect = {0, 0, kW, kH};
        in.motionRect = {0, 0, kMw, kMh};
        in.hostInputState = in.depthState = in.motionState = kRead;
        in.refine = in.expectedDepth = run.live; // off: the chain's sum stays exact; the tests compare this frame's depth
        in.colorTolerance = 0.04f;
        in.depthThreshold = 0.1f;
        in.chainRule = run.rule;
        in.carryChainRule = run.late ? 0 : run.carry;
        if (run.oracle) ApplyCarryProbe(in, 1, kProbeOracle, 0);
        machines[k].RecordResidual(r.list.Get(), in, fresh.Get(), kRead); // the residual's frame: its colour and depth
        in.color = colourNow.Get(); in.depth = depthNow.Get();
        machines[k].RecordAccumulate(r.list.Get(), in); // acc = mv
        in.carryChainRule = run.carry;
        machines[k].RecordAccumulate(r.list.Get(), in); // acc = mv + accPrev(p + mv), the link tested
        Check(machines[k].CarryChainCreated() == (run.carry != 0), "carry chain: its textures exist exactly with a carry rule");
        const std::uint32_t shown = k == kRule0 ? 0u : k == kRule2 ? 1u : k == kCarry2 ? 2u : 3u;
        if (shown < 3) { // a carried frame, then the model's vectors as a full pass would take them
            const ComPtr<ID3D12Resource> output = Texture(device, kColor, D3D12_RESOURCE_FLAG_NONE);
            Check(output != nullptr, "carry chain: the output allocates");
            if (!output) return failures;
            machines[k].RecordReproject(r.list.Get(), in, output.Get(), D3D12_RESOURCE_STATE_COMMON, 0, 0);
            machines[k].RecordModelMotion(r.list.Get(), in);
            ReadPicture(r.list.Get(), output.Get(), pictures.Get(), shown);
            outputs.push_back(output);
            Check(machines[k].ModelMv() != nullptr, "carry chain: the machine has ModelMotion");
            if (machines[k].ModelMv()) ReadAcc(r.list.Get(), machines[k].ModelMv(), readback.Get(), 2 * kRunCount + k);
        }
        Check(!machines[k].TakeExhausted(), "chain rule: the sequence records within the descriptor ring");
        Check((machines[k].CarryAcc() == machines[k].Acc()) == (run.carry == 0), "carry chain: CarryAcc() is Acc() exactly without a carry rule");
        ReadAcc(r.list.Get(), machines[k].Acc(), readback.Get(), k);
        ReadAcc(r.list.Get(), machines[k].CarryAcc(), readback.Get(), kRunCount + k);
        if (run.carry != 0) { // the next full pass drops the carry chain: until it accumulates again, CarryAcc() is Acc()
            in.color = colourThen.Get(); in.depth = depthThen.Get();
            machines[k].RecordResidual(r.list.Get(), in, fresh.Get(), kRead);
            Check(machines[k].CarryAcc() == machines[k].Acc(), "carry chain: a full pass drops it");
        }
    }
    const unsigned char *bytes = RunAndMap(device, queue, r, readback.Get(), 3 * kRunCount);
    Check(bytes != nullptr, "chain rule: the GPU completes the passes and the chains read back");
    if (failures) return failures;
    for (std::uint32_t k = 0; k < kRunCount; ++k) {
        const Run &run = kRuns[k];
        if (run.live) continue;
        const std::uint32_t wrong = Wrong(bytes, k, run.name, run.x);
        const std::uint32_t carryWrong = Wrong(bytes, kRunCount + k, run.name, run.carry ? run.carryX : run.x);
        std::printf("%s: %u / %u of %u texels differ from the expected main / carry chain\n", run.name, wrong, carryWrong, kMw * kMh);
        Check(wrong == 0, "chain rule: the chain restarts where the rule says and nowhere else");
        Check(carryWrong == 0, "carry chain: the carry chain restarts where the carry rule says and nowhere else");
    }
    const auto plane = [&](std::uint32_t slot) { return bytes + static_cast<std::size_t>(slot) * kSlot; };
    Check(std::memcmp(plane(2 * kRunCount + kRule0), plane(2 * kRunCount + kCarry2), kSlot) == 0,
          "carry chain: the model's vectors are those of the run without a carry chain");
    Check(std::memcmp(plane(2 * kRunCount + kRule0), plane(2 * kRunCount + kRule2), kSlot) != 0,
          "carry chain: the model's vectors depend on the main chain (the comparison above can fail)");
    Check(std::memcmp(plane(kLiveCarry2), plane(kLive0), kSlot) == 0, "carry chain, live: the main chain is the run without one");
    Check(std::memcmp(plane(kRunCount + kLiveCarry2), plane(kLive2), kSlot) == 0,
          "carry chain, live: the carry chain is the main chain of the run with its rule (Refine and the expectation on)");
    Check(std::memcmp(plane(kLive2), plane(kLive0), kSlot) != 0, "carry chain, live: the rule matters (the comparison above can fail)");
    const D3D12_RANGE none{0, 0};
    readback->Unmap(0, &none);
    void *data = nullptr;
    const D3D12_RANGE all{0, 3 * static_cast<SIZE_T>(kPictureSlot)};
    Check(SUCCEEDED(pictures->Map(0, &all, &data)), "carry chain: the carried frames read back");
    if (failures) return failures;
    const auto *frames = static_cast<const unsigned char *>(data);
    Check(std::memcmp(frames + 2 * kPictureSlot, frames + kPictureSlot, kPictureSlot) == 0,
          "carry chain: a carried frame reprojects along the carry chain (as the run whose main chain has its rule)");
    Check(std::memcmp(frames + 2 * kPictureSlot, frames, kPictureSlot) != 0,
          "carry chain: the carried frame differs from the main chain's (the comparison above can fail)");
    pictures->Unmap(0, &none);
    return failures;
}
} // namespace

int main(int argc, char **argv)
{
    if (argc != 2) {
        std::cerr << "usage: ofps_temporal_chain_rule_tests <shader dir>\n";
        return 1;
    }
    TestSettings();
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
    int result = TestRules(device.Get(), queue.Get(), argv[1]); // the settings' failures included
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
    if (result == 0) std::cout << "PASS: DebugTemporalChainRule / DebugTemporalCarryChainRule rows, rules 0-4 and the carry chain on WARP\n";
    return result == 0 ? 0 : 1;
}
