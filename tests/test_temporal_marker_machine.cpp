// The two-pass V3 in the machine (DebugTemporalMarkerRule 3/4): Resources::RecordMarkerFail before ModelMotion and
// MarkerReason, in the synchronous chain test (accumulation.cpp) and in the background kick's validation (background.cpp).
// D3D12 WARP with the debug layer. A building 8 px/frame across a sky: the chain's end leaves the building at its edges,
// so the depth test fails in bands the 5x5 vote thins. Three pass sets: with MarkerFail; without it (one log line, every
// neighbour tested inline); and a decoy MarkerFail (the MarkerReason binary: final verdicts in place of base verdicts).
//   - rule 0 and rule 4 at thresholds 8 and 25: with and without MarkerFail the model's vectors are the same bits, sync
//     (two full passes) and background (every kick with a copied chain);
//   - the decoy changes rule 4 at 25: the two-pass route reads the mask; rule 4 at 25 differs from rule 0: the vote acts;
//   - a set without either marker pass initializes and runs (they are optional);
//   - TemporalModelMarker through ResolveModelMarker (core/temporal/model_marker.h): 1 writes rule 0's bits, 2 rule 4 at
//     8's, 0 hands over the plain chain (no ModelMotion on full passes, unvalidated kicks) and so differs from rule 0.
// Usage: ofps_temporal_marker_machine_tests <shader dir>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "temporal_background_driver.h"
#include "core/temporal/model_marker.h"

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

constexpr int kEvery = 3, kKicks = 4, kFrames = 11, kSyncPasses = 2;
constexpr int kVectors = kKicks > kSyncPasses ? kKicks : kSyncPasses; // validated vector textures per run

enum class Set { Fail, Inline, Decoy, None };

// The marker's optional passes as the core loads them: MarkerFail as built, missing, or the decoy.
void LoadMarker(const char *directory, ofps::core::gpu::Shaders &s, Set set)
{
    const std::filesystem::path dir(directory);
    if (set != Set::None) ReadInto(dir / "temporal_MarkerReason_cs.dxbc", s.temporalMarkerReason);
    if (set == Set::Fail) ReadInto(dir / "temporal_MarkerFail_cs.dxbc", s.temporalMarkerFail);
    if (set == Set::Decoy) ReadInto(dir / "temporal_MarkerReason_cs.dxbc", s.temporalMarkerFail);
}

struct Scene {
    ComPtr<ID3D12Resource> color, motion, depth, fresh[2];
};

struct RunDesc {
    Set set;
    bool background;
    int rule, need;
    bool markers = true; // false: the model gets the plain chain (TemporalModelMarker=0)
};

FrameInputs Inputs(const Scene &scene, const RunDesc &run)
{
    FrameInputs in{};
    in.color = scene.color.Get(); in.depth = scene.depth.Get(); in.motion = scene.motion.Get();
    in.colorView = kColor; in.depthView = DXGI_FORMAT_R32_FLOAT; in.motionView = DXGI_FORMAT_R16G16_FLOAT;
    in.colorRect = in.motionRect = in.depthRect = {0, 0, kW, kH};
    in.hostInputState = in.depthState = in.motionState = kRead;
    in.background = run.background;
    in.markerRule = run.rule;
    in.markerNeighbours = run.need;
    return in;
}

void CopyModelMv(ID3D12GraphicsCommandList *list, Machine &machine, ID3D12Resource *to)
{
    ID3D12Resource *mv = machine.ModelMv();
    BarrierExternal(list, mv, kRead, D3D12_RESOURCE_STATE_COPY_SOURCE);
    BarrierExternal(list, to, kRead, D3D12_RESOURCE_STATE_COPY_DEST);
    list->CopyResource(to, mv);
    BarrierExternal(list, to, D3D12_RESOURCE_STATE_COPY_DEST, kRead);
    BarrierExternal(list, mv, D3D12_RESOURCE_STATE_COPY_SOURCE, kRead);
}

void RecordRun(Machine &machine, ID3D12GraphicsCommandList *list, const Scene &scene, const RunDesc &run, Job &job,
               ID3D12Resource *const *outputs, ID3D12Resource *const *vectors)
{
    ID3D12Resource *const fresh[2] = {scene.fresh[0].Get(), scene.fresh[1].Get()};
    const FrameInputs in = Inputs(scene, run);
    if (run.background) {
        job.modelVectors = vectors;
        job.validate = run.markers;
        for (int frame = 0; frame < kFrames; ++frame) RecordBackgroundFrame(machine, list, job, in, fresh, outputs[frame], kEvery);
        return;
    }
    for (int pass = 0; pass < kSyncPasses; ++pass) { // a full pass after three carried frames: the model's vectors
        machine.RecordResidual(list, in, fresh[pass % 2], kRead);
        for (int carried = 0; carried < 3; ++carried) machine.RecordAccumulate(list, in);
        if (!run.markers) { // the host hands the model the chain itself (dispatch_body.cpp's useModelMv)
            Copy(list, machine.Acc(), vectors[pass]);
            continue;
        }
        machine.RecordModelMotion(list, in);
        Check(machine.RecordMarkerReason(list, in) == (run.set != Set::None), "marker machine: the reason pass records when it is loaded");
        CopyModelMv(list, machine, vectors[pass]);
    }
}

int Test(ID3D12Device *device, ID3D12CommandQueue *queue, const char *shaderDirectory)
{
    ofps::core::gpu::Shaders sets[4];
    for (int i = 0; i < 4; ++i) {
        Check(LoadShaders(shaderDirectory, sets[i], true) && LoadSkyShaders(shaderDirectory, sets[i]), "marker machine: the shaders load");
        LoadMarker(shaderDirectory, sets[i], Set(i));
    }
    Check(!sets[0].temporalMarkerFail.empty() && sets[1].temporalMarkerFail.empty(), "marker machine: MarkerFail loads alone");
    Recording r;
    Check(r.Create(device), "marker machine: fixtures allocate");
    if (failures) return failures;
    const float sky[4] = {0.375f, 0.5f, 0.625f, 1.0f}, building[4] = {0.25f, 0.125f, 0.0625f, 1.0f};
    const float skyDepth[4] = {1.0f, 0, 0, 0}, buildingDepth[4] = {0.25f, 0, 0, 0};
    Scene scene;
    scene.color = r.Paint(device, kColor, sky, building, true);
    scene.depth = r.Paint(device, DXGI_FORMAT_R32_FLOAT, skyDepth, buildingDepth, true);
    scene.motion = r.Paint(device, DXGI_FORMAT_R16G16_FLOAT, {8.0f, 0, 0, 0}, {}, false);
    for (int pass = 0; pass < 2; ++pass) {
        const float base[4] = {sky[0] + 0.125f * float(pass + 1), sky[1] - 0.0625f, sky[2], 1.0f};
        const float patch[4] = {building[0] + 0.5f, building[1] + 0.25f * float(pass + 1), building[2] + 0.375f, 1.0f};
        scene.fresh[pass] = r.Paint(device, kColor, base, patch, true);
    }
    Check(scene.color && scene.depth && scene.motion && scene.fresh[0] && scene.fresh[1], "marker machine: the scene paints");
    if (failures) return failures;

    // Per mode: rule 0 (Fail, Inline), rule 4 n8 (Fail, Inline), rule 4 n25 (Fail, Inline, Decoy), rule 4 n8 with None.
    std::vector<RunDesc> runs;
    for (const bool background : {false, true}) {
        runs.push_back({Set::Fail, background, 0, 0}); runs.push_back({Set::Inline, background, 0, 0});
        runs.push_back({Set::Fail, background, 4, 8}); runs.push_back({Set::Inline, background, 4, 8});
        runs.push_back({Set::Fail, background, 4, 25}); runs.push_back({Set::Inline, background, 4, 25});
        runs.push_back({Set::Decoy, background, 4, 25}); runs.push_back({Set::None, background, 4, 8});
        for (const int setting : {0, 1, 2}) { // TemporalModelMarker, no Debug* override
            const auto m = ofps::core::temporal::ResolveModelMarker(setting, 0, 0, false, false);
            runs.push_back({Set::Fail, background, m.rule, m.neighbours, background ? m.background : m.sync});
        }
    }
    constexpr std::size_t kPerMode = 11;
    std::vector<ComPtr<ID3D12Resource>> outputs(runs.size() * kFrames), vectors(runs.size() * kVectors);
    std::vector<ID3D12Resource *> rawVectors(vectors.size());
    for (std::size_t v = 0; v < vectors.size(); ++v) {
        rawVectors[v] = (vectors[v] = Plain(device, kW, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_NONE)).Get();
        if (rawVectors[v]) BarrierExternal(r.list.Get(), rawVectors[v], D3D12_RESOURCE_STATE_COMMON, kRead);
    }
    std::vector<Machine> machines(runs.size());
    std::vector<Job> jobs(runs.size());
    for (std::size_t i = 0; i < runs.size(); ++i) {
        ID3D12Resource *raw[kFrames] = {};
        for (int o = 0; o < kFrames; ++o) raw[o] = (outputs[i * kFrames + o] = Texture(device, kColor, D3D12_RESOURCE_FLAG_NONE)).Get();
        char error[256]{};
        const bool background = runs[i].background;
        Check((!background || jobs[i].Create(device, r.list.Get(), kW)) &&
                  machines[i].Initialize(device, sets[int(runs[i].set)], kW, kH, kColor, kColor, kW, kH, DXGI_FORMAT_R32_FLOAT, kW, kH,
                                         error, sizeof(error), 3, background ? MachineRole::Background : MachineRole::Synchronous),
              "marker machine: the machine initializes (marker passes optional)");
        if (!machines[i].Ready()) { std::cerr << error << '\n'; return failures; }
        machines[i].SetUsePoint(r.Point(), 0);
        RecordRun(machines[i], r.list.Get(), scene, runs[i], jobs[i], raw, &rawVectors[i * kVectors]);
        Check(!machines[i].TakeExhausted(), "marker machine: the sequence records within the descriptor ring");
    }
    for (ID3D12Resource *v : rawVectors)
        if (v) BarrierExternal(r.list.Get(), v, kRead, D3D12_RESOURCE_STATE_COMMON);
    const std::vector<Picture> pictures = Execute(device, queue, r, outputs, vectors);
    Check(pictures.size() == outputs.size() + vectors.size(), "marker machine: the GPU completes the passes and the results read back");
    if (pictures.size() != outputs.size() + vectors.size()) return failures;
    auto same = [&](std::size_t a, std::size_t b) {
        for (int o = 0; o < kFrames; ++o) if (!SameBits(pictures[a * kFrames + o], pictures[b * kFrames + o])) return false;
        for (int k = 0; k < kVectors; ++k)
            if (!SameBits(pictures[outputs.size() + a * kVectors + k], pictures[outputs.size() + b * kVectors + k])) return false;
        return true;
    };
    for (std::size_t mode = 0; mode < 2; ++mode) {
        const std::size_t base = mode * kPerMode;
        const char *name = mode == 0 ? "sync" : "background";
        if (mode == 1) Check(jobs[base].vectorKicks != 0, "marker machine: the background kicks validate model vectors");
        Check(same(base + 0, base + 1), "marker machine: rule 0 with and without MarkerFail differ");
        Check(same(base + 2, base + 3), "marker machine: rule 4 n8 two-pass differs from inline");
        Check(same(base + 4, base + 5), "marker machine: rule 4 n25 two-pass differs from inline");
        Check(!same(base + 6, base + 4), "marker machine: the decoy mask does not change rule 4 n25 (the mask is not read)");
        Check(!same(base + 4, base + 0), "marker machine: rule 4 n25 equals rule 0 (the scene cannot see the vote)");
        Check(same(base + 7, base + 2), "marker machine: without either marker pass rule 4 n8 differs from the two-pass route");
        Check(!same(base + 8, base + 0), "marker machine: TemporalModelMarker=0 still marks (equals rule 0)");
        Check(same(base + 9, base + 0), "marker machine: TemporalModelMarker=1 differs from rule 0");
        Check(same(base + 10, base + 2), "marker machine: TemporalModelMarker=2 differs from rule 4 n8");
        std::printf("marker machine, %s: rules 0 and 4 (8, 25) write the same bits with and without MarkerFail; the decoy and the "
                    "vote show; TemporalModelMarker 0/1/2 = plain chain / rule 0 / rule 4 n8\n", name);
    }
    return failures;
}
} // namespace

int main(int argc, char **argv)
{
    if (argc != 2) {
        std::cerr << "usage: ofps_temporal_marker_machine_tests <shader dir>\n";
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
    int result = Test(device.Get(), queue.Get(), argv[1]);
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
    if (result == 0) std::cout << "PASS: the two-pass V3 marker in the machine, sync and background\n";
    return result == 0 ? 0 : 1;
}
