#include "temporal_controller.h"

#include "debug_readback.h"
#include "hook_context.h"
#include "host_depth_state.h"
#include "ngx_params.h"
#include "timing.h"
#include "warp_recorder.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>

namespace pwhook {

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

// Creates or re-creates the machine for the current host textures. False (with a reason) when the
// temporal mode cannot run on this feature.
bool EnsureTemporal(FeatureState &st, ID3D12GraphicsCommandList *cmd, ID3D12Resource *output, ID3D12Resource *motion,
                    ID3D12Resource *depth)
{
    if (st.temporalDisabled) return false;
    if (st.device == nullptr) {
        if (FAILED(cmd->GetDevice(IID_PPV_ARGS(&st.device))) || st.device == nullptr) {
            TemporalReason(st, "command list has no device");
            return false;
        }
        output->GetDevice(IID_PPV_ARGS(&st.realDevice));
    }
    if (!LoadShaders() || !Ctx().shaders.TemporalLoaded()) {
        st.temporalDisabled = true;
        TemporalReason(st, "temporal_*.dxbc missing in optimizer-fps-dlss5\\ beside the add-on");
        return false;
    }
    const D3D12_RESOURCE_DESC outputDesc = output->GetDesc();
    const D3D12_RESOURCE_DESC motionDesc = motion->GetDesc();
    const D3D12_RESOURCE_DESC depthDesc = depth->GetDesc();
    if (st.temporal && st.temporal->Matches(st.nativeWidth, st.nativeHeight, outputDesc.Format, (std::uint32_t) motionDesc.Width,
                                            motionDesc.Height, depthDesc.Format, (std::uint32_t) depthDesc.Width, depthDesc.Height))
        return true;
    if (st.temporal) {
        // The old machine's textures may be in flight: bury it, build a new one.
        pwngx::Grave g;
        g.disposables.push_back(std::move(st.temporal));
        Ctx().graveyard.Add(std::move(g), Ctx().evalCounter);
    }
    auto machine = std::make_unique<pwtemporal::Machine>();
    char error[256] = {};
    if (!machine->Initialize(st.device, Ctx().shaders, st.nativeWidth, st.nativeHeight, outputDesc.Format, TypedView(outputDesc.Format, false),
                             (std::uint32_t) motionDesc.Width, motionDesc.Height, depthDesc.Format, (std::uint32_t) depthDesc.Width,
                             depthDesc.Height, error, sizeof(error))) {
        st.temporalDisabled = true;
        TemporalReason(st, "%s", error);
        return false;
    }
    st.temporal = std::move(machine);
    st.sinceFull = 0;
    Log(false, "Optimizer FPS NGX hook: temporal machine ready (native %ux%u, motion %llux%u, depth %llux%u fmt %d, output fmt %d)",
        st.nativeWidth, st.nativeHeight, (unsigned long long) motionDesc.Width, motionDesc.Height, (unsigned long long) depthDesc.Width,
        depthDesc.Height, (int) depthDesc.Format, (int) outputDesc.Format);
    return true;
}

// Decides what this frame is. Requires the machine; `hostReset` forces a full pass.
// 26.24: the background mode needs the host's command queue registered (it signals the queue after the host
// submits our copies and waits on it). Hosts whose D3D12 device was created before ReShade loaded (OptiScaler
// in Fallen Order) register no queue: every pass then ran into the forced wait at the age limit, 0.3 passes/s
// and a stall on each - "async does not start, everything stutters". Without a queue the synchronous
// interpolation runs instead, and the tab/log say so.
bool BackgroundModeUsable(FeatureState &st)
{
    if (Ctx().temporal.mode != 3) return false;
    std::size_t total = 0;
    const bool ok = pwngx::CountQueues(st.realDevice, st.device, &total) > 0;
    static bool s_said = false;
    if (!ok && !s_said) {
        s_said = true;
        Log(true, "Optimizer FPS NGX hook: background mode needs the host's D3D12 queue and none is registered in this process (device created before ReShade loaded); the synchronous interpolation runs instead");
        TemporalReason(st, "background mode unavailable here (no registered host queue); running the synchronous interpolation");
    }
    return ok;
}

int EffectiveTemporalMode(FeatureState &st) { return (Ctx().temporal.mode == 3 && !BackgroundModeUsable(st)) ? 1 : Ctx().temporal.mode; }

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

pwtemporal::FrameInputs TemporalInputs(ID3D12Resource *color, ID3D12Resource *motion, ID3D12Resource *depth, DXGI_FORMAT colorView,
                                       DXGI_FORMAT motionView, DXGI_FORMAT depthView, const Subrect &colorRect, const Subrect &motionRect,
                                       const Subrect &depthRect, float mvScaleX, float mvScaleY, bool depthInverted)
{
    pwtemporal::FrameInputs t;
    t.color = color;
    t.motion = motion;
    t.depth = depth;
    t.colorView = colorView;
    t.motionView = motionView;
    t.depthView = depthView;
    t.colorRect = {colorRect.x, colorRect.y, colorRect.w, colorRect.h};
    t.motionRect = {motionRect.x, motionRect.y, motionRect.w, motionRect.h};
    t.depthRect = {depthRect.x, depthRect.y, depthRect.w, depthRect.h};
    t.mvScaleX = mvScaleX;
    t.mvScaleY = mvScaleY;
    t.depthInverted = depthInverted;
    t.hostInputState = kHostInputState;
    t.depthThreshold = Ctx().diag.temporalDepth >= 0.0f ? Ctx().diag.temporalDepth : Ctx().temporal.depthTolerance; // DebugTemporalDepth
    t.debugVis = Ctx().temporal.debugView; // the tab's "Debug view of interpolated frames" combo is the only source
    t.colorTolerance = Ctx().temporal.colorTolerance;
    t.motionSign = Ctx().temporal.flipMotionSign ? -1.0f : 1.0f;
    t.rawInterpolation = Ctx().temporal.debugRawInterpolation;
    t.holeFill = Ctx().temporal.holeFill;
    t.mvSearchRadiusPx = Ctx().temporal.mvSearchRadiusPx;
    {
        const float smooth = Ctx().diag.temporalSmooth; // DebugTemporalSmooth overrides the radius (0 = off)
        t.smoothRadius = smooth >= 0.0f ? smooth : std::clamp(Ctx().temporal.smoothRadiusPx, 0.0f, 128.0f);
    }
    t.residualCatmullRom = Ctx().temporal.residualCatmullRom;
    return t;
}

// DebugTemporalReadback=1: every 60th interpolated frame, print centre texels of the host colour,
// the residual, the interpolated output and the displacement (costs a GPU wait; debug only).
void TemporalFrameDone(FeatureState &st, bool full)
{
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
}

// ---------------------------------------------------------------------------------------------
// Temporal modes without the warp (Mode Off): the model runs at native size on full frames (and on
// the centre sub-rect in mode 2), the machine fills the rest. Returns kNotHandled to fall back to
// the plain pass-through.
// ---------------------------------------------------------------------------------------------
struct NativeTemporal {
    FeatureState *st;
    ID3D12GraphicsCommandList *cmd;
    void *params;
    void *callback;
    ID3D12Resource *output;
    Subrect outputRect;
    bool motionRewritten;
    float mvScaleX, mvScaleY;
    pwtemporal::FrameInputs tin;
    bool full;
    volatile int stage;
    int result;
};

int NativeTemporalBody(NativeTemporal &n)
{
    FeatureState &st = *n.st;
    ID3D12GraphicsCommandList *cmd = n.cmd;
    void *params = n.params;
    const bool accumulate = !n.full || st.temporal->AccValid();
    if (accumulate) {
        n.stage = StageTemporalAccumulate;
        st.temporal->RecordAccumulate(cmd, n.tin);
    }
    if (!n.full) {
        n.stage = StageTemporalReproject;
        st.temporal->RecordReproject(cmd, n.tin, KeepOutputOnInterpolation() ? nullptr : n.output, kHostOutputState,
                                     n.outputRect.x, n.outputRect.y);
        n.stage = StageDone;
        n.result = kNgxSuccess;
        return kNgxSuccess;
    }
    n.stage = StageParamsWrite;
    if (st.temporal->AccValid() && !Ctx().temporal.debugSingleFrameMotion) {
        // The model's history is as old as the residual: give it the accumulated displacement.
        SetResource(params, "DLSSNR.MVec", st.temporal->Acc());
        SetFloat(params, "DLSSNR.MVecScaleX", 1.0f);
        SetFloat(params, "DLSSNR.MVecScaleY", 1.0f);
        n.motionRewritten = true;
    }
    n.stage = StageModel;
    TimingBegin(st, cmd);
    const int result = CallEvaluate(cmd, st.realHandle, params, n.callback);
    TimingEnd(st, cmd);
    Ctx().status.lastNgxResult = result;
    n.stage = StageParamsRestore;
    if (n.motionRewritten) {
        SetResource(params, "DLSSNR.MVec", n.tin.motion);
        SetFloat(params, "DLSSNR.MVecScaleX", n.mvScaleX);
        SetFloat(params, "DLSSNR.MVecScaleY", n.mvScaleY);
        n.motionRewritten = false;
    }
    n.result = result;
    if (result != kNgxSuccess) {
        char why[96];
        std::snprintf(why, sizeof(why), "model returned %d on a native temporal frame", result);
        n.stage = StageCopyOut;
        FallbackOutput(st, cmd, n.tin.color, Subrect{n.tin.colorRect.x, n.tin.colorRect.y, n.tin.colorRect.w, n.tin.colorRect.h}, n.output, n.outputRect, &n.tin, why);
        return result;
    }
    n.stage = StageTemporalResidual;
    pwtemporal::FrameInputs tinR = n.tin;
    tinR.residualBlend = Ctx().temporal.debugSingleFrameMotion ? 0.0f : kResidualBlend;
    st.temporal->RecordResidual(cmd, tinR, n.output, kHostOutputState);
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

void NativeTemporalRestore(NativeTemporal &n)
{
    __try {
        if (n.motionRewritten) {
            SetResource(n.params, "DLSSNR.MVec", n.tin.motion);
            SetFloat(n.params, "DLSSNR.MVecScaleX", n.mvScaleX);
            SetFloat(n.params, "DLSSNR.MVecScaleY", n.mvScaleY);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

int NativeTemporalEvaluate(FeatureState &st, ID3D12GraphicsCommandList *cmd, void *params, void *callback)
{
    ID3D12Resource *color = GetResource(params, "DLSSNR.Color");
    ID3D12Resource *depth = GetResource(params, "DLSSNR.Depth");
    ID3D12Resource *motion = GetResource(params, "DLSSNR.MVec");
    ID3D12Resource *output = GetResource(params, "DLSSNR.Output");
    if (color == nullptr || depth == nullptr || motion == nullptr || output == nullptr || cmd->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT)
        return kNotHandled;
    const D3D12_RESOURCE_DESC colorDesc = color->GetDesc();
    const D3D12_RESOURCE_DESC depthDesc = depth->GetDesc();
    const D3D12_RESOURCE_DESC motionDesc = motion->GetDesc();
    const D3D12_RESOURCE_DESC outputDesc = output->GetDesc();
    if (colorDesc.Width < st.nativeWidth || colorDesc.Height < st.nativeHeight || outputDesc.Width < st.nativeWidth ||
        outputDesc.Height < st.nativeHeight) {
        TemporalReason(st, "host colour/output smaller than the feature (%llux%u / %llux%u)", (unsigned long long) colorDesc.Width,
                       colorDesc.Height, (unsigned long long) outputDesc.Width, outputDesc.Height);
        return kNotHandled;
    }
    if (!EnsureTemporal(st, cmd, output, motion, depth)) return kNotHandled;
    unsigned int hostReset = 0;
    GetUInt(params, "DLSSNR.Reset", &hostReset);
    TemporalPlan plan = PlanTemporal(st, hostReset != 0);
    Ctx().status.temporalMode = plan.active ? Ctx().temporal.mode : 0;
    if (!plan.active) return kNotHandled;
    Ctx().status.temporalReason[0] = 0;

    NativeTemporal n{};
    n.st = &st;
    n.cmd = cmd;
    n.params = params;
    n.callback = callback;
    n.output = output;
    const Subrect colorRect = ReadSubrect(params, "Color", (unsigned) colorDesc.Width, colorDesc.Height);
    const Subrect depthRect = ReadSubrect(params, "Depth", (unsigned) depthDesc.Width, depthDesc.Height);
    const Subrect motionRect = ReadSubrect(params, "MVec", (unsigned) motionDesc.Width, motionDesc.Height);
    n.outputRect = ReadSubrect(params, "Output", (unsigned) outputDesc.Width, outputDesc.Height);
    unsigned int depthInverted = 0;
    GetUInt(params, "DLSSNR.DepthInverted", &depthInverted);
    float mvScaleX = 1.0f, mvScaleY = 1.0f;
    if (!GetFloat(params, "DLSSNR.MVecScaleX", &mvScaleX) || !std::isfinite(mvScaleX) || mvScaleX == 0.0f) mvScaleX = 1.0f;
    if (!GetFloat(params, "DLSSNR.MVecScaleY", &mvScaleY) || !std::isfinite(mvScaleY) || mvScaleY == 0.0f) mvScaleY = 1.0f;
    n.mvScaleX = mvScaleX;
    n.mvScaleY = mvScaleY;
    n.tin = TemporalInputs(color, motion, depth, TypedView(colorDesc.Format, false), TypedView(motionDesc.Format, false),
                           TypedView(depthDesc.Format, true), colorRect, motionRect, depthRect, mvScaleX, mvScaleY, depthInverted != 0);
    // 26.26: the depth guide's resting state and its barrier subresource, as everywhere else the machine
    // touches the host's depth. Without them a planar depth-stencil host (D24S8/D32S8) took whole-resource
    // barriers and copies in the native temporal path.
    n.tin.depthState = HostDepthState(depth);
    n.tin.depthSubresource = pwngx::DepthBarrierSubresource(depth);
    n.full = plan.full;
    const int result = NativeTemporalGuarded(n);
    if (result == kNgxSuccess && !plan.full) TemporalDebugReadback(st, cmd, n.tin);
    if (result == kCrashed) {
        NativeTemporalRestore(n);
        st.temporalDisabled = true;
        TemporalReason(st, "exception 0x%08lX in stage %s; temporal mode disabled for this feature", Ctx().crash.code, StageName(Ctx().crash.stage));
        Log(true, "Optimizer FPS NGX hook: exception 0x%08lX at %p (%s) during stage %d (%s); temporal mode disabled",
            Ctx().crash.code, Ctx().crash.address, Ctx().crash.module, Ctx().crash.stage, StageName(Ctx().crash.stage));
        return n.stage >= StageModel && n.result != 0 ? n.result : kNgxSuccess;
    }
    if (result == kNgxSuccess) {
        ++Ctx().status.evaluations;
        Ctx().status.active = true;
        SetReason("temporal mode %d (no warp)", Ctx().temporal.mode);
        TemporalFrameDone(st, plan.full);
    }
    return result;
}

} // namespace pwhook
