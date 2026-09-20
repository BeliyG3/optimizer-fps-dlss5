#include "spread_passes.h"

#include "feature_state.h"
#include "hook_context.h"
#include "model_passes.h"
#include "ngx_params.h"
#include "timing.h"
#include "warp_recorder.h"

namespace pwhook {
int SpreadModel(FeatureState &st, ID3D12GraphicsCommandList *cmd, void *params, void *callback,
                const pwtemporal::FrameInputs &in, ID3D12Resource *input, pwtemporal::Machine &stage)
{
    auto *hostColor = GetResource(params, "DLSSNR.Color");
    const auto cd = hostColor->GetDesc();
    const auto originalRect = ReadSubrect(params, "Color", static_cast<unsigned>(cd.Width), cd.Height);
    auto *vectors = in.motion;
    bool accumulated = stage.AccValid() && !Ctx().temporal.debugSingleFrameMotion;
    if (accumulated) {
        vectors = stage.Acc();
        if (!Ctx().diag.temporalNoModelMotion && stage.ModelMv()) {
            stage.RecordModelMotion(cmd, in);
            vectors = stage.ModelMv();
        }
    }
    int result = kNgxSuccess;
    if (!st.warped) {
        SetResource(params, "DLSSNR.Color", input);
        WriteSubrect(params, "Color", 0, 0, st.nativeWidth, st.nativeHeight);
        SetResource(params, "DLSSNR.MVec", vectors);
        if (accumulated) {
            SetFloat(params, "DLSSNR.MVecScaleX", 1);
            SetFloat(params, "DLSSNR.MVecScaleY", 1);
        }
        TimingBegin(st, cmd);
        result = EvaluateModelPasses(st, cmd, params, callback);
        TimingEnd(st, cmd);
    } else {
        EvalContext c{};
        c.st = &st; c.cmd = cmd; c.params = params; c.callback = callback;
        c.color = input; c.depth = in.depth; c.motion = vectors; c.hostMotion = in.motion;
        c.output = GetResource(params, "DLSSNR.Output");
        c.ui = GetResource(params, "DLSSNR.UI");
        c.uiAlpha = GetResource(params, "DLSSNR.UIAlpha");
        c.backbuffer = GetResource(params, "DLSSNR.Backbuffer");
        const auto od = c.output->GetDesc();
        c.colorRect = {0, 0, st.nativeWidth, st.nativeHeight};
        c.depthRect = {in.depthRect.x, in.depthRect.y, in.depthRect.w, in.depthRect.h};
        c.motionRect = {in.motionRect.x, in.motionRect.y, in.motionRect.w, in.motionRect.h};
        c.outputRect = ReadSubrect(params, "Output", static_cast<unsigned>(od.Width), od.Height);
        c.depthState = in.depthState; c.depthSub = in.depthSubresource;
        c.mvScaleX = in.mvScaleX; c.mvScaleY = in.mvScaleY;
        c.motionIsAcc = accumulated;
        c.depthConvention = in.depthInverted ? pw::DepthConvention::Reversed : pw::DepthConvention::Normal;
        c.slot = c.packSlot = st.nextPackSlot;
        st.nextPackSlot = (st.nextPackSlot + 1) % FeatureState::kBgPackSlot;
        c.packSet = FeatureState::kPackSlots * 2 + (st.nextPackSet++ % FeatureState::kPackSetRing);
        c.unpackSlot = FeatureState::kPackSlots + c.packSlot;
        auto description = pw::DefaultInputDescriptionV2(st.nativeWidth, st.nativeHeight);
        description.colorRect = {0, 0, st.nativeWidth, st.nativeHeight};
        description.depthRect = {in.depthRect.x, in.depthRect.y, in.depthRect.w, in.depthRect.h};
        description.motionRect = {in.motionRect.x, in.motionRect.y, in.motionRect.w, in.motionRect.h};
        description.confidenceRect = {0, 0, 1, 1};
        const float adjust = Ctx().motionScaleAdjust * (Ctx().motionInvert ? -1.0f : 1.0f);
        description.motionScaleX = adjust * (accumulated ? 1.0f : in.mvScaleX) * static_cast<float>(st.nativeWidth) / in.motionRect.w;
        description.motionScaleY = adjust * (accumulated ? 1.0f : in.mvScaleY) * static_cast<float>(st.nativeHeight) / in.motionRect.h;
        description.motionDirection = pw::MotionDirection::CurrentToPrevious;
        description.depthConvention = c.depthConvention;
        description.colorEncoding = EncodingFor(in.colorView);
        const pw::D3D12SourceResources sources = {{input, in.colorView}, {in.depth, in.depthView},
            {vectors, accumulated ? DXGI_FORMAT_R16G16_FLOAT : in.motionView}, {nullptr, DXGI_FORMAT_UNKNOWN}};
        if (st.adapter->WriteSourceSetV2(c.packSet, sources, description) != pw::AdapterStatus::Ok) return kPackFailed;
        NotifyFirstWarped();
        result = WarpedGuarded(c);
        if (result == kCrashed) RestoreGuarded(c);
    }
    SetResource(params, "DLSSNR.Color", hostColor);
    WriteSubrect(params, "Color", originalRect.x, originalRect.y, originalRect.w, originalRect.h);
    SetResource(params, "DLSSNR.MVec", in.motion);
    SetFloat(params, "DLSSNR.MVecScaleX", in.mvScaleX);
    SetFloat(params, "DLSSNR.MVecScaleY", in.mvScaleY);
    return result;
}
} // namespace pwhook
