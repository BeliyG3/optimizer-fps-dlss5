#include "core/frame/callback_guard.h"
#include "core/frame/model_grid.h"
#include "core/frame/codec_frame.h"
#include "core/frame/feature_state.h"
#include "core/frame/lifecycle.h"
#include "core/frame/model_protocol.h"
#include "core/frame/model_ui.h"
#include "core/frame/host_shape.h"
#include "core/context.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace ofps::core {
void BuryWarpGpu(FeatureState &st)
{
    gpu::Grave grave;
    grave.gate = RetirementGate(st);
    grave.modelHost = st.modelHost;
    grave.adapter12 = std::move(st.adapter);
    if (st.compute) grave.disposables.push_back(std::move(st.compute));
    for (ID3D12Resource **resource : {&st.nrOutput, &st.unpackTarget, &st.unpackBase, &st.passStaging}) {
        if (*resource) grave.objects.push_back(*resource);
        *resource = nullptr;
    }
    if (st.rtvHeap) grave.objects.push_back(st.rtvHeap);
    st.rtvHeap = nullptr;
    if (grave.adapter12 || !grave.disposables.empty() || !grave.objects.empty())
        Ctx().graveyard.Add(std::move(grave), Ctx().evalCounter);
    st.nrOutputState = st.unpackState = st.unpackBaseState = st.passStagingState = D3D12_RESOURCE_STATE_COMMON;
    st.colorView = st.outputView = DXGI_FORMAT_UNKNOWN;
    st.warpPath = warp::PackPath::None;
    st.warpPathReason[0] = '\0';
    st.gpuNativeWidth = st.gpuNativeHeight = st.gpuWorkWidth = st.gpuWorkHeight = 0;
    st.nextPackSlot = 0;
    st.packSets.Reset(FeatureState::kPackSlots * 2, FeatureState::kPackSetRing);
    for (auto &key : st.packKeys) key = {};
    for (auto &valid : st.unpackValid) valid = false;
    for (auto &state : st.packedColorState) state = D3D12_RESOURCE_STATE_COMMON;
    for (auto &state : st.packedGuideState) state = D3D12_RESOURCE_STATE_COMMON;
    st.inputSignature = {};
    if (st.async) st.async->bgPackValid = false;
}

static int ReplaceModels(FeatureState &st, ID3D12GraphicsCommandList *cmd, uint32_t w, uint32_t h, bool warped)
{
    const bool firstModel = st.realHandle == nullptr;
    void *handles[3] = {};
    bool pending[3] = {};
    const int count = std::clamp(st.passesReady, 1, 3);
    for (int i = 0; i < count; ++i) {
        const int result = CreateModelGuarded(st, cmd, w, h, st.modelResolution ? st.withholdUi : warped, &handles[i]);
        if ((result != OFPS_OK && result != OFPS_S_MODEL_NEXT_FRAME) || !handles[i]) {
            const auto gate = RetirementGate(st);
            for (void *handle : handles) BuryReal(st.modelHost, handle, gate);
            return result < 0 ? result : OFPS_E_DEVICE;
        }
        pending[i] = result == OFPS_S_MODEL_NEXT_FRAME;
    }
    const auto gate = RetirementGate(st);
    BuryReal(st.modelHost, st.realHandle, gate);
    st.realHandle = handles[0];
    st.modelPending = pending[0];
    for (int i = 0; i < 2; ++i) {
        BuryReal(st.modelHost, st.passHandles[i], gate);
        st.passHandles[i] = handles[i + 1];
        st.passPending[i] = pending[i + 1];
        st.passReset[i] = true;
    }
    ++st.modelGeneration;
    st.createdWidth = w;
    st.createdHeight = h;
    st.forcedReset = true;
    Ctx().status.lastNgxResult = st.lastCreateHostResult;
    Ctx().status.workWidth = w;
    Ctx().status.workHeight = h;
    if (!firstModel)
        Log(false, "Optimizer FPS NGX hook: layout changed, model re-created at %ux%u (%d)", w, h, st.lastCreateHostResult);
    return std::any_of(pending, pending + count, [](bool value) { return value; })
        ? OFPS_S_MODEL_NEXT_FRAME : OFPS_OK;
}

int ConfigureModelGrid(FeatureState &st, ID3D12GraphicsCommandList *cmd, const CodecFrame &codec)
{
    // The established identity path owns its layout and disabled/pass-through decisions.
    if (codec.identity && !st.codecGridActive && !st.modelResolution) return OFPS_OK;
    if (!cmd || !codec.modelColor.res || !codec.answer.res) return OFPS_E_ARG;
    const auto w = codec.modelColor.rect.w, h = codec.modelColor.rect.h;
    if (!w || !h) return OFPS_E_ARG;
    const bool settingsChanged = st.modelResolution && !SameLayoutConfig(Ctx().config, st.config);
    const auto config = st.modelResolution ? Ctx().config : st.config;
    ofps::sdk::LayoutV2 layout{};
    const bool warped = !st.disabled && st.hostFits && DesiredLayout(config, w, h, &layout);
    const auto modelW = warped ? layout.workWidth : w;
    const auto modelH = warped ? layout.workHeight : h;
    const bool withholdUi = st.modelResolution && WithholdModelUi(codec.frame, modelW, modelH);
    const bool replace = !st.realHandle || st.createdWidth != modelW || st.createdHeight != modelH ||
        st.warped != warped || (st.modelResolution && (st.withholdUi != withholdUi || st.rebuildRequested));
    const bool oldWithholdUi = st.withholdUi;
    int result = OFPS_OK;
    if (replace) {
        if (st.modelResolution) st.withholdUi = withholdUi;
        result = ReplaceModels(st, cmd, modelW, modelH, warped);
        if (result < 0) {
            st.withholdUi = oldWithholdUi;
            return result;
        }
    }
    if (st.modelResolution) {
        st.config = config;
        st.withholdUi = withholdUi;
        st.rebuildRequested = false;
    }
    const bool layoutChanged = st.warped != warped ||
        (warped && std::memcmp(&st.layout, &layout, sizeof(layout)) != 0);
    if (layoutChanged || replace || settingsChanged) BuryWarpGpu(st);
    st.warped = warped;
    if (warped) st.layout = layout;
    st.codecGridActive = !codec.identity;
    if (result != OFPS_OK) return result;
    if (replace && st.modelResolution) return OFPS_S_MODEL_NEXT_FRAME;
    if (ModelPending(st)) return OFPS_S_MODEL_NEXT_FRAME;
    for (int i = 0; i < st.passesReady - 1; ++i) {
        if (!st.passPending[i]) continue;
        if (!GuardCallback([&] { return st.modelHost->ModelReady(st.passHandles[i]); }, 0)) return OFPS_S_MODEL_NEXT_FRAME;
        st.passPending[i] = false;
        st.passReset[i] = true;
        st.forcedReset = true;
    }
    if (warped && !EnsureGpu(st, cmd, codec.modelColor.res, codec.answer.res, codec.modelColor.view, codec.answer.view)) return OFPS_E_DEVICE;
    return OFPS_OK;
}
int PrepareModelGridFrame(FeatureState &st, ID3D12GraphicsCommandList *cmd,
                          const OfpsFrameInputs &frame, CodecFrame &codec) {
    if (!SameLayoutConfig(Ctx().config, st.config)) {
        st.disabled = false;
        st.hostLatched = false;
    }
    JudgeHostShape(st, ReadHostShape(frame));
    const int prepared = BeginCodecFrame(st, cmd, frame, codec);
    return prepared == OFPS_OK ? ConfigureModelGrid(st, cmd, codec) : prepared;
}
} // namespace ofps::core
