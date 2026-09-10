#include "hook_dispatch.h"

#include "async_scheduler.h"
#include "debug_readback.h"
#include "hook_context.h"
#include "host_depth_state.h"
#include "ngx_params.h"
#include "temporal_controller.h"
#include "timing.h"
#include "warp_recorder.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>

namespace pwhook {

bool DesiredLayout(const pw::ConfigV2 &config, std::uint32_t nativeW, std::uint32_t nativeH, pw::LayoutV2 *layout)
{
    if (config.mode == pw::WarpMode::Off) return false;
    if (pw::BuildLayout(config, nativeW, nativeH, layout) != pw::Status::Ok) return false;
    return layout->workWidth < nativeW || layout->workHeight < nativeH;
}

// Creates (or re-creates) the real feature behind the host's handle at the extent the layout needs.
int RecreateReal(FeatureState &st, ID3D12GraphicsCommandList *cmd, std::uint32_t w, std::uint32_t h)
{
    if (AsyncVerbose()) Log(false, "Optimizer FPS NGX hook [async] RecreateReal %ux%u begins", w, h);
    pwngx::GateSet gate;
    BuryAsync(st, &gate); // a background pass may still be running the model
    // Buried earlier by RetireGpu: the ticket outlives ReleaseGpu and is consumed here.
    if (gate.Empty() && !st.asyncTicket.Empty()) { gate = st.asyncTicket; st.asyncTicket.Clear(); }
    if (st.realHandle != nullptr && Ctx().realRelease != nullptr) {
        // The model owns history and scratch the GPU may still be working on.
        if (!gate.Empty()) BuryReal(st.realHandle, gate);
        else {
            pwngx::GateSet queueGate = pwngx::SignalGate(st.realDevice, st.device);
            if (!queueGate.Empty()) BuryReal(st.realHandle, queueGate);
            else if (WaitForGpu(st.realDevice, st.device)) CallRelease(st.realHandle);
            else BuryReal(st.realHandle);
        }
        st.realHandle = nullptr;
    }
    WriteSizes(st.params, w, h);
    // The host's UI/backbuffer resources stay native-sized while the model works on the packed frame,
    // so the model's UI correction is switched off for a warped feature (its UI inputs are withheld).
    unsigned int uiCorrection = 0;
    const bool hadUiCorrection = GetUInt(st.params, "DLSSNR.UICorrection", &uiCorrection);
    if (hadUiCorrection && (w != st.nativeWidth || h != st.nativeHeight)) SetUInt(st.params, "DLSSNR.UICorrection", 0);
    void *handle = nullptr;
    const int result = CallCreate(cmd, kFeatureNeuralRendering, st.params, &handle);
    WriteSizes(st.params, st.nativeWidth, st.nativeHeight);
    if (hadUiCorrection) SetUInt(st.params, "DLSSNR.UICorrection", uiCorrection);
    if (result == kNgxSuccess) {
        st.realHandle = handle;
        st.createdWidth = w;
        st.createdHeight = h;
        st.warped = (w != st.nativeWidth || h != st.nativeHeight);
    }
    return result;
}

int __cdecl HookCreate(ID3D12GraphicsCommandList *cmd, int featureId, void *params, void **outHandle)
{
    if (Ctx().safeMode || featureId != kFeatureNeuralRendering || params == nullptr || outHandle == nullptr || Ctx().getConfig == nullptr) {
        return CallCreate(cmd, featureId, params, outHandle);
    }
    std::lock_guard<std::mutex> lock(Ctx().mutex);
    unsigned int nativeW = 0, nativeH = 0;
    if (!GetUInt(params, "DLSSNR.Width", &nativeW) || !GetUInt(params, "DLSSNR.Height", &nativeH) || nativeW == 0 ||
        nativeH == 0) {
        Log(true, "Optimizer FPS NGX hook: feature 18 create without DLSSNR.Width/Height; forwarded untouched");
        return CallCreate(cmd, featureId, params, outHandle);
    }
    auto st = std::make_unique<FeatureState>();
    st->params = params;
    st->nativeWidth = nativeW;
    st->nativeHeight = nativeH;
    st->config = Ctx().getConfig();
    pw::LayoutV2 layout{};
    bool warp = DesiredLayout(st->config, nativeW, nativeH, &layout);
    // 26.7.2: a second NR consumer in the same process (Control: renodx-control-rr's direct NR plus renodx-dlss5's
    // inline NR) gets a plain pass-through feature. Two warped features fed by two different hosts crashed inside
    // D3D12 a second after the first warped frame; one warped feature per process is the safe default.
    if (warp && !Ctx().features.empty()) {
        Log(true, "Optimizer FPS NGX hook: another NR feature is alive in this process; this one is left to its host untouched (created through the original entry point) until it is the only one left");
        const int r = Ctx().realCreate(cmd, featureId, params, outHandle);
        if (r == kNgxSuccess && *outHandle != nullptr) Ctx().foreign.insert(*outHandle);
        else Log(true, "Optimizer FPS NGX hook: the second consumer's feature 18 create returned %d through the original entry point", r);
        Ctx().status.lastNgxResult = r;
        return r;
    }
    if (warp) st->layout = layout;
    int result = RecreateReal(*st, cmd, warp ? layout.workWidth : nativeW, warp ? layout.workHeight : nativeH);
    if (result != kNgxSuccess && warp) {
        Log(true, "Optimizer FPS NGX hook: feature 18 create failed (%d) at %ux%u; creating at native size", result,
            layout.workWidth, layout.workHeight);
        st->disabled = true;
        result = RecreateReal(*st, cmd, nativeW, nativeH);
    }
    Ctx().status.lastNgxResult = result;
    if (result != kNgxSuccess) {
        Log(true, "Optimizer FPS NGX hook: feature 18 create failed (%d)", result);
        return result;
    }
    *outHandle = st->realHandle; // the host keeps this value; the real handle may be replaced later
    Ctx().status.featureCreated = true;
    Ctx().status.nativeWidth = nativeW;
    Ctx().status.nativeHeight = nativeH;
    Ctx().status.workWidth = warp ? layout.workWidth : nativeW;
    Ctx().status.workHeight = warp ? layout.workHeight : nativeH;
    Log(false, "Optimizer FPS NGX hook: feature 18 created, native %ux%u, model %ux%u (%s)", nativeW, nativeH,
        Ctx().status.workWidth, Ctx().status.workHeight, warp ? "warped" : "pass-through");
    Ctx().features[*outHandle] = std::move(st);
    return result;
}

int __cdecl HookRelease(void *handle)
{
    std::lock_guard<std::mutex> lock(Ctx().mutex);
    if (Ctx().foreign.erase(handle) != 0) return Ctx().realRelease(handle);
    auto it = Ctx().features.find(handle);
    if (it == Ctx().features.end()) return CallRelease(handle);
    FeatureState &st = *it->second;
    void *real = st.realHandle;
    st.realHandle = nullptr;
    pwngx::GateSet gate;
    BuryAsync(st, &gate);
    if (gate.Empty() && !st.asyncTicket.Empty()) { gate = st.asyncTicket; st.asyncTicket.Clear(); }
    if (!gate.Empty()) {
        // A background pass may still run the model: everything waits in the graveyard for its fence.
        BuryGpu(st, gate);
        BuryReal(real, gate);
        Ctx().features.erase(it);
        Ctx().status.featureCreated = !Ctx().features.empty();
        DrainGraveyard(false);
        return kNgxSuccess;
    }
    {
        pwngx::GateSet queueGate = pwngx::SignalGate(st.realDevice, st.device);
        if (!queueGate.Empty()) {
            BuryGpu(st, queueGate);
            if (real) BuryReal(real, queueGate);
            Ctx().features.erase(it);
            Ctx().status.featureCreated = !Ctx().features.empty();
            DrainGraveyard(false);
            return kNgxSuccess;
        }
    }
    const bool idle = WaitForGpu(st.realDevice, st.device);
    if (idle) st.ReleaseGpu();
    else BuryGpu(st);
    Ctx().features.erase(it);
    Ctx().status.featureCreated = !Ctx().features.empty();
    DrainGraveyard(idle);
    if (real == nullptr) return kNgxSuccess;
    if (!idle) {
        BuryReal(real);
        return kNgxSuccess;
    }
    return CallRelease(real);
}

bool SameLayoutConfig(const pw::ConfigV2 &a, const pw::ConfigV2 &b)
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
// (a feature adopted at its first evaluate). Returns an NGX result; kNgxSuccess when nothing failed.
int ApplyLayout(FeatureState &st, ID3D12GraphicsCommandList *cmd, void *params, const pw::ConfigV2 &config, bool force)
{
    if (!force && SameLayoutConfig(config, st.config)) return kNgxSuccess;
    st.config = config;
    pw::LayoutV2 layout{};
    const bool warp = DesiredLayout(config, st.nativeWidth, st.nativeHeight, &layout);
    const std::uint32_t w = warp ? layout.workWidth : st.nativeWidth;
    const std::uint32_t h = warp ? layout.workHeight : st.nativeHeight;
    const bool extentChanged = w != st.createdWidth || h != st.createdHeight || st.realHandle == nullptr;
    const bool sameLayout = warp && !extentChanged && !st.disabled &&
                            std::memcmp(&layout, &st.layout, sizeof(layout)) == 0;
    if (sameLayout) return kNgxSuccess;
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
        Log(r == kNgxSuccess ? false : true, "Optimizer FPS NGX hook: layout changed, model re-created at %ux%u (%d)", w,
            h, r);
        if (r != kNgxSuccess) return r;
        SetUInt(params, "DLSSNR.Reset", 1);
    } else if (warp) {
        Log(false, "Optimizer FPS NGX hook: layout changed, GPU objects rebuilt (model extent unchanged)");
    }
    return kNgxSuccess;
}

// A feature-18 handle the hook has never seen was created before the hooks were installed (the
// host pre-creates its model while Detours may still be failing to attach). It is taken over here:
// the host's handle becomes the key, the model behind it is re-created at the work extent when the
// layout asks for it, exactly as for a feature created through HookCreate.
FeatureState *AdoptFeature(void *handle, void *params)
{
    unsigned int nativeW = 0, nativeH = 0;
    if (!GetUInt(params, "DLSSNR.Width", &nativeW) || !GetUInt(params, "DLSSNR.Height", &nativeH) || nativeW == 0 ||
        nativeH == 0)
        return nullptr;
    auto st = std::make_unique<FeatureState>();
    st->realHandle = handle;
    st->params = params;
    st->nativeWidth = nativeW;
    st->nativeHeight = nativeH;
    st->createdWidth = nativeW;
    st->createdHeight = nativeH;
    st->warped = false;
    st->config = Ctx().getConfig();
    Ctx().status.featureCreated = true;
    Ctx().status.adopted = true;
    Ctx().status.nativeWidth = nativeW;
    Ctx().status.nativeHeight = nativeH;
    Ctx().status.workWidth = nativeW;
    Ctx().status.workHeight = nativeH;
    Log(false, "Optimizer FPS NGX hook: feature 18 adopted (created before the hooks were installed), native %ux%u",
        nativeW, nativeH);
    FeatureState *raw = st.get();
    Ctx().features[handle] = std::move(st);
    return raw;
}

int __cdecl HookEvaluate(ID3D12GraphicsCommandList *cmd, void *handle, void *params, void *callback)
{
    std::unique_lock<std::mutex> lock(Ctx().mutex);
    ++Ctx().evalCounter;
    DrainGraveyard(false);
    auto it = Ctx().features.find(handle);
    bool adoptedNow = false;
    if (it == Ctx().features.end() && !Ctx().foreign.empty() && Ctx().foreign.count(handle)) {
        if (Ctx().features.empty() && !Ctx().safeMode) {
            Ctx().foreign.erase(handle); // the warped host's feature is gone: this one may be adopted below and take the layout
            Log(false, "Optimizer FPS NGX hook: the other NR feature is gone; the second consumer's feature is adopted now");
        } else {
            lock.unlock();
            return Ctx().realEvaluate(cmd, handle, params, callback);
        }
    }
    if (Ctx().safeMode && it == Ctx().features.end()) {
        lock.unlock();
        SetReason("crash guard: the previous session of this game ended right after the first warped frame; everything is forwarded untouched (Retry in the tab)");
        return CallEvaluate(cmd, handle, params, callback);
    }
    if (it == Ctx().features.end() && handle != nullptr && params != nullptr && cmd != nullptr && Ctx().getConfig != nullptr) {
        if (AdoptFeature(handle, params) != nullptr) {
            it = Ctx().features.find(handle);
            adoptedNow = true;
        }
    }
    if (it == Ctx().features.end() || params == nullptr || cmd == nullptr) {
        lock.unlock();
        return CallEvaluate(cmd, handle, params, callback);
    }
    FeatureState &st = *it->second;
    st.params = params;

    // DLSSNR.Reset is set for the model after a re-creation; the block goes back to the host with the
    // host's own value once the evaluate is done (a host that does not rewrite the key every frame
    // would otherwise see our 1 forever and the temporal modes would run full frames only).
    struct ResetRestore {
        void *params; unsigned int value = 0; bool had = false;
        explicit ResetRestore(void *p) : params(p) { had = GetUInt(params, "DLSSNR.Reset", &value); }
        ~ResetRestore()
        {
            unsigned int now = 0;
            if (GetUInt(params, "DLSSNR.Reset", &now) && now != (had ? value : 0u)) SetUInt(params, "DLSSNR.Reset", had ? value : 0u);
        }
    } resetRestore(params);

    const bool forceLayout = adoptedNow;
    const int applied = ApplyLayout(st, cmd, params, Ctx().getConfig(), forceLayout);
    if (applied != kNgxSuccess) {
        char why[96];
        std::snprintf(why, sizeof(why), "the layout could not be applied (model re-creation returned %d)", applied);
        FallbackFromParams(st, cmd, params, why);
        return applied;
    }

    if (st.realHandle == nullptr) {
        FallbackFromParams(st, cmd, params, "no model (creation failed)");
        return Ctx().status.lastNgxResult != kNgxSuccess ? Ctx().status.lastNgxResult : 0;
    }

    if (st.disabled && st.warped) {
        // A previous frame gave up on the warp; the model still expects the work frame.
        RetireGpu(st);
        const int r = RecreateReal(st, cmd, st.nativeWidth, st.nativeHeight);
        Log(r == kNgxSuccess ? false : true, "Optimizer FPS NGX hook: model re-created at native size after a failure (%d)", r);
        if (r != kNgxSuccess) { FallbackFromParams(st, cmd, params, "the native model could not be re-created"); return r; }
        SetUInt(params, "DLSSNR.Reset", 1);
    }

    const int effectiveMode = EffectiveTemporalMode(st);
    if (!st.warped && !st.disabled && effectiveMode == 3) {
        const int handled = AsyncTemporalEvaluate(st, cmd, params, callback, nullptr);
        if (handled != kNotHandled) return handled;
    }
    if (!st.warped && !st.disabled && effectiveMode != 0 && effectiveMode != 3) {
        const int handled = NativeTemporalEvaluate(st, cmd, params, callback);
        if (handled != kNotHandled) return handled;
    }

    if (!st.warped || st.disabled) {
        ++Ctx().status.passthroughs;
        Ctx().status.active = false;
        if (!st.disabled) {
            if (st.config.mode == pw::WarpMode::Off) SetReason("mode is Off");
            else SetReason("the layout does not reduce the frame (work size equals native)");
        }
        void *real = st.realHandle;
        TimingBegin(st, cmd);
        lock.unlock();
        const int result = CallEvaluate(cmd, real, params, callback);
        lock.lock();
        TimingEnd(st, cmd);
        return result;
    }

    ID3D12Resource *color = GetResource(params, "DLSSNR.Color");
    ID3D12Resource *depth = GetResource(params, "DLSSNR.Depth");
    ID3D12Resource *motion = GetResource(params, "DLSSNR.MVec");
    ID3D12Resource *output = GetResource(params, "DLSSNR.Output");
    if (color == nullptr || depth == nullptr || motion == nullptr || output == nullptr) {
        SetReason("host supplied no colour/depth/motion/output");
        st.disabled = true;
        Log(true, "Optimizer FPS NGX hook: %s; re-creating the model at native size", Ctx().status.reason);
        RecreateReal(st, cmd, st.nativeWidth, st.nativeHeight);
        void *real = st.realHandle;
        lock.unlock();
        if (real == nullptr) return 0;
        return CallEvaluate(cmd, real, params, callback);
    }

    const D3D12_RESOURCE_DESC outputDesc = output->GetDesc();
    if (outputDesc.Width != st.nativeWidth || outputDesc.Height != st.nativeHeight ||
        !EnsureGpu(st, cmd, color, output)) {
        if (outputDesc.Width != st.nativeWidth || outputDesc.Height != st.nativeHeight)
            SetReason("host output is %ux%u, feature is %ux%u", (unsigned) outputDesc.Width, outputDesc.Height,
                      st.nativeWidth, st.nativeHeight);
        st.disabled = true;
        Log(true, "Optimizer FPS NGX hook: %s; re-creating the model at native size", Ctx().status.reason);
        RetireGpu(st);
        RecreateReal(st, cmd, st.nativeWidth, st.nativeHeight);
        void *real = st.realHandle;
        lock.unlock();
        if (real == nullptr) return 0;
        return CallEvaluate(cmd, real, params, callback);
    }

    // ---- read the host's description ----
    const D3D12_RESOURCE_DESC colorDesc = color->GetDesc();
    const D3D12_RESOURCE_DESC depthDesc = depth->GetDesc();
    const D3D12_RESOURCE_DESC motionDesc = motion->GetDesc();
    const Subrect colorRect = ReadSubrect(params, "Color", (unsigned) colorDesc.Width, colorDesc.Height);
    const Subrect depthRect = ReadSubrect(params, "Depth", (unsigned) depthDesc.Width, depthDesc.Height);
    const Subrect motionRect = ReadSubrect(params, "MVec", (unsigned) motionDesc.Width, motionDesc.Height);
    const Subrect outputRect = ReadSubrect(params, "Output", (unsigned) outputDesc.Width, outputDesc.Height);
    unsigned int depthInverted = 0;
    GetUInt(params, "DLSSNR.DepthInverted", &depthInverted);
    float mvScaleX = 1.0f, mvScaleY = 1.0f;
    const bool mvReadX = GetFloat(params, "DLSSNR.MVecScaleX", &mvScaleX) && std::isfinite(mvScaleX) && mvScaleX != 0.0f;
    const bool mvReadY = GetFloat(params, "DLSSNR.MVecScaleY", &mvScaleY) && std::isfinite(mvScaleY) && mvScaleY != 0.0f;
    if (!mvReadX) mvScaleX = 1.0f;
    if (!mvReadY) mvScaleY = 1.0f;
    Ctx().status.hostMotionScaleRead = mvReadX && mvReadY;
    Ctx().status.hostMotionScaleX = mvScaleX;
    Ctx().status.hostMotionScaleY = mvScaleY;
    Ctx().status.hostMotionWidth = (std::uint32_t) motionDesc.Width;
    Ctx().status.hostMotionHeight = motionDesc.Height;
    Ctx().status.hostMotionRectWidth = motionRect.w;
    Ctx().status.hostMotionRectHeight = motionRect.h;
    Ctx().status.floatGetterSlot = pwngx::FloatGetterSlot();
    ID3D12Resource *ui = GetResource(params, "DLSSNR.UI");
    ID3D12Resource *uiAlpha = GetResource(params, "DLSSNR.UIAlpha");
    ID3D12Resource *backbuffer = GetResource(params, "DLSSNR.Backbuffer");

    ++st.frame;

    // ---- temporal plan for this frame ----
    unsigned int hostReset = 0;
    GetUInt(params, "DLSSNR.Reset", &hostReset);
    TemporalPlan plan;
    if (Ctx().temporal.mode != 0 && EnsureTemporal(st, cmd, output, motion, depth)) plan = PlanTemporal(st, hostReset != 0);
    Ctx().status.temporalMode = plan.active ? EffectiveTemporalMode(st) : 0;
    if (plan.active && EffectiveTemporalMode(st) == Ctx().temporal.mode) Ctx().status.temporalReason[0] = 0;
    const bool useAcc = plan.active && plan.full && EffectiveTemporalMode(st) != 3 && st.temporal->AccValid() && !Ctx().temporal.debugSingleFrameMotion;
    ID3D12Resource *const hostMotion = motion;
    if (useAcc) motion = st.temporal->NextAcc(); // written by the accumulation recorded before Pack
    pwtemporal::FrameInputs tin = TemporalInputs(color, hostMotion, depth, st.colorView, TypedView(motionDesc.Format, false),
                                                 TypedView(depthDesc.Format, true), colorRect, motionRect, depthRect, mvScaleX,
                                                 mvScaleY, depthInverted != 0);
    // 26.26: the same depth resting state and barrier subresource the Pack barriers below use (c.depthState /
    // c.depthSub), so the accumulation and the reprojection address a planar depth-stencil host the same way.
    tin.depthState = HostDepthState(depth);
    tin.depthSubresource = pwngx::DepthBarrierSubresource(depth);

    pw::InputDescriptionV2 input = pw::DefaultInputDescriptionV2(st.nativeWidth, st.nativeHeight);
    input.colorRect = {colorRect.x, colorRect.y, colorRect.w, colorRect.h};
    input.depthRect = {depthRect.x, depthRect.y, depthRect.w, depthRect.h};
    input.motionRect = {motionRect.x, motionRect.y, motionRect.w, motionRect.h};
    input.confidenceRect = {0, 0, 1, 1};
    // The model's motion scale converts stored components to its input pixels; Pack wants native pixels.
    // The user's adjustment (overlay, Advanced) multiplies on top; a sign flip is a multiplier of -1.
    const float adjust = Ctx().motionScaleAdjust * (Ctx().motionInvert ? -1.0f : 1.0f);
    // The accumulated displacement is already in motion-texture pixels (the host's scale applied).
    input.motionScaleX = adjust * (useAcc ? 1.0f : mvScaleX) * static_cast<float>(st.nativeWidth) / static_cast<float>(std::max(1u, motionRect.w));
    input.motionScaleY = adjust * (useAcc ? 1.0f : mvScaleY) * static_cast<float>(st.nativeHeight) / static_cast<float>(std::max(1u, motionRect.h));
    input.motionDirection = pw::MotionDirection::CurrentToPrevious;
    input.depthConvention = depthInverted != 0 ? pw::DepthConvention::Reversed : pw::DepthConvention::Normal;
    input.colorEncoding = EncodingFor(st.colorView);

    const pw::D3D12SourceResources sources = {{color, st.colorView},
                                              {depth, TypedView(depthDesc.Format, true)},
                                              {motion, useAcc ? DXGI_FORMAT_R16G16_FLOAT : TypedView(motionDesc.Format, false)},
                                              {nullptr, DXGI_FORMAT_UNKNOWN}};
    // The packed textures: next slot round-robin (reuse is ordered by the host's queue). The source
    // set: a fresh ring entry every evaluate, never one the GPU may still read.
    const std::uint32_t packSlot = st.nextPackSlot;
    st.nextPackSlot = (st.nextPackSlot + 1) % FeatureState::kBgPackSlot;
    const std::uint32_t packSet = FeatureState::kPackSlots * 2 + (st.nextPackSet++ % FeatureState::kPackSetRing);
    pw::AdapterStatus status = st.adapter->WriteSourceSetV2(packSet, sources, input);
    FeatureState::InputSignature sig;
    sig.colorFormat = colorDesc.Format; sig.depthFormat = depthDesc.Format; sig.motionFormat = motionDesc.Format; sig.outputFormat = outputDesc.Format;
    sig.motionW = (std::uint32_t) motionDesc.Width; sig.motionH = motionDesc.Height;
    sig.colorRect = colorRect; sig.depthRect = depthRect; sig.motionRect = motionRect;
    // 26.20: the host's scale, not the Pack scale - the latter flips to 1 whenever the temporal chain feeds the model
    // (full frames after interpolated ones), which logged the input block on every single evaluate.
    sig.motionScaleX = mvScaleX; sig.motionScaleY = mvScaleY;
    sig.depthInverted = depthInverted; sig.colorIsOutput = color == output; sig.valid = true;
    const bool inputsChanged = !st.inputSignature.valid || std::memcmp(&st.inputSignature, &sig, sizeof(sig)) != 0;
    if (inputsChanged) {
        st.inputSignature = sig;
        Log(false, "Optimizer FPS NGX hook: host resources: colour %p fmt %d, depth %p fmt %d, motion %p fmt %d, output %p fmt %d%s (pointers may rotate every frame; logged on a change of formats/rects/scale)",
            (void *) color, (int) colorDesc.Format, (void *) depth, (int) depthDesc.Format, (void *) motion, (int) motionDesc.Format, (void *) output, (int) outputDesc.Format,
            color == output ? " (output IS the colour: in-place host)" : "");
        Log(false, "Optimizer FPS NGX hook: host motion input: texture %ux%u fmt %d, subrect %u,%u %ux%u, MVecScale %.4f x %.4f (%s, float setter slot %d getter slot %d), depth inverted %u, colour subrect %ux%u; Pack scale %.4f x %.4f (adjust %.3f%s)",
            (unsigned) motionDesc.Width, motionDesc.Height, (int) motionDesc.Format, motionRect.x, motionRect.y, motionRect.w,
            motionRect.h, mvScaleX, mvScaleY, (mvReadX && mvReadY) ? "read" : "NOT READ, defaulted to 1", pwngx::FloatSetterSlot(), pwngx::FloatGetterSlot(),
            depthInverted, colorRect.w, colorRect.h, input.motionScaleX, input.motionScaleY, Ctx().motionScaleAdjust.load(),
            Ctx().motionInvert.load() ? ", inverted" : "");
        if (!(mvReadX && mvReadY)) {
            char probe[512];
            ProbeKey(params, "DLSSNR.MVecScaleX", probe, sizeof(probe));
            Log(false, "Optimizer FPS NGX hook: DLSSNR.MVecScaleX getter probe: %s", probe);
        }
        {
            unsigned int uiCorrection = 0, enabled = 0, style = 0, autoMask = 0;
            float intensity = 0.0f, localTone = 0.0f, localStructure = 0.0f, paperWhite = 0.0f;
            const bool hasUiCorrection = GetUInt(params, "DLSSNR.UICorrection", &uiCorrection);
            GetUInt(params, "DLSSNR.Enabled", &enabled);
            GetUInt(params, "DLSSNR.Style", &style);
            GetUInt(params, "DLSSNR.UseAutoMask", &autoMask);
            GetFloat(params, "DLSSNR.Intensity", &intensity);
            GetFloat(params, "DLSSNR.LocalToneStrength", &localTone);
            GetFloat(params, "DLSSNR.LocalStructureStrength", &localStructure);
            const bool hasPaperWhite = GetFloat(params, "DLSSNR.PaperWhiteNits", &paperWhite) || GetFloat(params, "DLSSNR.PaperWhite", &paperWhite);
            Log(false, "Optimizer FPS NGX hook: host model inputs: UI %p, UIAlpha %p, Backbuffer %p (%s), UICorrection %u (%s), Enabled %u, Style %u, AutoMask %u, Intensity %.3f, LocalTone %.3f, LocalStructure %.3f, PaperWhite %.3f (%s)",
                (void *) ui, (void *) uiAlpha, (void *) backbuffer, KeepBackbufferEnabled() ? "kept for the warped model (debug)" : "withheld from the warped model",
                uiCorrection, hasUiCorrection ? "set" : "unset", enabled, style, autoMask, intensity, localTone, localStructure, paperWhite,
                hasPaperWhite ? "set" : "unset");
        }
    }
    const std::uint32_t slot = packSlot;
    const std::uint32_t unpackSlot = FeatureState::kPackSlots + packSlot;
    if (status != pw::AdapterStatus::Ok) {
        ID3D12Device *colorDevice = nullptr, *depthDevice = nullptr, *motionDevice = nullptr;
        color->GetDevice(IID_PPV_ARGS(&colorDevice));
        depth->GetDevice(IID_PPV_ARGS(&depthDevice));
        motion->GetDevice(IID_PPV_ARGS(&motionDevice));
        SetReason("pack descriptors: %s (colour %d %ux%u mips %u flags %x, depth %d %ux%u mips %u flags %x, motion %d %ux%u mips %u flags %x, rects %ux%u/%ux%u/%ux%u, devices %p/%p/%p vs %p)",
                  pw::AdapterStatusString(status), (int) colorDesc.Format, (unsigned) colorDesc.Width, colorDesc.Height,
                  colorDesc.MipLevels, (unsigned) colorDesc.Flags, (int) depthDesc.Format, (unsigned) depthDesc.Width,
                  depthDesc.Height, depthDesc.MipLevels, (unsigned) depthDesc.Flags, (int) motionDesc.Format,
                  (unsigned) motionDesc.Width, motionDesc.Height, motionDesc.MipLevels, (unsigned) motionDesc.Flags,
                  colorRect.w, colorRect.h, depthRect.w, depthRect.h, motionRect.w, motionRect.h, (void *) colorDevice,
                  (void *) depthDevice, (void *) motionDevice, (void *) st.device);
        if (colorDevice) colorDevice->Release();
        if (depthDevice) depthDevice->Release();
        if (motionDevice) motionDevice->Release();
        st.disabled = true;
        Log(true, "Optimizer FPS NGX hook: %s; re-creating the model at native size", Ctx().status.reason);
        RetireGpu(st);
        RecreateReal(st, cmd, st.nativeWidth, st.nativeHeight);
        void *real = st.realHandle;
        lock.unlock();
        if (real == nullptr) return 0;
        return CallEvaluate(cmd, real, params, callback);
    }

    if (cmd->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT) {
        SetReason("host command list is type %d; Pack/Unpack need a direct list", (int) cmd->GetType());
        st.disabled = true;
        Log(true, "Optimizer FPS NGX hook: %s; re-creating the model at native size", Ctx().status.reason);
        RetireGpu(st);
        RecreateReal(st, cmd, st.nativeWidth, st.nativeHeight);
        void *real = st.realHandle;
        lock.unlock();
        if (real == nullptr) return 0;
        return CallEvaluate(cmd, real, params, callback);
    }

    // 26.21: the crash guard's marker is written here, before anything of the warped path is recorded.
    if (st.warped) NotifyFirstWarped();

    EvalContext c{};
    c.st = &st;
    c.cmd = cmd;
    c.params = params;
    c.callback = callback;
    c.color = color;
    c.depth = depth;
    c.depthState = HostDepthState(depth);
    c.depthSub = pwngx::DepthBarrierSubresource(depth);
    c.motion = motion;
    c.hostMotion = hostMotion;
    c.output = output;
    c.ui = ui;
    c.uiAlpha = uiAlpha;
    c.backbuffer = backbuffer;
    c.colorRect = colorRect;
    c.depthRect = depthRect;
    c.motionRect = motionRect;
    c.outputRect = outputRect;
    c.mvScaleX = mvScaleX;
    c.mvScaleY = mvScaleY;
    c.slot = slot;
    c.packSlot = packSlot;
    c.packSet = packSet;
    c.unpackSlot = unpackSlot;
    c.depthConvention = input.depthConvention;
    c.temporalActive = plan.active;
    c.temporalFull = plan.full;
    c.accumulate = plan.active && (!plan.full || useAcc);
    c.motionIsAcc = useAcc;
    c.tin = tin;
    c.wantBase = plan.active && Ctx().temporal.warpBase && st.unpackBase != nullptr;
    c.baseTarget = st.unpackBase;
    c.baseTargetState = &st.unpackBaseState;
    if (st.rtvHeap) c.baseRtv = BaseRtv(st);

    // Background mode in the warped path: the host context carries the pack slot for the base.
    if (EffectiveTemporalMode(st) == 3 && plan.active) {
        const int handled = AsyncTemporalEvaluate(st, cmd, params, callback, &c);
        if (handled != kNotHandled) return handled;
    }

    if (plan.active && !plan.full) {
        // Interpolated frame: the model rests.
        const int r = InterpolateGuarded(c);
        DumpDebugLayer(st.realDevice);
        TemporalDebugReadback(st, cmd, c.tin);
        if (r == kCrashed) {
            st.temporalDisabled = true;
            TemporalReason(st, "exception 0x%08lX in stage %s; temporal mode disabled for this feature", Ctx().crash.code, StageName(Ctx().crash.stage));
            Log(true, "Optimizer FPS NGX hook: exception 0x%08lX at %p (%s) during stage %d (%s); temporal mode disabled",
                Ctx().crash.code, Ctx().crash.address, Ctx().crash.module, Ctx().crash.stage, StageName(Ctx().crash.stage));
            FallbackOutput(c, "exception on an interpolated frame");
            return kNgxSuccess; // the next frame runs the model
        }
        if (r != kNgxSuccess) FallbackOutput(c, "interpolated frame failed");
        ++Ctx().status.evaluations;
        Ctx().status.active = true;
        Ctx().status.reason[0] = 0;
        TemporalFrameDone(st, false);
        return kNgxSuccess;
    }

    const int result = WarpedGuarded(c);
    DumpDebugLayer(st.realDevice);
    if (plan.active && result == kNgxSuccess) TemporalFrameDone(st, plan.full);
    if (result == kCrashed) {
        // Something faulted inside the warped path. Give the block back to the host as far as that
        // is possible, stop warping this feature, and let the next evaluate rebuild the native model.
        st.disabled = true;
        Ctx().status.active = false;
        RestoreGuarded(c);
        Log(true, "Optimizer FPS NGX hook: exception 0x%08lX at %p (%s) during stage %d (%s); warp disabled for this feature",
            Ctx().crash.code, Ctx().crash.address, Ctx().crash.module, Ctx().crash.stage, StageName(Ctx().crash.stage));
        SetReason("exception 0x%08lX in stage %s; warp disabled", Ctx().crash.code, StageName(Ctx().crash.stage));
        FallbackOutput(c, "exception on a warped frame");
        return 0;
    }
    if (result == kPackFailed) {
        st.disabled = true;
        Log(true, "Optimizer FPS NGX hook: %s; re-creating the model at native size", Ctx().status.reason);
        RetireGpu(st);
        RecreateReal(st, cmd, st.nativeWidth, st.nativeHeight);
        void *realNative = st.realHandle;
        lock.unlock();
        if (realNative == nullptr) return 0;
        return CallEvaluate(cmd, realNative, params, callback);
    }
    return result;
}

} // namespace pwhook
