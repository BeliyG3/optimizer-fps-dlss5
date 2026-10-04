#include "test_core_api_scenarios.h"
#include "test_core_api_checks.h"

#include <chrono>

// VRAM step 1, fix round 1: a change of the warp's optional textures while a background pass is in
// flight does not retire the job (settling it would wait on the CPU); the set switches once the pass
// has finished. The host queue is held behind a test fence, so the pass cannot finish meanwhile.

namespace coretest {
namespace {
// One evaluate into a render-target-only output: the first such output asks for the copy intermediates,
// a change of the texture plan. `held` keeps the host queue behind `gate` until the evaluate returned.
struct Timed {
    bool ok = false;
    int result = OFPS_E_STATE;
    double ms = 0.0;
};
Timed EvaluateNonUav(WarpDevice &w, IOfpsCore *core, IOfpsFeature *feature, HostFrame &frame, ID3D12Resource *output,
                     ID3D12Fence *gate, UINT64 held) {
    constexpr auto kRest = D3D12_RESOURCE_STATE_RENDER_TARGET;
    Timed timed;
    if (gate && FAILED(w.queue->Wait(gate, held))) return timed;
    if (!BeginList(w)) return timed;
    ID3D12GraphicsCommandList *cmd = w.list.Get();
    std::vector<ComPtr<ID3D12Resource>> uploads;
    if (!UploadPattern(w, frame.color.Get(), frame.colorRest, 0, uploads) ||
        !UploadPattern(w, frame.depth.Get(), kInputRest, 1, uploads) ||
        !UploadPattern(w, frame.motion.Get(), kInputRest, 2, uploads)) return timed;
    OfpsFrameInputs in = FrameInputs(frame, output, 0);
    in.output.restState = kRest;
    OfpsEvalResult eval{};
    eval.size = sizeof(eval);
    const auto start = std::chrono::steady_clock::now();
    timed.result = feature->Evaluate(cmd, &in, &eval);
    timed.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    if (gate && FAILED(gate->Signal(held))) return timed; // release the host queue (and the pass behind it)
    if (!SubmitList(w)) return timed;
    core->OnCommandListExecuted(w.queue.Get(), cmd);
    timed.ok = WaitForQueue(w.device.Get(), w.queue.Get());
    return timed;
}

std::size_t Count(const FakeHost &host, const char *needle) {
    std::size_t n = 0;
    for (const auto &line : host.logs) n += line.find(needle) != std::string::npos ? 1 : 0;
    return n;
}
} // namespace

void ScenarioPlanDeferral(WarpDevice &w, IOfpsCore *core, FakeHost &host, HostFrame &frame) {
    const auto before = CurrentSettings(core);
    auto settings = before;
    SetInt(settings, OFPS_SET_MODE, 2);
    SetFloat(settings, OFPS_SET_CENTER_X, 80.0f);
    SetFloat(settings, OFPS_SET_CENTER_Y, 80.0f);
    SetFloat(settings, OFPS_SET_WORK_X, 90.0f);
    SetFloat(settings, OFPS_SET_WORK_Y, 90.0f);
    SetInt(settings, OFPS_SET_TEMPORAL_MODE, 3);
    SetInt(settings, OFPS_SET_DEBUG_WARP_PATH, 1);
    SetInt(settings, OFPS_SET_DEBUG_ASYNC_NO_REALTIME, 1);
    Check(core->SetSettings(&settings) == OFPS_OK, "plan deferral: background compute settings accepted");
    auto nonUav = CreateTexture(w.device.Get(), kW, kH, kColorFormat, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                                D3D12_RESOURCE_STATE_RENDER_TARGET);
    ComPtr<ID3D12Fence> gate;
    Check(nonUav && SUCCEEDED(w.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate))),
          "plan deferral: fixtures allocate");
    FakeModelHost model;
    IOfpsFeature *feature = nullptr;
    Check(CreateFeatureOnList(w, core, &model, &feature) == OFPS_OK && feature, "plan deferral: feature creates");
    if (!feature || !nonUav || !gate) return;

    // UAV frames until one kicks a background pass (the model runs on the core's own list).
    bool kicked = false;
    for (int i = 0; i < 24 && !kicked; ++i) {
        const auto runs = model.runCalls;
        const EvalRun run = RunEvaluate(w, core, feature, frame, frame.output.Get(), i == 0 ? 1u : 0u);
        kicked = run.ok && i > 0 && model.runCalls > runs;
    }
    Check(kicked, "plan deferral: a background pass is kicked");
    if (!kicked) { feature->Release(); DrainReleasedModels(w, core); return; }

    // The pass waits for the host queue's signal, which is held: the plan change must not settle it.
    const auto readyBefore = Count(host, "GPU path ready");
    const Timed held = EvaluateNonUav(w, core, feature, frame, nonUav.Get(), gate.Get(), 1);
    std::cerr << "plan deferral: evaluate with the pass held took " << held.ms << " ms\n";
    Check(held.ok && held.result == OFPS_OK, "plan deferral: the frame with the pass in flight evaluates");
    Check(held.ms < 1500.0, "plan deferral: no CPU wait for the pass in flight (Settle would wait seconds)");
    Check(host.LogContains("texture set change deferred until the background pass completes"),
          "plan deferral: the texture set change is deferred");
    Check(Count(host, "GPU path ready") == readyBefore, "plan deferral: the set is not rebuilt while the pass runs");

    // Once the pass has finished, the next evaluates switch the set (a rebuild logs "GPU path ready").
    bool switched = false;
    for (int i = 0; i < 24 && !switched; ++i) {
        const Timed next = EvaluateNonUav(w, core, feature, frame, nonUav.Get(), nullptr, 0);
        Check(next.ok && next.result == OFPS_OK, "plan deferral: a frame after the release evaluates");
        switched = Count(host, "GPU path ready") > readyBefore;
        if (!switched) Sleep(5);
    }
    Check(switched, "plan deferral: the set switches after the pass completed");
    feature->Release();
    DrainReleasedModels(w, core);
    Check(core->SetSettings(&before) == OFPS_OK, "plan deferral: the previous settings are restored");
}

} // namespace coretest
