#include "core/frame/model_grid_path.h"

#include "core/frame/callback_guard.h"
#include "core/frame/codec_frame.h"
#include "core/frame/feature_state.h"
#include "core/frame/model_grid.h"
#include "core/frame/model_passes.h"
#include "core/frame/model_protocol.h"
#include "core/frame/timing.h"
#include "core/frame/warp_recorder.h"
#include "core/context.h"

#include <cstdio>

namespace ofps::core {

bool PassesPending(FeatureState &st) {
    bool pending = ModelPending(st);
    for (unsigned i = 0; i < 2; ++i) {
        if (!st.passPending[i]) continue;
        if (!GuardCallback([&] { return st.modelHost->ModelReady(st.passHandles[i]); }, 0)) {
            pending = true;
        } else {
            st.passPending[i] = false;
            st.passReset[i] = true;
            st.forcedReset = true;
        }
    }
    return pending;
}

int CreationFrame(FeatureState &st, ID3D12GraphicsCommandList *cmd,
                  const OfpsFrameInputs &frame, OfpsEvalResult &result) {
    result.path = OFPS_PATH_CREATION_FRAME;
    FallbackFromFrame(st, cmd, frame, "model creation frame");
    return OFPS_OK;
}

int ModelGridFallback(FeatureState &st, ID3D12GraphicsCommandList *cmd,
                      const OfpsFrameInputs &frame, const char *reason, int result) {
    FallbackFromFrame(st, cmd, frame, reason);
    return result;
}

void ModelGridInputs(const FeatureState &st, const CodecFrame &codec,
                     const OfpsFrameInputs &frame, OfpsModelInputs &inputs) {
    inputs.color = codec.modelColor;
    inputs.output = codec.answer;
    inputs.width = codec.modelColor.rect.w;
    inputs.height = codec.modelColor.rect.h;
    inputs.withholdUi = st.withholdUi;
    if (codec.modelColor.rect.x != frame.color.rect.x || codec.modelColor.rect.y != frame.color.rect.y ||
        codec.modelColor.rect.w != frame.color.rect.w || codec.modelColor.rect.h != frame.color.rect.h) {
        const float mvToWorkX = static_cast<float>(codec.modelColor.rect.w) / static_cast<float>(frame.color.rect.w);
        const float mvToWorkY = static_cast<float>(codec.modelColor.rect.h) / static_cast<float>(frame.color.rect.h);
        inputs.mvScaleX = frame.mvScaleX * mvToWorkX;
        inputs.mvScaleY = frame.mvScaleY * mvToWorkY;
    }
}

void ModelGridInitialInputs(const FeatureState &st, const CodecFrame &codec, OfpsModelInputs &inputs) {
    if (!st.modelResolution) return;
    inputs.color = codec.modelColor;
    inputs.output = codec.answer;
    inputs.width = st.createdWidth;
    inputs.height = st.createdHeight;
    inputs.withholdUi = st.withholdUi;
}

int RunModelGridDirect(FeatureState &st, ID3D12GraphicsCommandList *cmd,
                       const OfpsFrameInputs &frame, CodecFrame &codec, OfpsModelInputs &inputs) {
    ModelGridInputs(st, codec, frame, inputs);
    TimingBegin(st, cmd);
    int result = EvaluateModelPasses(st, cmd, inputs);
    if (result == OFPS_OK) result = ResolveCodecFrame(st, cmd, codec);
    TimingEnd(st, cmd);
    if (result != OFPS_OK) FallbackFromFrame(st, cmd, frame, "model or codec resolve failed");
    return result;
}

int RunUnfitModelPath(FeatureState &st, ID3D12GraphicsCommandList *cmd,
                      const OfpsFrameInputs &frame, CodecFrame &codec, OfpsModelInputs &inputs) {
    // Extra passes, spread and temporal modes require colour and output on the feature grid.
    if (!st.modelResolution) {
        inputs.width = frame.color.rect.w;
        inputs.height = frame.color.rect.h;
    }
    ++Ctx().status.passthroughs;
    Ctx().status.active = false;
    if (st.config.mode == ofps::sdk::WarpMode::Off && Ctx().temporal.mode == 0) SetReason("mode is Off");
    else SetReason("%s", st.hostReason);
    Ctx().status.temporalMode = 0;
    Ctx().status.modelPassesRunning = 1;
    if (Ctx().temporal.mode != 0)
        std::snprintf(Ctx().status.temporalReason, sizeof(Ctx().status.temporalReason), "%s", st.hostReason);
    if (Ctx().temporal.modelPasses > 1)
        std::snprintf(Ctx().status.modelPassReason, sizeof(Ctx().status.modelPassReason),
                      "Extra model passes off: %.150s", st.hostReason);
    if (st.modelResolution) {
        ReportModelGridPlan7Limit(st);
        return RunModelGridDirect(st, cmd, frame, codec, inputs);
    }
    TimingBegin(st, cmd);
    const int result = EvaluateModelPasses(st, cmd, inputs);
    TimingEnd(st, cmd);
    return result;
}

int RunPlainModelPath(FeatureState &st, ID3D12GraphicsCommandList *cmd,
                      const OfpsFrameInputs &frame, CodecFrame &codec, OfpsModelInputs &inputs) {
    ++Ctx().status.passthroughs;
    Ctx().status.active = false;
    if (!st.disabled) {
        if (st.config.mode == ofps::sdk::WarpMode::Off) SetReason("mode is Off");
        else SetReason("the layout does not reduce the frame (work size equals native)");
    }
    if (!st.modelResolution) {
        int prepared = BeginCodecFrame(st, cmd, frame, codec);
        if (prepared == OFPS_OK) prepared = ConfigureModelGrid(st, cmd, codec);
        if (prepared != OFPS_OK) {
            if (prepared != OFPS_S_MODEL_NEXT_FRAME)
                FallbackFromFrame(st, cmd, frame, "codec preparation failed");
            return prepared;
        }
    }
    if (st.modelResolution) return RunModelGridDirect(st, cmd, frame, codec, inputs);
    ModelGridInputs(st, codec, frame, inputs);
    TimingBegin(st, cmd);
    int result = EvaluateModelPasses(st, cmd, inputs);
    if (result == OFPS_OK) result = ResolveCodecFrame(st, cmd, codec);
    TimingEnd(st, cmd);
    if (result != OFPS_OK) FallbackFromFrame(st, cmd, frame, "model or codec resolve failed");
    return result;
}

void ReportModelGridPlan7Limit(const FeatureState &st) {
    if (!st.modelResolution) return;
    constexpr char reason[] = "not supported for a host with its own model grid yet (plan 7)";
    auto &status = Ctx().status;
    status.temporalMode = 0;
    status.modelPassesRunning = 1;
    std::snprintf(status.temporalReason, sizeof(status.temporalReason), "%s", reason);
    std::snprintf(status.modelPassReason, sizeof(status.modelPassReason), "%s", reason);
}
} // namespace ofps::core
