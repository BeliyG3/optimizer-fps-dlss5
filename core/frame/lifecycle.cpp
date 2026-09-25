#include "core/frame/callback_guard.h"
#include "core/frame/model_passes.h"
#include "core/frame/dispatch.h"

#include "core/temporal/async_scheduler.h"
#include "core/frame/debug_readback.h"
#include "core/context.h"
#include "core/frame/host_depth_state.h"
#include "core/frame/frame_inputs.h"
#include "core/temporal/controller.h"
#include "core/frame/timing.h"
#include "core/frame/warp_recorder.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>

#include "core/frame/lifecycle.h"

namespace ofps::core {

bool DesiredLayout(const ofps::sdk::ConfigV2 &config, std::uint32_t nativeW, std::uint32_t nativeH, ofps::sdk::LayoutV2 *layout)
{
    if (config.mode == ofps::sdk::WarpMode::Off) return false;
    if (ofps::sdk::BuildLayout(config, nativeW, nativeH, layout) != ofps::sdk::Status::Ok) return false;
    return layout->workWidth < nativeW || layout->workHeight < nativeH;
}

// Creates (or re-creates) the real feature behind the host's handle at the extent the layout needs.
int RecreateReal(FeatureState &st, ID3D12GraphicsCommandList *cmd, std::uint32_t w, std::uint32_t h)
{
    if (AsyncVerbose()) Log(false, "Optimizer FPS NGX hook [async] RecreateReal %ux%u begins", w, h);
    auto gate = RetirementGate(st);
    BuryAsync(st, &gate);
    if (st.realHandle != nullptr) {
        BuryReal(st.modelHost, st.realHandle, gate);
        st.realHandle = nullptr;
    }
    RetireModelPasses(st, gate);
    st.asyncTicket.Clear();
    void *handle = nullptr;
    const int result = CreateModelGuarded(st, cmd, w, h,
        st.modelResolution ? st.withholdUi : w != st.nativeWidth || h != st.nativeHeight, &handle);
    if ((result == OFPS_OK || result == OFPS_S_MODEL_NEXT_FRAME) && handle) {
        ++st.modelGeneration;
        st.realHandle = handle;
        st.createdWidth = w;
        st.createdHeight = h;
        st.warped = (w != st.nativeWidth || h != st.nativeHeight);
        st.modelPending = result == OFPS_S_MODEL_NEXT_FRAME;
    } else {
        if (handle) BuryReal(st.modelHost, handle, RetirementGate(st));
        return result < 0 ? result : OFPS_E_DEVICE;
    }
    return result;
}

bool SameLayoutConfig(const ofps::sdk::ConfigV2 &a, const ofps::sdk::ConfigV2 &b)
{
    return a.mode == b.mode && a.colorFilter == b.colorFilter &&
           std::memcmp(&a.xAxis, &b.xAxis, sizeof(a.xAxis)) == 0 &&
           std::memcmp(&a.yAxis, &b.yAxis, sizeof(a.yAxis)) == 0 &&
           a.globalScalePercent == b.globalScalePercent && a.flags == b.flags &&
           a.centerOffsetXPercent == b.centerOffsetXPercent && a.centerOffsetYPercent == b.centerOffsetYPercent &&
           a.workShiftXPercent == b.workShiftXPercent && a.workShiftYPercent == b.workShiftYPercent;
}

// Follows the overlay: a layout change re-creates the model at the new extent and rebuilds the GPU
// objects; with `force` the current configuration is applied even if it matches the recorded one
// (a feature adopted at its first evaluate). Returns an NGX result; OFPS_OK when nothing failed.
int ApplyLayout(FeatureState &st, ID3D12GraphicsCommandList *cmd, const ofps::sdk::ConfigV2 &config, bool force)
{
    const bool rebuild = st.rebuildRequested;
    st.rebuildRequested = false;
    force = force || rebuild;
    if (!force && SameLayoutConfig(config, st.config)) return OFPS_OK;
    st.config = config;
    ofps::sdk::LayoutV2 layout{};
    // A host frame the warp cannot take (host_shape.h) keeps the model at the feature's size.
    const bool warp = st.hostFits && DesiredLayout(config, st.nativeWidth, st.nativeHeight, &layout);
    const std::uint32_t w = warp ? layout.workWidth : st.nativeWidth;
    const std::uint32_t h = warp ? layout.workHeight : st.nativeHeight;
    const bool extentChanged = rebuild || w != st.createdWidth || h != st.createdHeight || st.realHandle == nullptr;
    const bool sameLayout = warp && !extentChanged && !st.disabled &&
                            std::memcmp(&layout, &st.layout, sizeof(layout)) == 0;
    if (sameLayout) return OFPS_OK;
    // The GPU objects follow the layout (filters and flags included); the model follows the
    // extent. Both are dropped only after the GPU has finished with them.
    if (warp) st.layout = layout;
    RetireGpu(st);
    st.disabled = false;
    if (extentChanged) {
        const int r = RecreateReal(st, cmd, w, h);
        Ctx().status.lastNgxResult = r;
        Ctx().status.workWidth = w;
        Ctx().status.workHeight = h;
        Log(r == OFPS_OK ? false : true, "Optimizer FPS NGX hook: layout changed, model re-created at %ux%u (%d)", w,
            h, st.lastCreateHostResult);
        if (r < 0) return r;
        st.forcedReset = true;
    } else if (warp) {
        Log(false, "Optimizer FPS NGX hook: layout changed, GPU objects rebuilt (model extent unchanged)");
    }
    return OFPS_OK;
}

int CreateModelGuarded(FeatureState &st, ID3D12GraphicsCommandList *cmd,
                      uint32_t w, uint32_t h, uint32_t withholdUi, void **handle)
{
    __try {
        const int result = CallbackCpp([&] { return st.modelHost->CreateModel(cmd, w, h, withholdUi, handle); }, OFPS_E_STATE);
        st.lastCreateHostResult = result;
        return result;
    } __except (RecordCrash(GetExceptionInformation(), StageModel)) {
        st.lastCreateHostResult = OFPS_E_DEVICE;
        return OFPS_E_DEVICE;
    }
}
} // namespace ofps::core
