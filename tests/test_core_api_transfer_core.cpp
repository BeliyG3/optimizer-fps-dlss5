#include "test_core_api_scenarios.h"
#include "test_core_api_checks.h"
#include "test_core_api_reference.h"

// Colour filters 2/3 through the whole core (the API only, so both the static and the DLL core run it).
// The fake model copies its input to its output: the model changes nothing, so with the transfer every
// frame (full, carried, background) is the host colour, while the soft filter stretches the periphery.

namespace coretest {
namespace {
OfpsSettingsValues TransferSettings(const OfpsSettingsValues &from, int filter, int temporalMode) {
    auto settings = from;
    SetInt(settings, OFPS_SET_MODE, 2);
    SetFloat(settings, OFPS_SET_CENTER_X, 80.0f);
    SetFloat(settings, OFPS_SET_CENTER_Y, 80.0f);
    SetFloat(settings, OFPS_SET_WORK_X, 90.0f);
    SetFloat(settings, OFPS_SET_WORK_Y, 90.0f);
    SetInt(settings, OFPS_SET_TEMPORAL_MODE, temporalMode);
    SetInt(settings, OFPS_SET_DEBUG_WARP_PATH, 1);
    SetInt(settings, OFPS_SET_COLOR_FILTER, filter);
    return settings;
}

bool IsSource(const EvalRun &run) {
    return run.ok && run.result == OFPS_OK && MatchesImage(run.output, Source(), kW, kH, kSdkTolerance);
}

// Sync modes 0 and 1: the request on the host output and on the temporal target, no temporal base.
void SyncFrames(WarpDevice &w, IOfpsCore *core, HostFrame &frame, const OfpsSettingsValues &before) {
    auto settings = TransferSettings(before, 1, 0);
    Check(core->SetSettings(&settings) == OFPS_OK, "transfer scenario: soft filter accepted");
    FakeModelHost model;
    IOfpsFeature *feature = nullptr;
    Check(CreateFeatureOnList(w, core, &model, &feature) == OFPS_OK && feature, "transfer scenario feature creates");
    if (!feature) return;
    const EvalRun plain = RunEvaluate(w, core, feature, frame, frame.output.Get(), 1);
    Check(plain.ok && plain.eval.warpPath == OFPS_WARP_COMPUTE &&
              !MatchesImage(plain.output, Source(), kW, kH, kSdkTolerance),
          "the soft filter stretches the periphery (differs from the host colour)");
    settings = TransferSettings(before, 3, 0);
    Check(core->SetSettings(&settings) == OFPS_OK, "depth-guided detail transfer setting is accepted");
    const EvalRun transferred = RunEvaluate(w, core, feature, frame, frame.output.Get(), 1);
    Check(transferred.ok && transferred.result == OFPS_OK && transferred.eval.warpPath == OFPS_WARP_COMPUTE,
          "detail transfer evaluates on the compute path");
    Check(IsSource(transferred), "detail transfer returns the host colour where the model changed nothing");
    settings = TransferSettings(before, 3, 1);
    Check(core->SetSettings(&settings) == OFPS_OK, "temporal mode with transfer accepted");
    const EvalRun full = RunEvaluate(w, core, feature, frame, frame.output.Get(), 1);
    const EvalRun carried = RunEvaluate(w, core, feature, frame, frame.output.Get(), 0);
    Check(IsSource(full), "a temporal full frame with transfer returns the host colour");
    Check(IsSource(carried) && carried.eval.path == OFPS_PATH_CARRIED,
          "an interpolated frame with transfer keeps full-size detail (no stretched base)");
    feature->Release();
    DrainReleasedModels(w, core);
}

// Background mode (3): the pass runs Pack -> model -> Unpack on the core's own list and the host frames
// carry its residual. With the transfer the residual is measured against the raw colour (no base).
void BackgroundFrames(WarpDevice &w, IOfpsCore *core, HostFrame &frame, const OfpsSettingsValues &before) {
    auto settings = TransferSettings(before, 3, 3);
    // A GLOBAL_REALTIME queue request without the privilege is a debug-layer error; the test needs none.
    SetInt(settings, OFPS_SET_DEBUG_ASYNC_NO_REALTIME, 1);
    Check(core->SetSettings(&settings) == OFPS_OK, "background mode with transfer accepted");
    FakeModelHost model;
    IOfpsFeature *feature = nullptr;
    Check(CreateFeatureOnList(w, core, &model, &feature) == OFPS_OK && feature, "background transfer feature creates");
    if (!feature) return;
    // More frames than the largest adoption age (16), so a finished pass is adopted and carried.
    constexpr int kFrames = 20;
    int carried = 0, warped = 0, wrong = 0;
    for (int i = 0; i < kFrames; ++i) {
        const EvalRun run = RunEvaluate(w, core, feature, frame, frame.output.Get(), i == 0 ? 1u : 0u);
        if (!IsSource(run)) ++wrong;
        if (run.eval.path == OFPS_PATH_CARRIED) ++carried;
        if (run.eval.path == OFPS_PATH_WARPED) ++warped;
    }
    std::cerr << "background transfer: warped=" << warped << " carried=" << carried << " not host colour=" << wrong << '\n';
    Check(CurrentStatus(core).temporalMode == 3, "background transfer runs in the background mode");
    Check(model.runCalls >= 2 && warped >= 1 && carried >= 1,
          "background transfer kicks passes and carries frames between them");
    Check(wrong == 0, "every background frame with transfer is the host colour (no stretched base in the residual)");
    feature->Release();
    DrainReleasedModels(w, core);
}
} // namespace

void ScenarioComputeTransferCore(WarpDevice &w, IOfpsCore *core, HostFrame &frame) {
    const auto before = CurrentSettings(core);
    SyncFrames(w, core, frame, before);
    BackgroundFrames(w, core, frame, before);
    Check(core->SetSettings(&before) == OFPS_OK, "transfer scenario restores the previous settings");
}

} // namespace coretest
