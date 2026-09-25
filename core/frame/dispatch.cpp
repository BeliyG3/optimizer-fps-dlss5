#include "core/frame/dispatch.h"
#include "core/context.h"
#include "core/frame/warp_recorder.h"
#include <cstdio>
namespace ofps::core {
namespace {
int EvaluateCpp(FeatureState &st, ID3D12GraphicsCommandList *cmd, const OfpsFrameInputs &frame, OfpsEvalResult &out) {
    try { return EvaluateFrameBody(st, cmd, frame, out); }
    catch (...) { out.path = OFPS_PATH_FALLBACK; out.warpPath = OFPS_WARP_NONE; return OFPS_E_STATE; }
}
int EndCpp(FeatureState &st, ID3D12GraphicsCommandList *cmd, const OfpsFrameInputs &frame, const OfpsEvalResult &out) {
    try { st.modelHost->EndFrame(cmd, &frame, &out); return OFPS_OK; }
    catch (...) { return OFPS_E_STATE; }
}
}
int EvaluateFrameGuarded(FeatureState &st, ID3D12GraphicsCommandList *cmd, const OfpsFrameInputs &frame, OfpsEvalResult &out) {
    int result = OFPS_E_STATE;
    __try { result = EvaluateCpp(st, cmd, frame, out); }
    __except (gpu::RecordCrash(GetExceptionInformation(), StageModel, Ctx().crash)) {
        result = OFPS_E_DEVICE; out.path = OFPS_PATH_FALLBACK; out.warpPath = OFPS_WARP_NONE;
    }
    out.modelResult = st.frameModelResult;
    if (st.frameCreation) out.path = OFPS_PATH_CREATION_FRAME;
    if (result < 0) { out.path = OFPS_PATH_FALLBACK; out.warpPath = OFPS_WARP_NONE; }
    Ctx().status.warpPath = out.path == OFPS_PATH_WARPED ? out.warpPath : OFPS_WARP_NONE;
    // A carried frame (temporal mode, no model pass) keeps the state of the last model frame, so the
    // banner does not flip between ACTIVE and NOT ACTIVE from frame to frame.
    if (out.path != OFPS_PATH_WARPED && out.path != OFPS_PATH_CARRIED) Ctx().status.active = false;
    std::snprintf(Ctx().status.warpPathReason, sizeof(Ctx().status.warpPathReason), "%s", st.warpPathReason);
    if (result == OFPS_OK && st.frameModelCalled && out.modelResult == OFPS_OK && out.path != OFPS_PATH_FALLBACK)
        st.forcedReset = false;
    __try { const int cleanup = EndCpp(st, cmd, frame, out); if (cleanup < 0) result = cleanup; }
    __except (gpu::RecordCrash(GetExceptionInformation(), StageParamsRestore, Ctx().crash)) { result = OFPS_E_DEVICE; }
    return result == kCrashed || result == kPackFailed || result == kNotHandled ? OFPS_E_DEVICE : result;
}
}
