#include "core/frame/callback_guard.h"
#include "core/frame/model_protocol.h"
#include "core/frame/feature_state.h"
namespace ofps::core {
bool ModelPending(FeatureState &st) {
    if (!st.modelHost)
        return false;
    if (!st.modelPending)
        return false;
    if (!GuardCallback([&] { return st.modelHost->ModelReady(st.realHandle); }, 0))
        return true;
    st.modelPending = false;
    st.forcedReset = true;
    return false;
}
int PrepareCodec(FeatureState &st, ID3D12GraphicsCommandList *cmd, const OfpsFrameInputs &frame,
                 OfpsResource *modelColor, OfpsResource *frameBefore, bool *identity) {
    *modelColor = {};
    modelColor->size = sizeof(*modelColor);
    *frameBefore = {};
    frameBefore->size = sizeof(*frameBefore);
    const int r =
        GuardCallback([&] { return st.modelHost->PrepareModelInput(cmd, &frame, modelColor, frameBefore); });
    *identity = r == OFPS_S_IDENTITY;
    if (*identity) {
        *modelColor = frame.color;
        *frameBefore = {};
        frameBefore->size = sizeof(*frameBefore);
        return OFPS_OK;
    }
    if (r != OFPS_OK)
        return r;
    return modelColor->res ? OFPS_OK : OFPS_E_ARG;
}
} // namespace ofps::core
