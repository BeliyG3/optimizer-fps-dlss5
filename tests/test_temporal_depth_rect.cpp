// The depth snapshots keep their own sub-rect (2026.10.2): every depth the temporal passes read at t6 (the residual
// frame's snapshot; for PSExpect the previous frame's depth) is read through the sub-rect its own frame described
// (PwTDepthFRect), not through the current frame's. A host with dynamic resolution moves the depth's sub-rect between
// frames; in the background mode the adoption read the previous kick's snapshot through the new kick's sub-rect.
// D3D12 WARP machine sequences, sync (three passes with a phase-in, two carried frames after each) and background
// (temporal_background_driver.h, a kick every 3rd frame). The scene's depth lies in one half of a 2x-wide texture, the
// other half holds another depth. Two pass sets: the current one, and the same source built with
// PW_T_DEPTHF_SHARED_RECT (every depth through this frame's sub-rect: the instructions of 13cc38f, see fix2-report.md).
//   - stable depth metadata (either half throughout): the current passes write the shared-rect passes' bytes;
//   - a moving sub-rect (sync passes alternating halves; background kicks on the left half, then on the right: two
//     kicks of different sub-rects): the current passes write the bytes of the stable sequence frame for frame, the
//     shared-rect passes do not;
//   - a discarded kick (background, the halves alternating per kick, the second kick's result dropped, every kick
//     with a copied chain validating model vectors): the next kick validates against the discarded kick's private
//     depth through that kick's own sub-rect, not the adopted snapshot's; frames and validated vectors are the stable
//     sequence's bit for bit.
// Usage: ofps_temporal_depth_rect_tests <shader dir> <shared-rect pass dir>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "temporal_background_driver.h"

#include <dxgi1_4.h>

#include <cstdio>
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

constexpr std::uint32_t kDepthW = 2 * kW;
constexpr int kOutputs = 11; // sync: 8 (2 carried, then publish + 2 carried twice); background: one per frame
constexpr int kEvery = 3;    // background: kicks on frames 0, 3, 6, 9
constexpr int kKicks = 4, kDiscarded = 1;
const Rect kHalf[2] = {{0, 0, kW, kH}, {kW, 0, kW, kH}};

// The scene's depth (sky 1.0 around a building 0.25) in one half, 0.5 in the other; rests readable.
ComPtr<ID3D12Resource> HalfDepth(ID3D12Device *device, Recording &r, int half)
{
    ComPtr<ID3D12Resource> texture = Plain(device, kDepthW, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    if (!texture || r.painted >= 16) return nullptr;
    auto rtv = r.heap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(r.painted++) * device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    device->CreateRenderTargetView(texture.Get(), nullptr, rtv);
    BarrierExternal(r.list.Get(), texture.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
    const float other[4] = {0.5f, 0, 0, 0}, sky[4] = {1.0f, 0, 0, 0}, building[4] = {0.25f, 0, 0, 0};
    const LONG x = half == 0 ? 0 : LONG(kW);
    const D3D12_RECT scene{x, 0, x + LONG(kW), LONG(kH)}, block{x + 40, 30, x + 140, 90};
    r.list->ClearRenderTargetView(rtv, other, 0, nullptr);
    r.list->ClearRenderTargetView(rtv, sky, 1, &scene);
    r.list->ClearRenderTargetView(rtv, building, 1, &block);
    BarrierExternal(r.list.Get(), texture.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, kRead);
    return texture;
}

struct Scene {
    ComPtr<ID3D12Resource> color, motion, fresh[2], depth[2];
};

enum class Mode { Sync, Background, Discard };
struct RunDesc {
    const ofps::core::gpu::Shaders *shaders;
    Mode mode;
    // The depth's half throughout; -1: moving (sync: pass p on half p % 2; background: half 0, from frame 3 half 1;
    // discard: the halves alternate per kick).
    int half;
};

FrameInputs Inputs(const Scene &scene, int half, bool background)
{
    FrameInputs in{};
    in.color = scene.color.Get(); in.depth = scene.depth[half].Get(); in.motion = scene.motion.Get();
    in.colorView = kColor; in.depthView = DXGI_FORMAT_R32_FLOAT; in.motionView = DXGI_FORMAT_R16G16_FLOAT;
    in.colorRect = in.motionRect = {0, 0, kW, kH};
    in.depthRect = kHalf[half];
    in.hostInputState = in.depthState = in.motionState = kRead;
    in.background = background;
    in.residualBlend = background ? 0.0f : 0.5f;
    in.phaseInFrames = 2;
    return in;
}

void RecordRun(Machine &machine, ID3D12GraphicsCommandList *list, const Scene &scene, const RunDesc &run, Job &job,
               ID3D12Resource *const *outputs)
{
    ID3D12Resource *const fresh[2] = {scene.fresh[0].Get(), scene.fresh[1].Get()};
    if (run.mode != Mode::Sync) {
        for (int frame = 0; frame < kOutputs; ++frame) {
            const int moving = run.mode == Mode::Background ? (frame < kEvery ? 0 : 1) : (frame / kEvery) % 2;
            RecordBackgroundFrame(machine, list, job, Inputs(scene, run.half >= 0 ? run.half : moving, true), fresh, outputs[frame], kEvery);
        }
        return;
    }
    int next = 0;
    for (int pass = 0; pass < 3; ++pass) {
        const FrameInputs in = Inputs(scene, run.half >= 0 ? run.half : pass % 2, false);
        if (pass > 0) machine.RecordAccumulate(list, in);
        machine.RecordResidual(list, in, fresh[pass % 2], kRead);
        if (pass > 0) machine.RecordApply(list, in, outputs[next++], D3D12_RESOURCE_STATE_COMMON, 0, 0);
        for (int carried = 0; carried < 2; ++carried) {
            machine.RecordAccumulate(list, in);
            machine.RecordReproject(list, in, outputs[next++], D3D12_RESOURCE_STATE_COMMON, 0, 0);
        }
    }
}

int Test(ID3D12Device *device, ID3D12CommandQueue *queue, const char *shaderDirectory, const char *sharedDirectory)
{
    ofps::core::gpu::Shaders current, shared;
    Check(LoadShaders(shaderDirectory, current, true) && LoadSkyShaders(shaderDirectory, current), "depth rect: the current shaders load");
    Check(LoadShaders(shaderDirectory, shared, true) && LoadSkyShaders(shaderDirectory, shared) &&
              LoadDepthFSharedPasses(sharedDirectory, shared),
          "depth rect: the shared-rect passes load");
    Recording r;
    Check(r.Create(device), "depth rect: fixtures allocate");
    if (failures) return failures;
    const float sky[4] = {0.375f, 0.5f, 0.625f, 1.0f}, building[4] = {0.25f, 0.125f, 0.0625f, 1.0f};
    Scene scene;
    scene.color = r.Paint(device, kColor, sky, building, true);
    scene.motion = r.Paint(device, DXGI_FORMAT_R16G16_FLOAT, {8.0f, 0, 0, 0}, {}, false);
    for (int pass = 0; pass < 2; ++pass) {
        const float base[4] = {sky[0] + 0.125f * float(pass + 1), sky[1] - 0.0625f, sky[2], 1.0f};
        const float patch[4] = {building[0] + 0.5f, building[1] + 0.25f * float(pass + 1), building[2] + 0.375f, 1.0f};
        scene.fresh[pass] = r.Paint(device, kColor, base, patch, true);
    }
    scene.depth[0] = HalfDepth(device, r, 0);
    scene.depth[1] = HalfDepth(device, r, 1);
    Check(scene.color && scene.motion && scene.fresh[0] && scene.fresh[1] && scene.depth[0] && scene.depth[1], "depth rect: the scene paints");
    if (failures) return failures;

    std::vector<RunDesc> runs;
    for (const Mode mode : {Mode::Sync, Mode::Background, Mode::Discard})
        for (const int half : {0, 1, -1})
            for (const auto *set : {&current, &shared}) runs.push_back({set, mode, half});
    constexpr std::size_t kDiscardRuns = 12; // the first run of Mode::Discard
    std::vector<ComPtr<ID3D12Resource>> outputs(runs.size() * kOutputs), vectors((runs.size() - kDiscardRuns) * kKicks);
    std::vector<Machine> machines(runs.size());
    std::vector<Job> jobs(runs.size());
    std::vector<ID3D12Resource *> rawVectors(vectors.size());
    for (std::size_t v = 0; v < vectors.size(); ++v) {
        rawVectors[v] = (vectors[v] = Plain(device, kW, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_NONE)).Get();
        if (rawVectors[v]) BarrierExternal(r.list.Get(), rawVectors[v], D3D12_RESOURCE_STATE_COMMON, kRead);
    }
    for (std::size_t i = 0; i < runs.size(); ++i) {
        ID3D12Resource *raw[kOutputs] = {};
        for (int o = 0; o < kOutputs; ++o) raw[o] = (outputs[i * kOutputs + o] = Texture(device, kColor, D3D12_RESOURCE_FLAG_NONE)).Get();
        const bool background = runs[i].mode != Mode::Sync;
        if (runs[i].mode == Mode::Discard) {
            jobs[i].modelVectors = &rawVectors[(i - kDiscardRuns) * kKicks];
            jobs[i].discardKick = kDiscarded;
        }
        char error[256]{};
        Check((!background || jobs[i].Create(device, r.list.Get(), kDepthW)) &&
                  machines[i].Initialize(device, *runs[i].shaders, kW, kH, kColor, kColor, kW, kH, DXGI_FORMAT_R32_FLOAT, kDepthW, kH,
                                         error, sizeof(error), 3, background ? MachineRole::Background : MachineRole::Synchronous),
              "depth rect: the machine initializes");
        if (!machines[i].Ready()) { std::cerr << error << '\n'; return failures; }
        machines[i].SetUsePoint(r.Point(), 0);
        RecordRun(machines[i], r.list.Get(), scene, runs[i], jobs[i], raw);
        Check(!machines[i].TakeExhausted(), "depth rect: the sequence records within the descriptor ring");
    }
    for (ID3D12Resource *v : rawVectors)
        if (v) BarrierExternal(r.list.Get(), v, kRead, D3D12_RESOURCE_STATE_COMMON);
    const std::vector<Picture> pictures = Execute(device, queue, r, outputs, vectors);
    Check(pictures.size() == outputs.size() + vectors.size(), "depth rect: the GPU completes the passes and the frames read back");
    if (pictures.size() != outputs.size() + vectors.size()) return failures;
    auto same = [&](std::size_t a, std::size_t b) {
        for (int o = 0; o < kOutputs; ++o) if (!SameBits(pictures[a * kOutputs + o], pictures[b * kOutputs + o])) return false;
        if (a < kDiscardRuns) return true;
        for (int k = 0; k < kKicks; ++k) // and the validated model vectors of every kick
            if (!SameBits(pictures[outputs.size() + (a - kDiscardRuns) * kKicks + k], pictures[outputs.size() + (b - kDiscardRuns) * kKicks + k]))
                return false;
        return true;
    };
    const unsigned validated = jobs[kDiscardRuns + 4].vectorKicks;
    Check((validated & (1u << kDiscarded)) && (validated & (1u << (kDiscarded + 1))),
          "depth rect: the discarded kick and the one after it do not validate model vectors");
    for (std::size_t mode = 0; mode < 3; ++mode) {
        const std::size_t base = mode * 6; // half 0 (current, shared), half 1 (current, shared), moving (current, shared)
        const char *name = mode == 0 ? "sync" : mode == 1 ? "background" : "background with a discarded kick";
        Check(same(base + 0, base + 1) && same(base + 2, base + 3), "depth rect: a stable sub-rect does not write the shared-rect passes' bytes");
        Check(same(base + 4, base + 0), "depth rect: a moving sub-rect does not write the stable sequence's bytes");
        Check(!same(base + 5, base + 1), "depth rect: the scene cannot tell the shared-rect passes' reads of a moved snapshot");
        std::printf("depth rect, %s: a stable sub-rect (either half) writes the shared-rect passes' bytes; a moving one "
                    "(%s) writes the stable sequence's bytes, where the shared-rect passes do not\n",
                    name, mode == 0 ? "passes alternating halves" : mode == 1 ? "kicks on the left half, then on the right"
                                                                              : "halves alternating per kick; frames and validated vectors");
    }
    return failures;
}
} // namespace

int main(int argc, char **argv)
{
    if (argc != 3) {
        std::cerr << "usage: ofps_temporal_depth_rect_tests <shader dir> <shared-rect pass dir>\n";
        return 1;
    }
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
    int result = Test(device.Get(), queue.Get(), argv[1], argv[2]);
    ComPtr<ID3D12InfoQueue> info;
    if (debug && SUCCEEDED(device.As(&info))) {
        for (UINT64 i = 0, n = info->GetNumStoredMessages(); i < n; ++i) {
            SIZE_T bytes = 0;
            if (FAILED(info->GetMessage(i, nullptr, &bytes))) continue;
            std::vector<char> storage(bytes);
            auto *message = reinterpret_cast<D3D12_MESSAGE *>(storage.data());
            if (SUCCEEDED(info->GetMessage(i, message, &bytes)) && message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) {
                std::cerr << "FAIL: D3D12 debug layer: " << message->pDescription << '\n';
                ++result;
            }
        }
    }
    if (result == 0) std::cout << "PASS: the depth snapshots keep their own sub-rect\n";
    return result == 0 ? 0 : 1;
}
