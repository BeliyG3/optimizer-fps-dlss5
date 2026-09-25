#include "core/frame/model_passes.h"
#include "core/frame/warp_recorder.h"

#include "core/context.h"
#include "core/frame/host_depth_state.h"
#include "core/frame/frame_inputs.h"
#include "core/temporal/controller.h"
#include "core/frame/timing.h"
#include "core/frame/codec_frame.h"

#include <algorithm>
#include <cstdio>
#include <string>

namespace ofps::core {

D3D12_CPU_DESCRIPTOR_HANDLE BaseRtv(const FeatureState &st)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h = st.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += st.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    return h;
}

ofps::sdk::D3D12PackedViews PackedViewsForFeature(const FeatureState &st, std::uint32_t slot)
{
    if (st.warpPath == warp::PackPath::Pixel) return st.adapter->PackedViews(slot);
    const auto owned = st.compute->PackedFor(slot);
    ofps::sdk::D3D12PackedViews views{};
    views.resources = {{owned.color, st.colorView}, {owned.depth, DXGI_FORMAT_R32_FLOAT},
                       {owned.motion, DXGI_FORMAT_R16G16_FLOAT}, {nullptr, DXGI_FORMAT_UNKNOWN}};
    return views;
}

// Pack: the host's native inputs into the slot's packed textures (colour / depth / motion).
int RecordPackStage(EvalContext &c)
{
    FeatureState &st = *c.st;
    ID3D12GraphicsCommandList *cmd = c.cmd;
    const std::uint32_t slot = c.slot;
    c.stage = StagePackBarriers;
    if (st.warpPath == warp::PackPath::Compute) {
        const D3D12_RESOURCE_STATES rest[3]{c.colorResource.restState, c.depthState,
            c.motionIsAcc ? kModelInputState : c.motionResource.restState};
        const UINT subresources[3]{c.colorResource.subresource, c.depthSub,
            c.motionIsAcc ? D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES : c.motionResource.subresource};
        warp::Packed packed{};
        std::string reason;
        c.stage = StagePackDraw;
        if (!st.compute->Pack(cmd, c.packSlot, c.packSources, c.packInput, rest, subresources,
                              c.privateUsePoint.fence ? &c.privateUsePoint : nullptr, &packed, reason)) {
            SetReason("compute pack: %s", reason.c_str());
            return kPackFailed;
        }
        c.stage = StagePackRestore;
        return OFPS_OK;
    }
    const ofps::sdk::D3D12PackedViews packed = PackedViewsForFeature(st, c.packSlot);
    const auto colorState = c.colorResource.restState;
    const auto colorSub = c.colorResource.subresource;
    BarrierExternal(cmd, c.color, colorState, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, colorSub);
    BarrierExternal(cmd, c.depth, c.depthState, c.depthState | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, c.depthSub);
    if (!c.motionIsAcc) BarrierExternal(cmd, c.motion, c.motionResource.restState, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, c.motionResource.subresource);
    Barrier(cmd, packed.resources.color.resource, st.packedColorState[slot], D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_RESOURCE_STATES guideState = st.packedGuideState[slot];
    Barrier(cmd, packed.resources.depth.resource, guideState, D3D12_RESOURCE_STATE_RENDER_TARGET);
    guideState = st.packedGuideState[slot];
    Barrier(cmd, packed.resources.motion.resource, guideState, D3D12_RESOURCE_STATE_RENDER_TARGET);
    st.packedGuideState[slot] = D3D12_RESOURCE_STATE_RENDER_TARGET;
    c.stage = StagePackDraw;
    ofps::sdk::AdapterStatus status = st.adapter->RecordPackFromSet(cmd, c.packSlot, c.packSet);
    c.stage = StagePackRestore;
    BarrierExternal(cmd, c.color, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, colorState, colorSub);
    BarrierExternal(cmd, c.depth, c.depthState | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, c.depthState, c.depthSub);
    if (!c.motionIsAcc) BarrierExternal(cmd, c.motion, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, c.motionResource.restState, c.motionResource.subresource);
    Barrier(cmd, packed.resources.color.resource, st.packedColorState[slot], kModelInputState);
    guideState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    Barrier(cmd, packed.resources.depth.resource, guideState, kModelInputState);
    guideState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    Barrier(cmd, packed.resources.motion.resource, guideState, kModelInputState);
    st.packedGuideState[slot] = kModelInputState;
    if (status != ofps::sdk::AdapterStatus::Ok) {
        SetReason("pack draw: %s", ofps::sdk::AdapterStatusString(status));
        return kPackFailed;
    }
    return OFPS_OK;
}

// The slot's packed colour unpacked without the model into c.baseTarget (packed guides must be in
// PIXEL_SHADER_RESOURCE); the target rests in PIXEL_SHADER_RESOURCE afterwards.
ofps::sdk::AdapterStatus RecordBaseUnpack(EvalContext &c)
{
    FeatureState &st = *c.st;
    if (st.warpPath == warp::PackPath::Compute) {
        const warp::UnpackTarget target{c.baseTarget, st.outputView, *c.baseTargetState, 0,
            0, 0, st.layout.nativeWidth, st.layout.nativeHeight, false};
        std::string reason;
        c.stage = StageUnpackDraw;
        if (!st.compute->BaseUnpack(c.cmd, c.packSlot, target,
                                    c.privateUsePoint.fence ? &c.privateUsePoint : nullptr, reason)) {
            SetReason("base unpack: %s", reason.c_str());
            return ofps::sdk::AdapterStatus::DeviceError;
        }
        return ofps::sdk::AdapterStatus::Ok;
    }
    Barrier(c.cmd, c.baseTarget, *c.baseTargetState, D3D12_RESOURCE_STATE_RENDER_TARGET);
    (void) st.adapter->SetOutputColorAdjust(Ctx().outputGain, Ctx().outputGamma);
    // The slot's textures with the constants of the set that packed them (the unpack shader reads the
    // input description too).
    const ofps::sdk::AdapterStatus status = st.adapter->RecordUnpackOwnedColorFromSet(c.cmd, c.packSlot, c.packSet, c.baseRtv, ofps::sdk::DiagnosticOutlineNone);
    Barrier(c.cmd, c.baseTarget, *c.baseTargetState, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    return status;
}

// Packed guides of the slot to / from PIXEL_SHADER_RESOURCE (for an unpack outside WarpedBody).
void PackedGuidesToPixel(FeatureState &st, ID3D12GraphicsCommandList *cmd, std::uint32_t slot, std::uint32_t packSlot)
{
    const ofps::sdk::D3D12PackedViews packed = PackedViewsForFeature(st, packSlot);
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
void FallbackOutput(FeatureState &st, ID3D12GraphicsCommandList *cmd, const OfpsResource &colorResource, const OfpsResource &outputResource, const ofps::core::temporal::FrameInputs *tin, const char *why)
{
    if (st.frameResult && st.frameResult->path != OFPS_PATH_CREATION_FRAME) {
        st.frameResult->path = OFPS_PATH_FALLBACK; st.frameResult->warpPath = OFPS_WARP_NONE;
    }

    auto *color = colorResource.res, *output = outputResource.res;
    const auto &colorRect = colorResource.rect, &outputRect = outputResource.rect;
    ++Ctx().status.fallbackFrames;
    std::snprintf(Ctx().status.fallbackReason, sizeof(Ctx().status.fallbackReason), "%s", why);
    if (Ctx().status.fallbackFrames <= 5 || Ctx().status.fallbackFrames % 500 == 0)
        Log(true, "Optimizer FPS NGX hook: frame %llu shown as the host's colour (%s), %llu such frames so far",
            static_cast<unsigned long long>(Ctx().evalCounter), why, static_cast<unsigned long long>(Ctx().status.fallbackFrames));
    if (cmd == nullptr || color == nullptr || output == nullptr || (color == output && CopySubresource(colorResource) == CopySubresource(outputResource))) return;
    const D3D12_RESOURCE_DESC cd = color->GetDesc(), od = output->GetDesc();
    if (cd.Format == od.Format) {
        BarrierExternal(cmd, color, colorResource.restState, D3D12_RESOURCE_STATE_COPY_SOURCE, colorResource.subresource);
        BarrierExternal(cmd, output, outputResource.restState, D3D12_RESOURCE_STATE_COPY_DEST, outputResource.subresource);
        D3D12_TEXTURE_COPY_LOCATION dst{}; dst.SubresourceIndex = CopySubresource(outputResource); dst.pResource = output; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION src{}; src.SubresourceIndex = CopySubresource(colorResource); src.pResource = color; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        const UINT w = std::min<UINT>(st.nativeWidth, static_cast<UINT>(cd.Width) - std::min<UINT>(colorRect.x, static_cast<UINT>(cd.Width)));
        const UINT h = std::min<UINT>(st.nativeHeight, cd.Height - std::min<UINT>(colorRect.y, cd.Height));
        const D3D12_BOX box{colorRect.x, colorRect.y, 0, colorRect.x + w, colorRect.y + h, 1};
        cmd->CopyTextureRegion(&dst, outputRect.x, outputRect.y, 0, &src, &box);
        BarrierExternal(cmd, output, D3D12_RESOURCE_STATE_COPY_DEST, outputResource.restState, outputResource.subresource);
        BarrierExternal(cmd, color, D3D12_RESOURCE_STATE_COPY_SOURCE, colorResource.restState, colorResource.subresource);
    } else if (st.temporal && tin != nullptr && tin->color != nullptr && tin->depth != nullptr && tin->motion != nullptr) {
        ofps::core::temporal::FrameInputs in = *tin;
        in.base = nullptr;
        in.debugVis = 3;
        st.temporal->RecordReproject(cmd, in, output, outputResource.restState, outputRect.x, outputRect.y, outputResource.subresource);
    } else {
        static bool s_once = false;
        if (!s_once) { s_once = true; Log(true, "Optimizer FPS NGX hook: no fallback for this frame (colour fmt %d, output fmt %d, no temporal machine)", (int) cd.Format, (int) od.Format); }
    }
}

void FallbackOutput(EvalContext &c, const char *why)
{
    FallbackOutput(*c.st, c.cmd, c.colorResource, c.outputResource, c.tin.color ? &c.tin : nullptr, why);
}

// Before an EvalContext exists (the layout / model could not be applied): read the host's colour and
// output straight from the block.
void FallbackFromFrame(FeatureState &st, ID3D12GraphicsCommandList *cmd, const OfpsFrameInputs &frame, const char *why)
{
    auto tin = TemporalInputs(frame.color, frame.motion, frame.depth,
                              frame.mvScaleX, frame.mvScaleY, frame.depthInverted != 0, st.modelResolution);
    FallbackOutput(st, cmd, frame.color, frame.output, &tin, why);
}

// The temporal inputs with the base colour when this frame has one.
ofps::core::temporal::FrameInputs TemporalInputsWithBase(const EvalContext &c)
{
    ofps::core::temporal::FrameInputs t = c.tin;
    if (c.wantBase && c.baseTarget) { t.base = c.baseTarget; t.baseState = *c.baseTargetState; }
    else if (c.codec && c.codec->frameBefore.res) {
        t.base = c.codec->frameBefore.res;
        t.baseState = c.codec->frameBefore.restState;
        t.baseSubresource = c.codec->frameBefore.subresource;
        if (c.frame && c.frame->color.res == c.frame->output.res) {
            t.color = c.codec->frameBefore.res;
            t.hostInputState = c.codec->frameBefore.restState;
            t.colorSubresource = c.codec->frameBefore.subresource;
            const auto &r = c.codec->frameBefore.rect;
            t.colorRect = {r.x, r.y, r.w, r.h};
        }
    }
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
    const std::uint32_t slot = c.slot;

    // ---- temporal: accumulate the host's motion before anything reads the displacement ----
    if (c.temporalActive && c.accumulate) {
        c.stage = StageTemporalAccumulate;
        st.temporal->RecordAccumulate(cmd, c.tin);
        // The vectors the model gets on this full pass (hook_dispatch pointed Pack's source at them).
        if (c.modelMotion) st.temporal->RecordModelMotion(cmd, c.tin);
    }

    // ---- Pack ----
    const int packResult = RecordPackStage(c);
    if (packResult != OFPS_OK) return packResult;
    const ofps::sdk::D3D12PackedViews packed = PackedViewsForFeature(st, c.packSlot);
    D3D12_RESOURCE_STATES guideState = st.packedGuideState[slot];
    ofps::sdk::AdapterStatus status = ofps::sdk::AdapterStatus::Ok;

    // ---- model on the packed frame ----
    c.stage = StageParamsWrite;
    const std::uint32_t ww = st.layout.workWidth, wh = st.layout.workHeight;
    OfpsModelInputs in = c.modelInputs;
    in.color.res = packed.resources.color.resource;
    in.depth.res = packed.resources.depth.resource;
    in.motion.res = packed.resources.motion.resource;
    in.output.res = st.nrOutput;
    in.color.view = packed.resources.color.format;
    in.depth.view = packed.resources.depth.format;
    in.motion.view = packed.resources.motion.format;
    in.output.view = st.outputView;
    in.withholdUi = st.modelResolution ? st.withholdUi : !KeepBackbufferEnabled();
    in.width = ww; in.height = wh;
    for (auto *r : {&in.color, &in.depth, &in.motion, &in.output}) {
        r->rect = {0, 0, ww, wh};
        r->subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        r->restState = r == &in.output ? kModelOutputState : kModelInputState;
    }
    in.mvScaleX = in.mvScaleY = 1.0f;
    Barrier(cmd, st.nrOutput, st.nrOutputState, kModelOutputState);

    c.stage = StageModel;
    TimingBegin(st, cmd);
    const int result = EvaluateModelPasses(st, cmd, in);
    TimingEnd(st, cmd);
    Ctx().status.lastNgxResult = result;

    // ---- Unpack into the host output ----
    if (result == OFPS_OK) {
        c.stage = StageUnpackDescriptors;
        if (st.warpPath == warp::PackPath::Compute) {
            Barrier(cmd, st.nrOutput, st.nrOutputState, kModelInputState);
            const bool codecTarget = c.codec && !c.codec->identity;
            const bool nativeTarget = c.temporalActive && !codecTarget;
            ID3D12Resource *targetResource = codecTarget ? st.answer :
                nativeTarget ? st.unpackTarget : c.output;
            const auto targetState = codecTarget ? st.answerState :
                nativeTarget ? st.unpackState : c.outputResource.restState;
            const warp::UnpackTarget target{targetResource, st.outputView, targetState,
                codecTarget || nativeTarget ? 0u : CopySubresource(c.outputResource),
                codecTarget || nativeTarget ? 0u : c.outputRect.x,
                codecTarget || nativeTarget ? 0u : c.outputRect.y,
                st.layout.nativeWidth, st.layout.nativeHeight, false};
            std::uint32_t outlines = 0;
            if (Ctx().showCenterOutline) outlines |= ofps::sdk::DiagnosticOutlineCenter;
            if (Ctx().showWorkOutline) outlines |= ofps::sdk::DiagnosticOutlineRawWork;
            std::string reason;
            c.stage = StageUnpackDraw;
            if (!st.compute->Unpack(cmd, c.packSlot, st.nrOutput, st.outputView, target,
                                    outlines, Ctx().outputGain, Ctx().outputGamma,
                                    c.privateUsePoint.fence ? &c.privateUsePoint : nullptr, reason)) {
                SetReason("compute unpack: %s", reason.c_str());
                status = ofps::sdk::AdapterStatus::DeviceError;
            }
        } else {
        Barrier(cmd, st.nrOutput, st.nrOutputState, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        guideState = st.packedGuideState[slot];
        Barrier(cmd, packed.resources.depth.resource, guideState, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        guideState = st.packedGuideState[slot];
        Barrier(cmd, packed.resources.motion.resource, guideState, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        st.packedGuideState[slot] = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        Barrier(cmd, st.unpackTarget, st.unpackState, D3D12_RESOURCE_STATE_RENDER_TARGET);

        ofps::sdk::InputDescriptionV2 unpackInput = ofps::sdk::DefaultInputDescriptionV2(st.layout.nativeWidth, st.layout.nativeHeight);
        unpackInput.colorRect = {0, 0, ww, wh};
        unpackInput.depthRect = {0, 0, ww, wh};
        unpackInput.motionRect = {0, 0, ww, wh};
        unpackInput.confidenceRect = {0, 0, 1, 1};
        unpackInput.motionScaleX = 1.0f;
        unpackInput.motionScaleY = 1.0f;
        unpackInput.depthConvention = c.depthConvention;
        unpackInput.colorEncoding = EncodingFor(st.outputView);
        const ofps::sdk::D3D12SourceResources workSources = {{st.nrOutput, st.outputView},
                                                      {packed.resources.depth.resource, DXGI_FORMAT_R32_FLOAT},
                                                      {packed.resources.motion.resource, DXGI_FORMAT_R16G16_FLOAT},
                                                      {nullptr, DXGI_FORMAT_UNKNOWN}};
        if (!st.unpackValid[slot] || st.unpackDepthConvention[slot] != c.depthConvention) {
            // Its sources are the adapter's own packed textures and nrOutput: stable, so this is
            // written once per slot (a rewrite while the GPU reads it would be the same race).
            status = st.adapter->WriteSourceDescriptorsV2(c.unpackSlot, workSources, unpackInput);
            if (status == ofps::sdk::AdapterStatus::Ok) { st.unpackValid[slot] = true; st.unpackDepthConvention[slot] = c.depthConvention; }
        }
        if (status == ofps::sdk::AdapterStatus::Ok) {
            c.stage = StageUnpackDraw;
            ofps::sdk::DiagnosticOutlineFlags outlines = ofps::sdk::DiagnosticOutlineNone;
            if (Ctx().showCenterOutline) outlines = outlines | ofps::sdk::DiagnosticOutlineCenter;
            if (Ctx().showWorkOutline) outlines = outlines | ofps::sdk::DiagnosticOutlineRawWork;
            (void) st.adapter->SetOutputColorAdjust(Ctx().outputGain, Ctx().outputGamma);
            status = st.adapter->RecordUnpackColor(
                cmd, c.unpackSlot, st.rtvHeap->GetCPUDescriptorHandleForHeapStart(), outlines);
        }
        }
        if (status == ofps::sdk::AdapterStatus::Ok && c.wantBase) {
            // The packed colour without the model: what the residual is measured against. Without it
            // the frame still goes out (the residual is then measured against the raw colour).
            c.stage = StageUnpackDraw;
            if (st.warpPath == warp::PackPath::Pixel)
                Barrier(cmd, packed.resources.color.resource, st.packedColorState[slot], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            const ofps::sdk::AdapterStatus bs = RecordBaseUnpack(c);
            if (bs != ofps::sdk::AdapterStatus::Ok) {
                if (st.warpPath == warp::PackPath::Compute) {
                    st.disabled = true;
                    FallbackOutput(c, Ctx().status.reason);
                    return OFPS_E_DEVICE;
                }
                SetReason("base unpack: %s", ofps::sdk::AdapterStatusString(bs)); c.wantBase = false;
            }
        }
        if (status == ofps::sdk::AdapterStatus::Ok) {
            if (c.codec && !c.codec->identity) {
                const int resolved = FinishWarpCodec(c);
                if (resolved != OFPS_OK) { FallbackOutput(c, "codec resolve failed"); return resolved; }
            } else {
            const ofps::core::temporal::FrameInputs tinB = TemporalInputsWithBase(c);
            if (c.temporalActive && !c.temporalFull) {
                c.stage = StageTemporalReproject;
                st.temporal->RecordReproject(cmd, tinB, c.output, c.outputResource.restState, c.outputRect.x, c.outputRect.y, c.outputResource.subresource);
            } else {
                if (c.temporalActive) {
                    c.stage = StageTemporalResidual;
                    ofps::core::temporal::FrameInputs tinR = tinB;
                    tinR.residualBlend = Ctx().temporal.debugSingleFrameMotion ? 0.0f : kResidualBlend; // the machine's chain moves the previous residual
                    st.temporal->RecordResidual(cmd, tinR, st.unpackTarget, st.unpackState);
                }
                c.stage = StageCopyOut;
                if (c.temporalActive && st.temporal->PhaseInActive()) {
                    // 26.28: while a pass is being phased in, the full frame is shown as "colour + the
                    // residual mix" too; otherwise it would still snap to the model's new version once
                    // per cadence while only the carried frames faded.
                    st.temporal->RecordApply(cmd, tinB, c.output, c.outputResource.restState, c.outputRect.x, c.outputRect.y, c.outputResource.subresource);
                } else if (st.warpPath == warp::PackPath::Pixel || c.temporalActive) {
                    // Into the host's output region: its texture may be larger than the frame (and may
                    // carry more mips), where a whole-resource copy would be invalid.
                    Barrier(cmd, st.unpackTarget, st.unpackState, D3D12_RESOURCE_STATE_COPY_SOURCE);
                    BarrierExternal(cmd, c.output, c.outputResource.restState, D3D12_RESOURCE_STATE_COPY_DEST, c.outputResource.subresource);
                    D3D12_TEXTURE_COPY_LOCATION dst{}; dst.SubresourceIndex = CopySubresource(c.outputResource); dst.pResource = c.output; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                    D3D12_TEXTURE_COPY_LOCATION src{}; src.pResource = st.unpackTarget; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                    const D3D12_BOX box{0, 0, 0, st.nativeWidth, st.nativeHeight, 1};
                    cmd->CopyTextureRegion(&dst, c.outputRect.x, c.outputRect.y, 0, &src, &box);
                    BarrierExternal(cmd, c.output, D3D12_RESOURCE_STATE_COPY_DEST, c.outputResource.restState, c.outputResource.subresource);
                }
            }
            }
            ++Ctx().status.evaluations;
            if (c.temporalActive)
                st.temporal->RecordFlowCapture(cmd, TemporalInputsWithBase(c), st.realDevice, c.temporalFull);
            Ctx().status.active = true;
            Ctx().status.reason[0] = 0;
            if (Ctx().status.evaluations == 1)
                Log(false, "Optimizer FPS NGX hook: first warped evaluate completed (model %ux%u)", ww, wh);
        } else {
            if (st.warpPath == warp::PackPath::Pixel)
                SetReason("unpack: %s", ofps::sdk::AdapterStatusString(status));
            Log(true, "Optimizer FPS NGX hook: %s", Ctx().status.reason);
            Ctx().status.active = false;
            c.stage = StageCopyOut;
            FallbackOutput(c, Ctx().status.reason);
        }
        Barrier(cmd, st.nrOutput, st.nrOutputState, kModelOutputState);
    } else {
        Ctx().status.active = false;
        char why[96];
        std::snprintf(why, sizeof(why), "model returned %d on a warped frame", result);
        c.stage = StageCopyOut;
        FallbackOutput(c, why);
    }

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
        if (packResult != OFPS_OK) {
            c.wantBase = false; // the interpolated frame still goes out, measured against the raw colour
        } else {
            if (st.warpPath == warp::PackPath::Pixel)
                PackedGuidesToPixel(st, c.cmd, c.slot, c.packSlot);
            c.stage = StageUnpackDraw;
            const ofps::sdk::AdapterStatus bs = RecordBaseUnpack(c);
            if (bs != ofps::sdk::AdapterStatus::Ok) {
                if (st.warpPath == warp::PackPath::Compute) return kPackFailed;
                SetReason("base unpack: %s", ofps::sdk::AdapterStatusString(bs)); c.wantBase = false;
            }
        }
    }
    c.stage = StageTemporalReproject;
    st.temporal->RecordReproject(c.cmd, TemporalInputsWithBase(c), KeepOutputOnInterpolation() ? nullptr : c.output, c.outputResource.restState,
                                 c.outputRect.x, c.outputRect.y, c.outputResource.subresource);
    st.temporal->RecordFlowCapture(c.cmd, TemporalInputsWithBase(c), st.realDevice, false);
    c.stage = StageDone;
    return OFPS_OK;
}

LONG RecordCrash(EXCEPTION_POINTERS *info, int stage) { return ofps::core::gpu::RecordCrash(info, stage, Ctx().crash); }

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

} // namespace ofps::core
