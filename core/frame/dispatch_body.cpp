#include "core/frame/callback_guard.h"
#include "core/context.h"
#include "core/frame/codec_frame.h"
#include "core/frame/debug_readback.h"
#include "core/frame/dispatch.h"
#include "core/frame/frame_inputs.h"
#include "core/frame/host_depth_state.h"
#include "core/frame/host_shape.h"
#include "core/frame/lifecycle.h"
#include "core/frame/model_grid.h"
#include "core/frame/model_grid_path.h"
#include "core/frame/model_passes.h"
#include "core/frame/model_protocol.h"
#include "core/frame/motion_smooth.h"
#include "core/frame/timing.h"
#include "core/frame/warp_recorder.h"
#include "core/temporal/async_scheduler.h"
#include "core/temporal/controller.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>

namespace ofps::core {

int EvaluateFrameBody(FeatureState &st, ID3D12GraphicsCommandList *cmd,
                      const OfpsFrameInputs &frame, OfpsEvalResult &evalResult) {
    evalResult.path = OFPS_PATH_PASSTHROUGH;
    if (cmd->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT) {
        SetReason("host command list type %d cannot run the model; DIRECT required", (int)cmd->GetType());
        if (cmd->GetType() == D3D12_COMMAND_LIST_TYPE_COMPUTE ||
            cmd->GetType() == D3D12_COMMAND_LIST_TYPE_COPY)
            FallbackFromFrame(st, cmd, frame, Ctx().status.reason);
        return OFPS_E_ARG;
    }
    OfpsModelInputs modelInputs = ModelInputsFrom(frame, st.nativeWidth, st.nativeHeight);
    AsyncSignalPending(st);
    CodecFrame codec;
    bool codecPrepared = false;
    if (st.modelResolution && !st.realHandle) {
        const int prepared = PrepareModelGridFrame(st, cmd, frame, codec);
        if (prepared == OFPS_S_MODEL_NEXT_FRAME) return CreationFrame(st, cmd, frame, evalResult);
        if (prepared != OFPS_OK) return ModelGridFallback(st, cmd, frame, "codec preparation failed", prepared);
        codecPrepared = true;
    }
    if (PassesPending(st)) return CreationFrame(st, cmd, frame, evalResult);
    const bool adoptedNow = st.adoptedPending;
    st.adoptedPending = false;
    const auto generation = st.modelGeneration;

    // The host's frame is judged before the layout (host_shape.h): a frame the warp cannot take keeps the
    // model at the feature's size, and a slider move no longer re-creates it twice (work, then native).
    const ofps::sdk::ConfigV2 config = Ctx().config;
    if (!SameLayoutConfig(config, st.config)) st.hostLatched = false; // a user change re-tests the host's frame
    const bool verdictChanged = JudgeHostShape(st, ReadHostShape(frame));
    const bool forceLayout = adoptedNow || verdictChanged || st.rebuildRequested;
    const int applied = st.modelResolution || (st.codecGridActive && !forceLayout && SameLayoutConfig(config, st.config))
        ? OFPS_OK : ApplyLayout(st, cmd, config, forceLayout);
    if (applied != OFPS_OK) {
        char why[96];
        std::snprintf(why, sizeof(why), "the layout could not be applied (model re-creation returned %d)", applied);
        FallbackFromFrame(st, cmd, frame, why);
        return applied;
    }

    if (verdictChanged && !st.hostFits && st.passesRequested > 1) {
        // The extra passes get no frames while the host does not fit; their history would be stale by the
        // time it fits again, so they are dropped now and rebuilt then (PrepareModelPasses).
        ofps::core::gpu::GateSet gate;
        BuryAsync(st, &gate);
        gate = RetirementGate(st, gate);
        RetireModelPasses(st, gate);
    }

    if (st.realHandle == nullptr) {
        FallbackFromFrame(st, cmd, frame, "no model (creation failed)");
        return OFPS_E_DEVICE;
    }

    if (!st.modelResolution && st.disabled && st.warped) {
        // A previous frame gave up on the warp; the model still expects the work frame.
        RetireGpu(st);
        const int r = RecreateReal(st, cmd, st.nativeWidth, st.nativeHeight);
        Log(r == OFPS_OK ? false : true, "Optimizer FPS NGX hook: model re-created at native size after a failure (%d)", st.lastCreateHostResult);
        if (r < 0) {
            FallbackFromFrame(st, cmd, frame, "the native model could not be re-created");
            return r;
        }
        st.forcedReset = true;
    }

    if (st.modelPending && generation != st.modelGeneration) return CreationFrame(st, cmd, frame, evalResult);
    const MotionSmoothScope motionSmooth(st, cmd, modelInputs);
    modelInputs.motion = FrameMotion(st, frame.motion);
    modelInputs.reset = modelInputs.reset || st.forcedReset;
    if (!st.hostFits) {
        if (st.modelResolution && !codecPrepared) {
            const int prepared = PrepareModelGridFrame(st, cmd, frame, codec);
            if (prepared == OFPS_S_MODEL_NEXT_FRAME) return CreationFrame(st, cmd, frame, evalResult);
            if (prepared != OFPS_OK) return ModelGridFallback(st, cmd, frame, "codec preparation failed", prepared);
            codecPrepared = true;
        }
        ModelGridInitialInputs(st, codec, modelInputs);
        return RunUnfitModelPath(st, cmd, frame, codec, modelInputs);
    }

    if (PrepareModelPasses(st, cmd, modelInputs)) {
        return CreationFrame(st, cmd, frame, evalResult);
    }

    const int temporalRoute = TryTemporalRoute(st, cmd, modelInputs, frame, evalResult);
    if (temporalRoute != kNotHandled) return temporalRoute;

    if (st.modelResolution && !codecPrepared) {
        const int prepared = PrepareModelGridFrame(st, cmd, frame, codec);
        if (prepared == OFPS_S_MODEL_NEXT_FRAME) return CreationFrame(st, cmd, frame, evalResult);
        if (prepared != OFPS_OK) return ModelGridFallback(st, cmd, frame, "codec preparation failed", prepared);
    }
    ModelGridInitialInputs(st, codec, modelInputs);

    if (!st.warped || st.disabled) {
        const int result = RunPlainModelPath(st, cmd, frame, codec, modelInputs);
        return result == OFPS_S_MODEL_NEXT_FRAME ? CreationFrame(st, cmd, frame, evalResult) : result;
    }

    ID3D12Resource *color = frame.color.res;
    ID3D12Resource *depth = frame.depth.res;
    ID3D12Resource *motion = modelInputs.motion.res;
    ID3D12Resource *output = frame.output.res;
    if (color == nullptr || depth == nullptr || motion == nullptr || output == nullptr) {
        SetReason("host supplied no colour/depth/motion/output");
        if (st.modelResolution) return ModelGridFallback(st, cmd, frame, Ctx().status.reason, OFPS_E_ARG);
        st.disabled = true;
        Log(true, "Optimizer FPS NGX hook: %s; re-creating the model at native size", Ctx().status.reason);
        const int recreated = RecreateReal(st, cmd, st.nativeWidth, st.nativeHeight);
        if (recreated < 0) return recreated;
        if (st.modelPending) return CreationFrame(st, cmd, frame, evalResult);
        void *real = st.realHandle;
        if (real == nullptr) return OFPS_E_DEVICE;
        return EvaluateModelPasses(st, cmd, modelInputs);
    }

    // The output region is the feature's size at 0,0 (host_shape.h judged this frame); the texture may be larger.
    const D3D12_RESOURCE_DESC outputDesc = output->GetDesc();
    if (!EnsureGpu(st, cmd, color, output, frame.color.view, frame.output.view)) {
        if (st.modelResolution) {
            st.disabled = true;
            return ModelGridFallback(st, cmd, frame, "GPU preparation failed", OFPS_E_DEVICE);
        }
        st.disabled = true;
        Log(true, "Optimizer FPS NGX hook: %s; re-creating the model at native size", Ctx().status.reason);
        RetireGpu(st);
        const int recreated = RecreateReal(st, cmd, st.nativeWidth, st.nativeHeight);
        if (recreated < 0) return recreated;
        if (st.modelPending) return CreationFrame(st, cmd, frame, evalResult);
        void *real = st.realHandle;
        if (real == nullptr) return OFPS_E_DEVICE;
        return EvaluateModelPasses(st, cmd, modelInputs);
    }

    TemporalPlan plan;
    if (Ctx().temporal.mode != 0 && EnsureTemporal(st, cmd, output, motion, depth))
        plan = PlanTemporal(st, frame.hostReset != 0 || st.forcedReset);
    if (!st.modelResolution) {
        codec.frame = frame; codec.modelColor = frame.color; codec.answer = frame.output;
    }
    if (plan.full && EffectiveTemporalMode(st) != 3) {
        int prepared = OFPS_OK;
        if (!st.modelResolution) {
            prepared = BeginCodecFrame(st, cmd, frame, codec);
            if (prepared == OFPS_OK) prepared = ConfigureModelGrid(st, cmd, codec);
        }
        if (prepared == OFPS_S_MODEL_NEXT_FRAME) return CreationFrame(st, cmd, frame, evalResult);
        if (prepared != OFPS_OK) {
            FallbackFromFrame(st, cmd, frame, "codec preparation failed");
            return prepared;
        }
        color = codec.modelColor.res;
    }

    // ---- read the host's description ----
    const D3D12_RESOURCE_DESC colorDesc = color->GetDesc();
    const D3D12_RESOURCE_DESC depthDesc = depth->GetDesc();
    const D3D12_RESOURCE_DESC motionDesc = motion->GetDesc();
    const OfpsRect colorRect{codec.modelColor.rect.x, codec.modelColor.rect.y, codec.modelColor.rect.w, codec.modelColor.rect.h};
    const OfpsRect depthRect = frame.depth.rect;
    const OfpsRect motionRect = frame.motion.rect;
    const OfpsRect outputRect = frame.output.rect;
    const unsigned int depthInverted = frame.depthInverted;
    const float mvScaleX = frame.mvScaleX, mvScaleY = frame.mvScaleY;
    ID3D12Resource *ui = frame.ui.res;
    ID3D12Resource *uiAlpha = frame.uiAlpha.res;
    ID3D12Resource *backbuffer = frame.backbuffer.res;

    ++st.frame;

    // The "Host motion" status row: the shell used to write these from the NGX block before the ABI
    // split (2337f95 dropped it); the frame inputs carry the same values.
    Ctx().status.hostMotionWidth = static_cast<std::uint32_t>(motionDesc.Width);
    Ctx().status.hostMotionHeight = motionDesc.Height;
    Ctx().status.hostMotionRectWidth = frame.motion.rect.w;
    Ctx().status.hostMotionRectHeight = frame.motion.rect.h;
    Ctx().status.hostMotionScaleX = frame.mvScaleX;
    Ctx().status.hostMotionScaleY = frame.mvScaleY;
    Ctx().status.hostMotionScaleRead = frame.mvScaleX != 0.0f || frame.mvScaleY != 0.0f;

    // ---- temporal plan for this frame ----

    Ctx().status.temporalMode = plan.active ? EffectiveTemporalMode(st) : 0;
    if (plan.active && EffectiveTemporalMode(st) == Ctx().temporal.mode) Ctx().status.temporalReason[0] = 0;
    const bool useAcc = plan.active && plan.full && EffectiveTemporalMode(st) != 3 && st.temporal->AccValid() && !Ctx().temporal.debugSingleFrameMotion;
    ID3D12Resource *const hostMotion = motion;
    // 26.28: where the chain does not end on the pixel's own surface in the residual's frame the model
    // must be told there is no history there (PSModelMotion), or it blends its old picture of the
    // occluder into the wall he has left. Both textures are written before Pack reads them.
    const bool useModelMv = useAcc && !Ctx().diag.temporalNoModelMotion && st.temporal->ModelMv() != nullptr;
    if (useAcc) motion = useModelMv ? st.temporal->ModelMv() : st.temporal->NextAcc(); // written by the accumulation recorded before Pack
    ofps::core::temporal::FrameInputs tin = TemporalInputs(frame.color, modelInputs.motion, frame.depth, mvScaleX, mvScaleY, depthInverted != 0, st.modelResolution);
    // 26.26: the same depth resting state and barrier subresource the Pack barriers below use (c.depthState /
    // c.depthSub), so the accumulation and the reprojection address a planar depth-stencil host the same way.
    tin.depthState = HostDepthState(frame.depth);
    tin.depthSubresource = frame.depth.subresource;

    ofps::sdk::InputDescriptionV2 input = ofps::sdk::DefaultInputDescriptionV2(st.layout.nativeWidth, st.layout.nativeHeight);
    input.colorRect = {colorRect.x, colorRect.y, colorRect.w, colorRect.h};
    input.depthRect = {depthRect.x, depthRect.y, depthRect.w, depthRect.h};
    input.motionRect = {motionRect.x, motionRect.y, motionRect.w, motionRect.h};
    input.confidenceRect = {0, 0, 1, 1};
    // The model's motion scale converts stored components to its input pixels; Pack wants native pixels.
    // The user's adjustment (overlay, Advanced) multiplies on top; a sign flip is a multiplier of -1.
    const float adjust = Ctx().motionScaleAdjust * (Ctx().motionInvert ? -1.0f : 1.0f);
    // The accumulated displacement is already in motion-texture pixels (the host's scale applied).
    input.motionScaleX = adjust * (useAcc ? 1.0f : mvScaleX) * static_cast<float>(st.layout.nativeWidth) / static_cast<float>(std::max(1u, motionRect.w));
    input.motionScaleY = adjust * (useAcc ? 1.0f : mvScaleY) * static_cast<float>(st.layout.nativeHeight) / static_cast<float>(std::max(1u, motionRect.h));
    input.motionDirection = ofps::sdk::MotionDirection::CurrentToPrevious;
    input.depthConvention = depthInverted != 0 ? ofps::sdk::DepthConvention::Reversed : ofps::sdk::DepthConvention::Normal;
    input.colorEncoding = EncodingFor(codec.modelColor.view);

    const ofps::sdk::D3D12SourceResources sources = {{color, codec.modelColor.view},
                                              {depth, frame.depth.view},
                                              {motion, useAcc ? DXGI_FORMAT_R16G16_FLOAT : modelInputs.motion.view},
                                              {nullptr, DXGI_FORMAT_UNKNOWN}};
    // The packed textures: next slot round-robin (reuse is ordered by the host's queue). The source
    // set: a fresh ring entry every evaluate, never one the GPU may still read.
    const std::uint32_t packSlot = st.nextPackSlot;
    st.nextPackSlot = (st.nextPackSlot + 1) % FeatureState::kBgPackSlot;
    const auto packSet = st.warpPath == warp::PackPath::Pixel ?
        st.packSets.Acquire(HostUsePoint(cmd), Ctx().evalCounter, gpu::kPoolWaitMilliseconds) : 0u;
    if (st.warpPath == warp::PackPath::Pixel && packSet == gpu::DescriptorPool::kNone) {
        FallbackOutput(st, cmd, frame.color, frame.output, &tin, "pack source-set ring exhausted");
        return OFPS_OK;
    }
    const ofps::sdk::AdapterStatus status = st.warpPath == warp::PackPath::Pixel ?
        st.adapter->WriteSourceSetV2(packSet, sources, input) : ofps::sdk::AdapterStatus::Ok;
    FeatureState::InputSignature sig;
    std::memset(&sig, 0, sizeof(sig)); // padding bytes take part in the memcmp below; leave none indeterminate
    sig.colorFormat = colorDesc.Format; sig.depthFormat = depthDesc.Format; sig.motionFormat = motionDesc.Format; sig.outputFormat = outputDesc.Format;
    sig.motionW = (std::uint32_t) motionDesc.Width; sig.motionH = motionDesc.Height;
    sig.colorRect = colorRect; sig.depthRect = depthRect; sig.motionRect = motionRect;
    // 26.20: the host's scale, not the Pack scale - the latter flips to 1 whenever the temporal chain feeds the model
    // (full frames after interpolated ones), which logged the input block on every single evaluate.
    sig.motionScaleX = mvScaleX; sig.motionScaleY = mvScaleY;
    sig.depthInverted = depthInverted; sig.colorIsOutput = color == output; sig.valid = true;
    const bool inputsChanged = !st.inputSignature.valid || std::memcmp(&st.inputSignature, &sig, sizeof(sig)) != 0;
    if (inputsChanged) {
    char descriptions[2048] = {};
    if (st.modelHost) GuardCallback([&] { return st.modelHost->DescribeInputs(descriptions, sizeof(descriptions)); }, 0);
    char *resourcesTail = std::strchr(descriptions, '\n');
    if (resourcesTail) *resourcesTail++ = 0;
    char *modelTail = resourcesTail ? std::strchr(resourcesTail, '\n') : nullptr;
    if (modelTail) *modelTail++ = 0;
    char *probeTail = modelTail ? std::strchr(modelTail, '\n') : nullptr;
    if (probeTail) *probeTail++ = 0;

        st.inputSignature = sig;
        Log(false, "Optimizer FPS NGX hook: host resources: colour %p fmt %d, depth %p fmt %d, motion %p fmt %d, output %p fmt %d%s %s",
            (void *) color, (int) colorDesc.Format, (void *) depth, (int) depthDesc.Format, (void *) motion, (int) motionDesc.Format, (void *) output, (int) outputDesc.Format,
            color == output ? " (output IS the colour: in-place host)" : "", resourcesTail ? resourcesTail : "(pointers may rotate every frame; logged on a change of formats/rects/scale)");
        Log(false, "Optimizer FPS NGX hook: host motion input: texture %ux%u fmt %d, subrect %u,%u %ux%u, MVecScale %.4f x %.4f (%s), depth inverted %u, colour subrect %ux%u; Pack scale %.4f x %.4f (adjust %.3f%s)",
            (unsigned) motionDesc.Width, motionDesc.Height, (int) motionDesc.Format, motionRect.x, motionRect.y, motionRect.w,
            motionRect.h, mvScaleX, mvScaleY, descriptions,
            depthInverted, colorRect.w, colorRect.h, input.motionScaleX, input.motionScaleY, Ctx().motionScaleAdjust.load(),
            Ctx().motionInvert.load() ? ", inverted" : "");
        if (probeTail && *probeTail) Log(false, "%s", probeTail);
        Log(false, "Optimizer FPS NGX hook: host model inputs: UI %p, UIAlpha %p, Backbuffer %p (%s), %s",
            (void *) ui, (void *) uiAlpha, (void *) backbuffer,
            (st.modelResolution ? !st.withholdUi : KeepBackbufferEnabled())
                ? "kept for the warped model (debug)" : "withheld from the warped model",
            modelTail ? modelTail : "");
    }
    const std::uint32_t slot = packSlot;
    const std::uint32_t unpackSlot = FeatureState::kPackSlots + packSlot;
    if (status != ofps::sdk::AdapterStatus::Ok) {
        ID3D12Device *colorDevice = nullptr, *depthDevice = nullptr, *motionDevice = nullptr;
        color->GetDevice(IID_PPV_ARGS(&colorDevice));
        depth->GetDevice(IID_PPV_ARGS(&depthDevice));
        motion->GetDevice(IID_PPV_ARGS(&motionDevice));
        SetReason("pack descriptors: %s (colour %d %ux%u mips %u flags %x, depth %d %ux%u mips %u flags %x, motion %d %ux%u mips %u flags %x, rects %ux%u/%ux%u/%ux%u, devices %p/%p/%p vs %p)",
                  ofps::sdk::AdapterStatusString(status), (int) colorDesc.Format, (unsigned) colorDesc.Width, colorDesc.Height,
                  colorDesc.MipLevels, (unsigned) colorDesc.Flags, (int) depthDesc.Format, (unsigned) depthDesc.Width,
                  depthDesc.Height, depthDesc.MipLevels, (unsigned) depthDesc.Flags, (int) motionDesc.Format,
                  (unsigned) motionDesc.Width, motionDesc.Height, motionDesc.MipLevels, (unsigned) motionDesc.Flags,
                  colorRect.w, colorRect.h, depthRect.w, depthRect.h, motionRect.w, motionRect.h, (void *) colorDevice,
                  (void *) depthDevice, (void *) motionDevice, (void *) st.device);
        if (colorDevice) colorDevice->Release();
        if (depthDevice) depthDevice->Release();
        if (motionDevice) motionDevice->Release();
        if (st.modelResolution) {
            st.disabled = true;
            return ModelGridFallback(st, cmd, frame, Ctx().status.reason, OFPS_E_DEVICE);
        }
        st.disabled = true;
        Log(true, "Optimizer FPS NGX hook: %s; re-creating the model at native size", Ctx().status.reason);
        RetireGpu(st);
        const int recreated = RecreateReal(st, cmd, st.nativeWidth, st.nativeHeight);
        if (recreated < 0) return recreated;
        if (st.modelPending) return CreationFrame(st, cmd, frame, evalResult);
        void *real = st.realHandle;
        if (real == nullptr) return OFPS_E_DEVICE;
        return EvaluateModelPasses(st, cmd, modelInputs);
    }

    // 26.21: the crash guard's marker is written here, before anything of the warped path is recorded.
    if (st.warped) NotifyFirstWarped();

    EvalContext c{};
    c.st = &st;
    c.cmd = cmd;
    c.modelInputs = modelInputs;
    c.frame = &frame;
    c.codec = &codec;
    c.color = color;
    c.colorResource = codec.modelColor;
    c.motionResource = modelInputs.motion;
    c.outputResource = frame.output;
    c.depth = depth;
    c.depthState = HostDepthState(frame.depth);
    c.depthSub = frame.depth.subresource;
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
    c.packSources = sources;
    c.packInput = input;
    c.unpackSlot = unpackSlot;
    c.depthConvention = input.depthConvention;
    c.temporalActive = plan.active;
    c.temporalFull = plan.full;
    c.accumulate = plan.active && (!plan.full || useAcc);
    c.motionIsAcc = useAcc;
    c.modelMotion = useModelMv;
    c.tin = tin;
    c.wantBase = codec.identity && plan.active && Ctx().temporal.warpBase && st.unpackBase != nullptr;
    c.baseTarget = st.unpackBase;
    c.baseTargetState = &st.unpackBaseState;
    if (st.rtvHeap) c.baseRtv = BaseRtv(st);

    // Background mode in the warped path: the host context carries the pack slot for the base.
    if (EffectiveTemporalMode(st) == 3 && plan.active) {
        const int handled = AsyncTemporalEvaluate(st, cmd, modelInputs, &c, frame);
        if (handled != kNotHandled) { if (evalResult.path != OFPS_PATH_FALLBACK) evalResult.path = st.frameModelCalled ? (st.warped ? OFPS_PATH_WARPED : OFPS_PATH_PASSTHROUGH) : OFPS_PATH_CARRIED; return handled; }
    }

    if (plan.active && !plan.full) {
        // Interpolated frame: the model rests.
        evalResult.path = OFPS_PATH_CARRIED;
        const int r = InterpolateGuarded(c);
        if (TemporalExhausted(st)) { FallbackOutput(c, "temporal descriptor pool exhausted"); return OFPS_OK; }
        DumpDebugLayer(st.realDevice);
        TemporalDebugReadback(st, cmd, c.tin);
        if (r == kCrashed) {
            st.temporalDisabled = true;
            TemporalReason(st, "exception 0x%08lX in stage %s; temporal mode disabled for this feature", Ctx().crash.code, StageName(Ctx().crash.stage));
            Log(true, "Optimizer FPS NGX hook: exception 0x%08lX at %p (%s) during stage %d (%s); temporal mode disabled",
                Ctx().crash.code, Ctx().crash.address, Ctx().crash.module, Ctx().crash.stage, StageName(Ctx().crash.stage));
            FallbackOutput(c, "exception on an interpolated frame");
            return OFPS_OK; // the next frame runs the model
        }
        if (r != OFPS_OK) { FallbackOutput(c, Ctx().status.reason); return OFPS_OK; }
        ++Ctx().status.evaluations;
        Ctx().status.active = true;
        Ctx().status.reason[0] = 0;
        TemporalFrameDone(st, false);
        return OFPS_OK;
    }

    evalResult.path = OFPS_PATH_WARPED;
    evalResult.warpPath = st.warpPath == warp::PackPath::Compute ? OFPS_WARP_COMPUTE : OFPS_WARP_PIXEL;
    const int result = WarpedGuarded(c);
    if (TemporalExhausted(st)) { FallbackOutput(c, "temporal descriptor pool exhausted"); return OFPS_OK; }
    DumpDebugLayer(st.realDevice);
    if (plan.active && result == OFPS_OK) TemporalFrameDone(st, plan.full);
    if (result == kCrashed) {
        // Something faulted inside the warped path. Give the block back to the host as far as that
        // is possible, stop warping this feature, and let the next evaluate rebuild the native model.
        st.disabled = true;
        Ctx().status.active = false;
        Log(true, "Optimizer FPS NGX hook: exception 0x%08lX at %p (%s) during stage %d (%s); warp disabled for this feature",
            Ctx().crash.code, Ctx().crash.address, Ctx().crash.module, Ctx().crash.stage, StageName(Ctx().crash.stage));
        SetReason("exception 0x%08lX in stage %s; warp disabled", Ctx().crash.code, StageName(Ctx().crash.stage));
        FallbackOutput(c, "exception on a warped frame");
        return OFPS_E_DEVICE;
    }
    if (result == kPackFailed) {
        evalResult.path = OFPS_PATH_FALLBACK;
        evalResult.warpPath = OFPS_WARP_NONE;
        if (st.modelResolution) {
            st.disabled = true;
            FallbackOutput(c, "pack failed");
            return OFPS_E_DEVICE;
        }
        st.disabled = true;
        Log(true, "Optimizer FPS NGX hook: %s; re-creating the model at native size", Ctx().status.reason);
        RetireGpu(st);
        const int recreated = RecreateReal(st, cmd, st.nativeWidth, st.nativeHeight);
        if (recreated < 0) return recreated;
        if (st.modelPending) return CreationFrame(st, cmd, frame, evalResult);
        void *realNative = st.realHandle;
        if (realNative == nullptr) return OFPS_E_DEVICE;
        return EvaluateModelPasses(st, cmd, modelInputs);
    }
    return result;
}

} // namespace ofps::core
