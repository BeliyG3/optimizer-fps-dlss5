#include "warp_recorder.h"

#include "hook_context.h"
#include "host_depth_state.h"
#include "ngx_params.h"
#include "temporal_controller.h"
#include "timing.h"

#include <algorithm>
#include <cstdio>

namespace pwhook {

D3D12_CPU_DESCRIPTOR_HANDLE BaseRtv(const FeatureState &st)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h = st.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += st.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    return h;
}

void RestoreParams(EvalContext &c)
{
    FeatureState &st = *c.st;
    SetResource(c.params, "DLSSNR.Color", c.color);
    SetResource(c.params, "DLSSNR.Depth", c.depth);
    SetResource(c.params, "DLSSNR.MVec", c.hostMotion != nullptr ? c.hostMotion : c.motion);
    SetResource(c.params, "DLSSNR.Output", c.output);
    if (c.ui) SetResource(c.params, "DLSSNR.UI", c.ui);
    if (c.uiAlpha) SetResource(c.params, "DLSSNR.UIAlpha", c.uiAlpha);
    if (c.backbuffer) SetResource(c.params, "DLSSNR.Backbuffer", c.backbuffer);
    WriteSizes(c.params, st.nativeWidth, st.nativeHeight);
    WriteSubrect(c.params, "Color", c.colorRect.x, c.colorRect.y, c.colorRect.w, c.colorRect.h);
    WriteSubrect(c.params, "Depth", c.depthRect.x, c.depthRect.y, c.depthRect.w, c.depthRect.h);
    WriteSubrect(c.params, "MVec", c.motionRect.x, c.motionRect.y, c.motionRect.w, c.motionRect.h);
    WriteSubrect(c.params, "Output", c.outputRect.x, c.outputRect.y, c.outputRect.w, c.outputRect.h);
    SetFloat(c.params, "DLSSNR.MVecScaleX", c.mvScaleX);
    SetFloat(c.params, "DLSSNR.MVecScaleY", c.mvScaleY);
}

// Pack: the host's native inputs into the slot's packed textures (colour / depth / motion).
int RecordPackStage(EvalContext &c)
{
    FeatureState &st = *c.st;
    ID3D12GraphicsCommandList *cmd = c.cmd;
    const std::uint32_t slot = c.slot;
    c.stage = StagePackBarriers;
    const pw::D3D12PackedViews packed = st.adapter->PackedViews(c.packSlot);
    BarrierExternal(cmd, c.color, kHostInputState, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    BarrierExternal(cmd, c.depth, c.depthState, c.depthState | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, c.depthSub);
    if (!c.motionIsAcc) BarrierExternal(cmd, c.motion, kHostInputState, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Barrier(cmd, packed.resources.color.resource, st.packedColorState[slot], D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_RESOURCE_STATES guideState = st.packedGuideState[slot];
    Barrier(cmd, packed.resources.depth.resource, guideState, D3D12_RESOURCE_STATE_RENDER_TARGET);
    guideState = st.packedGuideState[slot];
    Barrier(cmd, packed.resources.motion.resource, guideState, D3D12_RESOURCE_STATE_RENDER_TARGET);
    st.packedGuideState[slot] = D3D12_RESOURCE_STATE_RENDER_TARGET;
    c.stage = StagePackDraw;
    pw::AdapterStatus status = st.adapter->RecordPackFromSet(cmd, c.packSlot, c.packSet);
    c.stage = StagePackRestore;
    BarrierExternal(cmd, c.color, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, kHostInputState);
    BarrierExternal(cmd, c.depth, c.depthState | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, c.depthState, c.depthSub);
    if (!c.motionIsAcc) BarrierExternal(cmd, c.motion, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, kHostInputState);
    Barrier(cmd, packed.resources.color.resource, st.packedColorState[slot], kHostInputState);
    guideState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    Barrier(cmd, packed.resources.depth.resource, guideState, kHostInputState);
    guideState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    Barrier(cmd, packed.resources.motion.resource, guideState, kHostInputState);
    st.packedGuideState[slot] = kHostInputState;
    if (status != pw::AdapterStatus::Ok) {
        SetReason("pack draw: %s", pw::AdapterStatusString(status));
        return kPackFailed;
    }
    return kNgxSuccess;
}

// The slot's packed colour unpacked without the model into c.baseTarget (packed guides must be in
// PIXEL_SHADER_RESOURCE); the target rests in PIXEL_SHADER_RESOURCE afterwards.
pw::AdapterStatus RecordBaseUnpack(EvalContext &c)
{
    FeatureState &st = *c.st;
    Barrier(c.cmd, c.baseTarget, *c.baseTargetState, D3D12_RESOURCE_STATE_RENDER_TARGET);
    (void) st.adapter->SetOutputColorAdjust(Ctx().outputGain, Ctx().outputGamma);
    // The slot's textures with the constants of the set that packed them (the unpack shader reads the
    // input description too).
    const pw::AdapterStatus status = st.adapter->RecordUnpackOwnedColorFromSet(c.cmd, c.packSlot, c.packSet, c.baseRtv, pw::DiagnosticOutlineNone);
    Barrier(c.cmd, c.baseTarget, *c.baseTargetState, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    return status;
}

// Packed guides of the slot to / from PIXEL_SHADER_RESOURCE (for an unpack outside WarpedBody).
void PackedGuidesToPixel(FeatureState &st, ID3D12GraphicsCommandList *cmd, std::uint32_t slot, std::uint32_t packSlot)
{
    const pw::D3D12PackedViews packed = st.adapter->PackedViews(packSlot);
    D3D12_RESOURCE_STATES s1 = st.packedGuideState[slot];
    Barrier(cmd, packed.resources.depth.resource, s1, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    s1 = st.packedGuideState[slot];
    Barrier(cmd, packed.resources.motion.resource, s1, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    st.packedGuideState[slot] = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    Barrier(cmd, packed.resources.color.resource, st.packedColorState[slot], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

// When no path produced this frame the host's colour goes into its output, so an evaluate never
// leaves the output unwritten (the host shows whatever its texture held - black from a pool, or a
// frame from several frames back). Same formats: a copy; otherwise the temporal machine's raw-colour
// reprojection (debugVis 3 returns the colour) when the machine exists.
void FallbackOutput(FeatureState &st, ID3D12GraphicsCommandList *cmd, ID3D12Resource *color, const Subrect &colorRect,
                    ID3D12Resource *output, const Subrect &outputRect, const pwtemporal::FrameInputs *tin, const char *why)
{
    ++Ctx().status.fallbackFrames;
    std::snprintf(Ctx().status.fallbackReason, sizeof(Ctx().status.fallbackReason), "%s", why);
    if (Ctx().status.fallbackFrames <= 5 || Ctx().status.fallbackFrames % 500 == 0)
        Log(true, "Optimizer FPS NGX hook: frame %llu shown as the host's colour (%s), %llu such frames so far",
            static_cast<unsigned long long>(Ctx().evalCounter), why, static_cast<unsigned long long>(Ctx().status.fallbackFrames));
    if (cmd == nullptr || color == nullptr || output == nullptr || color == output) return;
    const D3D12_RESOURCE_DESC cd = color->GetDesc(), od = output->GetDesc();
    if (cd.Format == od.Format) {
        BarrierExternal(cmd, color, kHostInputState, D3D12_RESOURCE_STATE_COPY_SOURCE);
        BarrierExternal(cmd, output, kHostOutputState, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource = output; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION src{}; src.pResource = color; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        const UINT w = std::min<UINT>(st.nativeWidth, static_cast<UINT>(cd.Width) - std::min<UINT>(colorRect.x, static_cast<UINT>(cd.Width)));
        const UINT h = std::min<UINT>(st.nativeHeight, cd.Height - std::min<UINT>(colorRect.y, cd.Height));
        const D3D12_BOX box{colorRect.x, colorRect.y, 0, colorRect.x + w, colorRect.y + h, 1};
        cmd->CopyTextureRegion(&dst, outputRect.x, outputRect.y, 0, &src, &box);
        BarrierExternal(cmd, output, D3D12_RESOURCE_STATE_COPY_DEST, kHostOutputState);
        BarrierExternal(cmd, color, D3D12_RESOURCE_STATE_COPY_SOURCE, kHostInputState);
    } else if (st.temporal && tin != nullptr && tin->color != nullptr && tin->depth != nullptr && tin->motion != nullptr) {
        pwtemporal::FrameInputs in = *tin;
        in.base = nullptr;
        in.debugVis = 3;
        st.temporal->RecordReproject(cmd, in, output, kHostOutputState, outputRect.x, outputRect.y);
    } else {
        static bool s_once = false;
        if (!s_once) { s_once = true; Log(true, "Optimizer FPS NGX hook: no fallback for this frame (colour fmt %d, output fmt %d, no temporal machine)", (int) cd.Format, (int) od.Format); }
    }
}

void FallbackOutput(EvalContext &c, const char *why)
{
    FallbackOutput(*c.st, c.cmd, c.color, c.colorRect, c.output, c.outputRect, c.tin.color ? &c.tin : nullptr, why);
}

// Before an EvalContext exists (the layout / model could not be applied): read the host's colour and
// output straight from the block.
void FallbackFromParams(FeatureState &st, ID3D12GraphicsCommandList *cmd, void *params, const char *why)
{
    ID3D12Resource *color = GetResource(params, "DLSSNR.Color");
    ID3D12Resource *output = GetResource(params, "DLSSNR.Output");
    ID3D12Resource *depth = GetResource(params, "DLSSNR.Depth");
    ID3D12Resource *motion = GetResource(params, "DLSSNR.MVec");
    if (color == nullptr || output == nullptr) { FallbackOutput(st, cmd, nullptr, Subrect{}, nullptr, Subrect{}, nullptr, why); return; }
    const D3D12_RESOURCE_DESC cd = color->GetDesc(), od = output->GetDesc();
    const Subrect colorRect = ReadSubrect(params, "Color", static_cast<unsigned>(cd.Width), cd.Height);
    const Subrect outputRect = ReadSubrect(params, "Output", static_cast<unsigned>(od.Width), od.Height);
    pwtemporal::FrameInputs tin{};
    if (st.temporal && depth && motion) {
        const D3D12_RESOURCE_DESC dd = depth->GetDesc(), md = motion->GetDesc();
        const Subrect depthRect = ReadSubrect(params, "Depth", static_cast<unsigned>(dd.Width), dd.Height);
        const Subrect motionRect = ReadSubrect(params, "MVec", static_cast<unsigned>(md.Width), md.Height);
        tin = TemporalInputs(color, motion, depth, TypedView(cd.Format, false), TypedView(md.Format, false), TypedView(dd.Format, true),
                             colorRect, motionRect, depthRect, 1.0f, 1.0f, false);
        tin.depthState = HostDepthState(depth);
        tin.depthSubresource = pwngx::DepthBarrierSubresource(depth);
    }
    FallbackOutput(st, cmd, color, colorRect, output, outputRect, tin.color ? &tin : nullptr, why);
}

// The temporal inputs with the base colour when this frame has one.
pwtemporal::FrameInputs TemporalInputsWithBase(const EvalContext &c)
{
    pwtemporal::FrameInputs t = c.tin;
    if (c.wantBase && c.baseTarget) { t.base = c.baseTarget; t.baseState = *c.baseTargetState; }
    return t;
}

// ---------------------------------------------------------------------------------------------
// The warped evaluate body runs under a structured-exception guard: a fault anywhere in it is
// reported with its stage instead of taking the game down, and the feature falls back to native.
// ---------------------------------------------------------------------------------------------
int WarpedBody(EvalContext &c)
{
    FeatureState &st = *c.st;
    ID3D12GraphicsCommandList *cmd = c.cmd;
    void *params = c.params;
    const std::uint32_t slot = c.slot;

    // ---- temporal: accumulate the host's motion before anything reads the displacement ----
    if (c.temporalActive && c.accumulate) {
        c.stage = StageTemporalAccumulate;
        st.temporal->RecordAccumulate(cmd, c.tin);
    }

    // ---- Pack ----
    const int packResult = RecordPackStage(c);
    if (packResult != kNgxSuccess) return packResult;
    const pw::D3D12PackedViews packed = st.adapter->PackedViews(c.packSlot);
    D3D12_RESOURCE_STATES guideState = st.packedGuideState[slot];
    pw::AdapterStatus status = pw::AdapterStatus::Ok;

    // ---- model on the packed frame ----
    c.stage = StageParamsWrite;
    const std::uint32_t ww = st.layout.workWidth, wh = st.layout.workHeight;
    c.paramsRewritten = true;
    SetResource(params, "DLSSNR.Color", packed.resources.color.resource);
    SetResource(params, "DLSSNR.Depth", packed.resources.depth.resource);
    SetResource(params, "DLSSNR.MVec", packed.resources.motion.resource);
    SetResource(params, "DLSSNR.Output", st.nrOutput);
    if (!KeepBackbufferEnabled()) {
        if (c.ui) SetResource(params, "DLSSNR.UI", nullptr);
        if (c.uiAlpha) SetResource(params, "DLSSNR.UIAlpha", nullptr);
        if (c.backbuffer) SetResource(params, "DLSSNR.Backbuffer", nullptr);
    }
    WriteSizes(params, ww, wh);
    for (const char *name : kSubrectNames) WriteSubrect(params, name, 0, 0, ww, wh);
    SetFloat(params, "DLSSNR.MVecScaleX", 1.0f);
    SetFloat(params, "DLSSNR.MVecScaleY", 1.0f);
    Barrier(cmd, st.nrOutput, st.nrOutputState, kHostOutputState);

    c.stage = StageModel;
    TimingBegin(st, cmd);
    const int result = CallEvaluate(cmd, st.realHandle, params, c.callback);
    TimingEnd(st, cmd);
    Ctx().status.lastNgxResult = result;

    // ---- Unpack into the host output ----
    if (result == kNgxSuccess) {
        c.stage = StageUnpackDescriptors;
        Barrier(cmd, st.nrOutput, st.nrOutputState, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        guideState = st.packedGuideState[slot];
        Barrier(cmd, packed.resources.depth.resource, guideState, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        guideState = st.packedGuideState[slot];
        Barrier(cmd, packed.resources.motion.resource, guideState, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        st.packedGuideState[slot] = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        Barrier(cmd, st.unpackTarget, st.unpackState, D3D12_RESOURCE_STATE_RENDER_TARGET);

        pw::InputDescriptionV2 unpackInput = pw::DefaultInputDescriptionV2(st.nativeWidth, st.nativeHeight);
        unpackInput.colorRect = {0, 0, ww, wh};
        unpackInput.depthRect = {0, 0, ww, wh};
        unpackInput.motionRect = {0, 0, ww, wh};
        unpackInput.confidenceRect = {0, 0, 1, 1};
        unpackInput.motionScaleX = 1.0f;
        unpackInput.motionScaleY = 1.0f;
        unpackInput.depthConvention = c.depthConvention;
        unpackInput.colorEncoding = EncodingFor(st.outputView);
        const pw::D3D12SourceResources workSources = {{st.nrOutput, st.outputView},
                                                      {packed.resources.depth.resource, DXGI_FORMAT_R32_FLOAT},
                                                      {packed.resources.motion.resource, DXGI_FORMAT_R16G16_FLOAT},
                                                      {nullptr, DXGI_FORMAT_UNKNOWN}};
        if (!st.unpackValid[slot] || st.unpackDepthConvention[slot] != c.depthConvention) {
            // Its sources are the adapter's own packed textures and nrOutput: stable, so this is
            // written once per slot (a rewrite while the GPU reads it would be the same race).
            status = st.adapter->WriteSourceDescriptorsV2(c.unpackSlot, workSources, unpackInput);
            if (status == pw::AdapterStatus::Ok) { st.unpackValid[slot] = true; st.unpackDepthConvention[slot] = c.depthConvention; }
        }
        if (status == pw::AdapterStatus::Ok) {
            c.stage = StageUnpackDraw;
            pw::DiagnosticOutlineFlags outlines = pw::DiagnosticOutlineNone;
            if (Ctx().showCenterOutline) outlines = outlines | pw::DiagnosticOutlineCenter;
            if (Ctx().showWorkOutline) outlines = outlines | pw::DiagnosticOutlineRawWork;
            (void) st.adapter->SetOutputColorAdjust(Ctx().outputGain, Ctx().outputGamma);
            status = st.adapter->RecordUnpackColor(
                cmd, c.unpackSlot, st.rtvHeap->GetCPUDescriptorHandleForHeapStart(), outlines);
        }
        if (status == pw::AdapterStatus::Ok && c.wantBase) {
            // The packed colour without the model: what the residual is measured against. Without it
            // the frame still goes out (the residual is then measured against the raw colour).
            c.stage = StageUnpackDraw;
            Barrier(cmd, packed.resources.color.resource, st.packedColorState[slot], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            const pw::AdapterStatus bs = RecordBaseUnpack(c);
            if (bs != pw::AdapterStatus::Ok) { SetReason("base unpack: %s", pw::AdapterStatusString(bs)); c.wantBase = false; }
        }
        if (status == pw::AdapterStatus::Ok) {
            const pwtemporal::FrameInputs tinB = TemporalInputsWithBase(c);
            if (c.temporalActive && !c.temporalFull) {
                c.stage = StageTemporalReproject;
                st.temporal->RecordReproject(cmd, tinB, c.output, kHostOutputState, c.outputRect.x, c.outputRect.y);
            } else {
                if (c.temporalActive) {
                    c.stage = StageTemporalResidual;
                    pwtemporal::FrameInputs tinR = tinB;
                    tinR.residualBlend = Ctx().temporal.debugSingleFrameMotion ? 0.0f : kResidualBlend; // the machine's chain moves the previous residual
                    st.temporal->RecordResidual(cmd, tinR, st.unpackTarget, st.unpackState);
                }
                c.stage = StageCopyOut;
                Barrier(cmd, st.unpackTarget, st.unpackState, D3D12_RESOURCE_STATE_COPY_SOURCE);
                BarrierExternal(cmd, c.output, kHostOutputState, D3D12_RESOURCE_STATE_COPY_DEST);
                cmd->CopyResource(c.output, st.unpackTarget);
                BarrierExternal(cmd, c.output, D3D12_RESOURCE_STATE_COPY_DEST, kHostOutputState);
            }
            ++Ctx().status.evaluations;
            Ctx().status.active = true;
            Ctx().status.reason[0] = 0;
            if (Ctx().status.evaluations == 1)
                Log(false, "Optimizer FPS NGX hook: first warped evaluate completed (model %ux%u)", ww, wh);
        } else {
            SetReason("unpack: %s", pw::AdapterStatusString(status));
            Log(true, "Optimizer FPS NGX hook: %s", Ctx().status.reason);
            Ctx().status.active = false;
            c.stage = StageCopyOut;
            FallbackOutput(c, Ctx().status.reason);
        }
        Barrier(cmd, st.nrOutput, st.nrOutputState, kHostOutputState);
    } else {
        Ctx().status.active = false;
        char why[96];
        std::snprintf(why, sizeof(why), "model returned %d on a warped frame", result);
        c.stage = StageCopyOut;
        FallbackOutput(c, why);
    }

    // ---- hand the block back the way the host left it ----
    c.stage = StageParamsRestore;
    RestoreParams(c);
    c.paramsRewritten = false;
    c.stage = StageDone;
    return result;
}

// Mode 1 on an interpolated frame: no Pack, no model. Colour + reprojected residual into the host's output.
int InterpolateBody(EvalContext &c)
{
    FeatureState &st = *c.st;
    c.stage = StageTemporalAccumulate;
    st.temporal->RecordAccumulate(c.cmd, c.tin);
    if (c.wantBase) {
        // The frame's own colour through Pack -> Unpack (no model): the same compression blur as a
        // full frame, so the residual added on top holds only the model's contribution.
        const int packResult = RecordPackStage(c);
        if (packResult != kNgxSuccess) {
            c.wantBase = false; // the interpolated frame still goes out, measured against the raw colour
        } else {
            PackedGuidesToPixel(st, c.cmd, c.slot, c.packSlot);
            c.stage = StageUnpackDraw;
            const pw::AdapterStatus bs = RecordBaseUnpack(c);
            if (bs != pw::AdapterStatus::Ok) { SetReason("base unpack: %s", pw::AdapterStatusString(bs)); c.wantBase = false; }
        }
    }
    c.stage = StageTemporalReproject;
    st.temporal->RecordReproject(c.cmd, TemporalInputsWithBase(c), KeepOutputOnInterpolation() ? nullptr : c.output, kHostOutputState,
                                 c.outputRect.x, c.outputRect.y);
    c.stage = StageDone;
    return kNgxSuccess;
}

LONG RecordCrash(EXCEPTION_POINTERS *info, int stage) { return pwngx::RecordCrash(info, stage, Ctx().crash); }

int InterpolateGuarded(EvalContext &c)
{
    __try {
        return InterpolateBody(c);
    } __except (RecordCrash(GetExceptionInformation(), c.stage)) {
        return kCrashed;
    }
}

// No C++ objects with destructors in these two frames: __try requires it.
int WarpedGuarded(EvalContext &c)
{
    __try {
        return WarpedBody(c);
    } __except (RecordCrash(GetExceptionInformation(), c.stage)) {
        return kCrashed;
    }
}

void RestoreGuarded(EvalContext &c)
{
    if (!c.paramsRewritten) return;
    __try {
        RestoreParams(c);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

} // namespace pwhook
