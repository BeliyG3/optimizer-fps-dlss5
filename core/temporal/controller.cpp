#include "core/gpu/host_list.h"
#include "core/frame/model_passes.h"
#include "core/frame/codec_frame.h"
#include "core/frame/model_grid.h"
#include "core/frame/model_grid_path.h"
#include "core/frame/spread_passes.h"
#include "core/temporal/controller.h"
#include "core/temporal/diagnostics.h"

#include "core/frame/debug_readback.h"
#include "core/context.h"
#include "core/frame/host_depth_state.h"
#include "core/frame/timing.h"
#include "core/frame/warp_recorder.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>

namespace ofps::core {

void TemporalReason(FeatureState &st, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(Ctx().status.temporalReason, sizeof(Ctx().status.temporalReason), fmt, args);
    va_end(args);
    if (!st.temporalLogged) {
        st.temporalLogged = true;
        Log(true, "Optimizer FPS NGX hook: temporal mode off: %s", Ctx().status.temporalReason);
    }
}

bool EnsureTemporalDevice(FeatureState &st, ID3D12GraphicsCommandList *cmd, ID3D12Resource *output)
{
    if (st.device == nullptr) {
        if (FAILED(cmd->GetDevice(IID_PPV_ARGS(&st.device))) || st.device == nullptr) {
            TemporalReason(st, "command list has no device");
            return false;
        }
    }
    if (st.realDevice == nullptr && (output == nullptr || FAILED(output->GetDevice(IID_PPV_ARGS(&st.realDevice))))) {
        TemporalReason(st, "output has no device");
        return false;
    }
    return true;
}

// Creates or re-creates the machine for the current host textures. False (with a reason) when the
// temporal mode cannot run on this feature.
bool EnsureTemporal(FeatureState &st, ID3D12GraphicsCommandList *cmd, ID3D12Resource *output, ID3D12Resource *motion,
                    ID3D12Resource *depth)
{
    if (st.temporalDisabled || !EnsureTemporalDevice(st, cmd, output)) return false;
    if (!LoadShaders() || !Ctx().shaders.TemporalLoaded()) {
        st.temporalDisabled = true;
        TemporalReason(st, "temporal_*.dxbc missing in optimizer-fps-dlss5\\ beside the add-on");
        return false;
    }
    const D3D12_RESOURCE_DESC outputDesc = output->GetDesc();
    const D3D12_RESOURCE_DESC motionDesc = motion->GetDesc();
    const D3D12_RESOURCE_DESC depthDesc = depth->GetDesc();
    const auto pictureDivisor = static_cast<std::uint32_t>(ofps::temporal::Value(
        Ctx().temporalDiagnostics, ofps::temporal::DiagnosticId::HistoryDiv, 3.0f));
    if (st.temporal && st.temporal->Matches(st.nativeWidth, st.nativeHeight, outputDesc.Format, (std::uint32_t) motionDesc.Width,
                                            motionDesc.Height, depthDesc.Format, (std::uint32_t) depthDesc.Width, depthDesc.Height,
                                            pictureDivisor)) {
        st.temporal->SetUsePoint(HostUsePoint(cmd), Ctx().evalCounter, Ctx().diag.timing);
        return true;
    }
    if (st.temporal) {
        // The old machine's textures may be in flight: bury it, build a new one.
        ofps::core::gpu::Grave g;
        g.modelHost = st.modelHost;
        g.gate = RetirementGate(st);
        g.disposables.push_back(std::move(st.temporal));
        Ctx().graveyard.Add(std::move(g), Ctx().evalCounter);
    }
    auto machine = std::make_unique<ofps::core::temporal::Machine>();
    char error[256] = {};
    if (!machine->Initialize(st.device, Ctx().shaders, st.nativeWidth, st.nativeHeight, outputDesc.Format, TypedView(outputDesc.Format, false),
                             (std::uint32_t) motionDesc.Width, motionDesc.Height, depthDesc.Format, (std::uint32_t) depthDesc.Width,
                             depthDesc.Height, error, sizeof(error), pictureDivisor)) {
        st.temporalDisabled = true;
        TemporalReason(st, "%s", error);
        return false;
    }
    st.temporal = std::move(machine);
    st.temporal->SetUsePoint(HostUsePoint(cmd), Ctx().evalCounter, Ctx().diag.timing);
    st.sinceFull = 0;
    Log(false, "Optimizer FPS NGX hook: temporal machine ready (native %ux%u, motion %llux%u, depth %llux%u fmt %d, output fmt %d)",
        st.nativeWidth, st.nativeHeight, (unsigned long long) motionDesc.Width, motionDesc.Height, (unsigned long long) depthDesc.Width,
        depthDesc.Height, (int) depthDesc.Format, (int) outputDesc.Format);
    return true;
}

// Decides what this frame is. Requires the machine; `hostReset` forces a full pass.
int TryTemporalRoute(FeatureState &st, ID3D12GraphicsCommandList *cmd,
                     const OfpsModelInputs &inputs, const OfpsFrameInputs &frame, OfpsEvalResult &result) {
    const auto recordPath = [&] {
        if (result.path != OFPS_PATH_FALLBACK)
            result.path = st.frameModelCalled ? (st.warped ? OFPS_PATH_WARPED : OFPS_PATH_PASSTHROUGH)
                                              : OFPS_PATH_CARRIED;
    };
    if (SpreadRequested(st)) {
        const int spread = SpreadEvaluate(st, cmd, inputs, frame);
        if (spread == OFPS_S_MODEL_NEXT_FRAME) {
            result.path = OFPS_PATH_CREATION_FRAME;
            return OFPS_OK;
        }
        if (spread != kNotHandled) {
            recordPath();
            return spread;
        }
    } else if (st.spread) {
        RetireSpread(st, ofps::core::gpu::SignalGate(st.realDevice, st.device));
        if (st.temporal) st.temporal->Invalidate();
    }

    // Layout recreation retires both device aliases. Restore them before checking host queues.
    const bool temporalDeviceReady = st.modelResolution || Ctx().temporal.mode != 3 ||
                                     EnsureTemporalDevice(st, cmd, frame.output.res);
    const int effectiveMode = temporalDeviceReady ? EffectiveTemporalMode(st) : 1;
    if (!st.warped && !st.disabled && effectiveMode == 3) {
        const int handled = AsyncTemporalEvaluate(st, cmd, inputs, nullptr, frame);
        if (handled != kNotHandled) {
            recordPath();
            return handled;
        }
    }
    const bool modelGridCarry = st.modelResolution && st.warped && st.temporal &&
        !PlanTemporal(st, frame.hostReset != 0 || st.forcedReset).full;
    if ((!st.warped || modelGridCarry) && !st.disabled && effectiveMode != 0 && effectiveMode != 3) {
        const int handled = NativeTemporalEvaluate(st, cmd, inputs, frame);
        if (handled != kNotHandled) {
            if (handled == OFPS_S_MODEL_NEXT_FRAME)
                return CreationFrame(st, cmd, frame, result);
            recordPath();
            return handled;
        }
    }
    return kNotHandled;
}

TemporalPlan PlanTemporal(FeatureState &st, bool hostReset)
{
    TemporalPlan p;
    const int mode = Ctx().temporal.mode == 3 ? 1 : Ctx().temporal.mode; // the warped path runs the background mode as the sync one until it moves to the background queue
    const int every = std::clamp(Ctx().temporal.every, 2, 8);
    p.active = mode != 0 && st.temporal && st.temporal->Ready();
    if (!p.active) return p;
    if (hostReset) st.temporal->Invalidate();
    p.full = !st.temporal->HasResidual() || hostReset || st.sinceFull >= static_cast<std::uint32_t>(every - 1);
    return p;
}

ofps::core::temporal::FrameInputs TemporalInputs(const OfpsResource &color, const OfpsResource &motion,
                                                 const OfpsResource &depth, float mvScaleX, float mvScaleY,
                                                 bool depthInverted, bool hostModelGrid) {
    ofps::core::temporal::FrameInputs t;
    const auto& diagnostics = Ctx().temporalDiagnostics;
    const auto& profile = Ctx().frameProfile;
    using ofps::temporal::DiagnosticId;
    using ofps::temporal::Value;
    t.color = color.res;
    t.motion = motion.res;
    t.depth = depth.res;
    t.colorView = color.view;
    t.motionView = motion.view;
    t.depthView = depth.view;
    t.colorRect = {color.rect.x, color.rect.y, color.rect.w, color.rect.h};
    t.motionRect = {motion.rect.x, motion.rect.y, motion.rect.w, motion.rect.h};
    t.depthRect = {depth.rect.x, depth.rect.y, depth.rect.w, depth.rect.h};
    t.mvScaleX = mvScaleX;
    t.mvScaleY = mvScaleY;
    t.depthInverted = depthInverted;
    t.hostModelGrid = hostModelGrid;
    t.hostInputState = color.restState;
    t.motionState = motion.restState;
    t.depthState = HostDepthState(depth);
    t.colorSubresource = color.subresource;
    t.motionSubresource = motion.subresource;
    t.depthSubresource = depth.subresource;
    t.depthThreshold = Ctx().diag.temporalDepth >= 0.0f ? Ctx().diag.temporalDepth : profile.depthTolerance;
    t.debugVis = static_cast<int>(Value(diagnostics, DiagnosticId::View, static_cast<float>(Ctx().temporal.debugView)));
    t.colorTolerance = profile.colourTolerance;
    t.ratioScale = profile.ratioScale;
    t.lumaBand = profile.lumaBand;
    t.fillFloor = profile.fillFloor;
    t.rawFull = Value(diagnostics, DiagnosticId::RawFull, 0.0f) != 0.0f;
    t.history = Value(diagnostics, DiagnosticId::NoHistory, 0.0f) == 0.0f;
    t.historySearch = Value(diagnostics, DiagnosticId::NoHistorySearch, 0.0f) == 0.0f;
    t.refine = Value(diagnostics, DiagnosticId::NoRefine, 0.0f) == 0.0f;
    t.compose = Value(diagnostics, DiagnosticId::NoCompose, 0.0f) == 0.0f;
    t.historyPasses = static_cast<std::uint32_t>(Value(diagnostics, DiagnosticId::HistoryPasses, 2.0f));
    t.motionSign = Ctx().temporal.flipMotionSign ? -1.0f : 1.0f;
    t.opticalFlow = Ctx().temporalMotionSource.load() == 1;
    t.rawInterpolation = Ctx().temporal.debugRawInterpolation;
    t.holeFill = Ctx().temporal.holeFill && Value(diagnostics, DiagnosticId::NoFill, 0.0f) == 0.0f;
    t.mvSearchRadiusPx = Ctx().temporal.mvSearchRadiusPx;
    {
        const float smooth = Ctx().diag.temporalSmooth; // DebugTemporalSmooth overrides the radius (0 = off)
        t.smoothRadius = smooth >= 0.0f ? smooth : std::clamp(Ctx().temporal.smoothRadiusPx, 0.0f, 128.0f);
    }
    t.residualCatmullRom = Ctx().temporal.residualCatmullRom;
    // 26.28: the passes ported from the OptiScaler fork. A new pass fades in over the shorter of the
    // cadence and three frames - long enough that the model's new opinion of the whole picture is not
    // a click, short enough that the picture is not held back by a residual two passes old.
    t.expectedDepth = !Ctx().diag.temporalNoExpect;
    t.cells = !Ctx().diag.temporalNoCells;
    const int every = std::clamp(Ctx().temporal.every, 2, 8);
    t.phaseInFrames = Ctx().diag.temporalPhaseIn >= 0 ? static_cast<std::uint32_t>(Ctx().diag.temporalPhaseIn)
                                                      : static_cast<std::uint32_t>(std::min(every - 1, 3));
    return t;
}

bool TemporalExhausted(FeatureState &st)
{
    bool exhausted = st.temporal && st.temporal->TakeExhausted();
    if (st.spread) for (auto &hidden : st.spread->hidden)
        if (hidden) exhausted |= hidden->TakeExhausted();
    if (exhausted) {
        if (st.temporal) st.temporal->Invalidate();
        if (st.spread) {
            for (auto &hidden : st.spread->hidden) if (hidden) hidden->Invalidate();
            st.spread->position = 0;
        }
        TemporalReason(st, "descriptor pool exhausted; showing host colour");
    }
    return exhausted;
}

void TemporalFrameDone(FeatureState &st, bool full)
{
    Ctx().status.temporalFlowRequested = Ctx().temporalMotionSource.load() == 1;
    Ctx().status.temporalFlowRunning = st.temporal && st.temporal->FlowRunning();
    if (st.temporal)
        std::snprintf(Ctx().status.temporalFlowReason, sizeof(Ctx().status.temporalFlowReason),
                      "%s", st.temporal->FlowProblem().c_str());
    if (full) {
        st.sinceFull = 0;
        ++Ctx().status.fullFrames;
    } else {
        ++st.sinceFull;
        ++Ctx().status.interpFrames;
    }
    if (Ctx().status.fullFrames + Ctx().status.interpFrames == 2)
        Log(false, "Optimizer FPS NGX hook: temporal mode %d running (full pass every %d frames%s)", Ctx().temporal.mode,
            std::clamp(Ctx().temporal.every, 2, 8), Ctx().temporal.mode == 2 ? ", centre every frame" : "");
    if (ofps::temporal::Value(Ctx().temporalDiagnostics, ofps::temporal::DiagnosticId::Log, 0.0f) != 0.0f)
        Log(false, "Optimizer FPS temporal: frame=%llu mode=%d full=%d age=%u",
            static_cast<unsigned long long>(Ctx().evalCounter), Ctx().temporal.mode, full ? 1 : 0, st.sinceFull);
    if (st.temporal) {
        const auto stats = st.temporal->Stats();
        const bool everySample = ofps::temporal::Value(Ctx().temporalDiagnostics,
            ofps::temporal::DiagnosticId::StatsEvery, 0.0f) != 0.0f;
        if (!stats.reason && stats.samples != st.temporalStatsLogged &&
            (stats.samples == 1 || stats.samples % 120 == 0 || everySample)) {
            const auto& all = everySample ? stats.all : stats.windowAll;
            const auto& centre = everySample ? stats.centre : stats.windowCentre;
            const auto& edges = everySample ? stats.edges : stats.windowEdges;
            Log(false, "Optimizer FPS temporal acceptance: samples=%llu floor=%d window=%u W/R/NR/NF%% all=%.2f/%.2f/%.2f/%.2f centre=%.2f/%.2f/%.2f/%.2f edges=%.2f/%.2f/%.2f/%.2f skipped=%llu",
                static_cast<unsigned long long>(stats.samples), stats.fillFloor ? 1 : 0,
                everySample ? 1u : stats.windowFrames,
                100 * all.weight, 100 * all.rejected, 100 * all.noRing, 100 * all.noFill,
                100 * centre.weight, 100 * centre.rejected, 100 * centre.noRing, 100 * centre.noFill,
                100 * edges.weight, 100 * edges.rejected, 100 * edges.noRing, 100 * edges.noFill,
                static_cast<unsigned long long>(stats.skipped));
        }
        st.temporalStatsLogged = stats.samples;
    }
}

// ---------------------------------------------------------------------------------------------
// Temporal modes without the warp (Mode Off): the model runs at native size on full frames (and on
// the centre sub-rect in mode 2), the machine fills the rest. Returns kNotHandled to fall back to
// the plain pass-through.
// ---------------------------------------------------------------------------------------------
struct NativeTemporal {
    FeatureState *st;
    ID3D12GraphicsCommandList *cmd;
    OfpsModelInputs inputs;
    OfpsFrameInputs frame;
    ID3D12Resource *output;
    OfpsRect outputRect;
    ofps::core::temporal::FrameInputs tin;
    bool full;
    volatile int stage;
    int result;
};

int NativeTemporalBody(NativeTemporal &n)
{
    FeatureState &st = *n.st;
    ID3D12GraphicsCommandList *cmd = n.cmd;
    if (!n.full && n.frame.color.res == n.frame.output.res) {
        OfpsResource colorCopy{};
        const int copied = SnapshotFrameColor(st, cmd, n.frame.color, colorCopy);
        if (copied != OFPS_OK) return copied;
        n.tin.color = colorCopy.res;
        n.tin.colorView = colorCopy.view;
        n.tin.colorRect = {colorCopy.rect.x, colorCopy.rect.y, colorCopy.rect.w, colorCopy.rect.h};
        n.tin.hostInputState = colorCopy.restState;
        n.tin.colorSubresource = colorCopy.subresource;
    }
    const bool accumulate = !n.full || st.temporal->AccValid();
    if (accumulate && !n.full) {
        n.stage = StageTemporalAccumulate;
        st.temporal->RecordAccumulate(cmd, n.tin);
    }
    if (!n.full) {
        n.stage = StageTemporalReproject;
        st.temporal->RecordReproject(cmd, n.tin, KeepOutputOnInterpolation() ? nullptr : n.output, n.frame.output.restState,
                                     n.outputRect.x, n.outputRect.y, n.frame.output.subresource);
        st.temporal->RecordFlowCapture(cmd, n.tin, st.realDevice, false);
        n.stage = StageDone;
        n.result = OFPS_OK;
        return OFPS_OK;
    }
    CodecFrame codec{};
    int prepared = BeginCodecFrame(st, cmd, n.frame, codec);
    if (prepared == OFPS_OK) prepared = ConfigureModelGrid(st, cmd, codec);
    if (prepared != OFPS_OK) {
        if (prepared == OFPS_S_MODEL_NEXT_FRAME) return prepared;
        n.stage = StageCopyOut;
        FallbackOutput(st, cmd, n.frame.color, n.frame.output, &n.tin, "codec preparation or model grid setup");
        return prepared;
    }
    if (accumulate) {
        n.stage = StageTemporalAccumulate;
        st.temporal->RecordAccumulate(cmd, n.tin);
    }
    ModelGridInputs(st, codec, n.frame, n.inputs);
    n.stage = StageParamsWrite;
    if (st.temporal->AccValid() && !Ctx().temporal.debugSingleFrameMotion) {
        // The model's history is as old as the residual: give it the accumulated displacement. 26.28:
        // where the chain does not end on the pixel's own surface in the residual's frame, hand the
        // model a vector that leaves the picture instead - there is no history for it there.
        ID3D12Resource *vectors = st.temporal->Acc();
        if (!Ctx().diag.temporalNoModelMotion && st.temporal->ModelMv() != nullptr) {
            st.temporal->RecordModelMotion(cmd, n.tin);
            vectors = st.temporal->ModelMv();
        }
        n.inputs.motion.res = vectors;
        n.inputs.motion.view = DXGI_FORMAT_R16G16_FLOAT;
        n.inputs.motion.restState = kModelInputState;
        n.inputs.motion.subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        // Chain vectors are measured on the frame grid; NR consumes them on the
        // model input grid when the host codec changes resolution.
        n.inputs.mvScaleX = static_cast<float>(codec.modelColor.rect.w) / n.frame.color.rect.w;
        n.inputs.mvScaleY = static_cast<float>(codec.modelColor.rect.h) / n.frame.color.rect.h;
    }
    n.stage = StageModel;
    TimingBegin(st, cmd);
    int result = EvaluateModelPasses(st, cmd, n.inputs);
    if (result == OFPS_OK) result = ResolveCodecFrame(st, cmd, codec);
    TimingEnd(st, cmd);
    Ctx().status.lastNgxResult = result;
    n.result = result;
    if (result != OFPS_OK) {
        char why[96];
        std::snprintf(why, sizeof(why), "model returned %d on a native temporal frame", result);
        n.stage = StageCopyOut;
        FallbackOutput(st, cmd, n.frame.color, n.frame.output, &n.tin, why);
        return result;
    }
    n.stage = StageTemporalResidual;
    ofps::core::temporal::FrameInputs tinR = n.tin;
    if (codec.frameBefore.res) {
        tinR.color = codec.frameBefore.res;
        tinR.colorView = codec.frameBefore.view;
        tinR.colorRect = {codec.frameBefore.rect.x, codec.frameBefore.rect.y, codec.frameBefore.rect.w, codec.frameBefore.rect.h};
        tinR.hostInputState = codec.frameBefore.restState;
        tinR.colorSubresource = codec.frameBefore.subresource;
    }
    tinR.residualBlend = Ctx().temporal.debugSingleFrameMotion ? 0.0f : kResidualBlend;
    st.temporal->RecordResidual(cmd, tinR, n.output, n.frame.output.restState, n.frame.output.subresource);
    // 26.28: while a pass is being phased in, the full frame is shown as "colour + the residual mix"
    // too - otherwise the phase-in would only steady the carried frames and the full frame itself
    // would still snap to the model's new version once per cadence.
    st.temporal->RecordApply(cmd, tinR, n.output, n.frame.output.restState, n.outputRect.x, n.outputRect.y, n.frame.output.subresource);
    st.temporal->RecordFlowCapture(cmd, tinR, st.realDevice, true);
    n.stage = StageDone;
    return result;
}

int NativeTemporalGuarded(NativeTemporal &n)
{
    __try {
        return NativeTemporalBody(n);
    } __except (RecordCrash(GetExceptionInformation(), n.stage)) {
        return kCrashed;
    }
}

int NativeTemporalEvaluate(FeatureState &st, ID3D12GraphicsCommandList *cmd, const OfpsModelInputs &inputs, const OfpsFrameInputs &frame)
{
    ID3D12Resource *color = inputs.color.res;
    ID3D12Resource *depth = inputs.depth.res;
    ID3D12Resource *motion = inputs.motion.res;
    ID3D12Resource *output = inputs.output.res;
    if (color == nullptr || depth == nullptr || motion == nullptr || output == nullptr || !gpu::HostListRecordable(cmd))
        return kNotHandled;
    const D3D12_RESOURCE_DESC colorDesc = color->GetDesc();
    const D3D12_RESOURCE_DESC outputDesc = output->GetDesc();
    if (colorDesc.Width < st.nativeWidth || colorDesc.Height < st.nativeHeight || outputDesc.Width < st.nativeWidth ||
        outputDesc.Height < st.nativeHeight) {
        TemporalReason(st, "host colour/output smaller than the feature (%llux%u / %llux%u)", (unsigned long long) colorDesc.Width,
                       colorDesc.Height, (unsigned long long) outputDesc.Width, outputDesc.Height);
        return kNotHandled;
    }
    if (!EnsureTemporal(st, cmd, output, motion, depth)) return kNotHandled;
    const unsigned int hostReset = inputs.reset;
    TemporalPlan plan = PlanTemporal(st, hostReset != 0);
    Ctx().status.temporalMode = plan.active ? Ctx().temporal.mode : 0;
    if (!plan.active) return kNotHandled;
    Ctx().status.temporalReason[0] = 0;

    NativeTemporal n{};
    n.st = &st;
    n.cmd = cmd;
    n.inputs = inputs;
    n.frame = frame;
    n.output = output;
    n.outputRect = OfpsRect{inputs.output.rect.x, inputs.output.rect.y, inputs.output.rect.w, inputs.output.rect.h};
    const unsigned int depthInverted = inputs.depthInverted;
    float mvScaleX = inputs.mvScaleX, mvScaleY = inputs.mvScaleY;
    if (!std::isfinite(mvScaleX) || mvScaleX == 0.0f) mvScaleX = 1.0f;
    if (!std::isfinite(mvScaleY) || mvScaleY == 0.0f) mvScaleY = 1.0f;
    n.tin = TemporalInputs(inputs.color, inputs.motion, inputs.depth, mvScaleX, mvScaleY, depthInverted != 0, st.modelResolution);
    // 26.26: the depth guide's resting state and its barrier subresource, as everywhere else the machine
    // touches the host's depth. Without them a planar depth-stencil host (D24S8/D32S8) took whole-resource
    // barriers and copies in the native temporal path.
    n.tin.depthState = HostDepthState(inputs.depth);
    n.tin.depthSubresource = inputs.depth.subresource;
    n.full = plan.full;
    const int result = NativeTemporalGuarded(n);
    if (TemporalExhausted(st)) {
        FallbackOutput(st, cmd, frame.color, frame.output, &n.tin, "temporal descriptor pool exhausted");
        return OFPS_OK;
    }
    if (result == OFPS_OK && !plan.full) TemporalDebugReadback(st, cmd, n.tin);
    if (result == kCrashed) {
        st.temporalDisabled = true;
        TemporalReason(st, "exception 0x%08lX in stage %s; temporal mode disabled for this feature", Ctx().crash.code, StageName(Ctx().crash.stage));
        Log(true, "Optimizer FPS NGX hook: exception 0x%08lX at %p (%s) during stage %d (%s); temporal mode disabled",
            Ctx().crash.code, Ctx().crash.address, Ctx().crash.module, Ctx().crash.stage, StageName(Ctx().crash.stage));
        return n.stage >= StageModel && n.result != 0 ? n.result : OFPS_OK;
    }
    if (result == OFPS_OK) {
        ++Ctx().status.evaluations;
        Ctx().status.active = true;
        SetReason("temporal mode %d (no warp)", Ctx().temporal.mode);
        TemporalFrameDone(st, plan.full);
    }
    return result;
}

} // namespace ofps::core
