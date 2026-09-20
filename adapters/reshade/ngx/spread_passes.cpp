#include "spread_passes.h"

#include "feature_state.h"
#include "hook_context.h"
#include "host_depth_state.h"
#include "ngx_params.h"
#include "temporal_controller.h"

#include <algorithm>
#include <cstdio>

namespace pwhook {
SpreadState::~SpreadState()
{
    if (raw) raw->Release();
    if (carried) carried->Release();
}

bool SpreadRequested(const FeatureState &st)
{
    return !st.disabled && st.passesReady > 1 && Ctx().temporal.spreadPasses && Ctx().temporal.mode != 0;
}

void RetireSpread(FeatureState &st, const pwngx::GateSet &gate)
{
    if (!st.spread) return;
    pwngx::Grave g;
    g.gate = gate;
    g.disposables.push_back(std::move(st.spread));
    Ctx().graveyard.Add(std::move(g), Ctx().evalCounter);
}

static bool EnsureSpread(FeatureState &st, ID3D12Resource *output, ID3D12Resource *motion, ID3D12Resource *depth)
{
    const auto outputDesc = output->GetDesc(), motionDesc = motion->GetDesc(), depthDesc = depth->GetDesc();
    if (st.spread) {
        if (st.spread->hidden[0]->Matches(st.nativeWidth, st.nativeHeight, outputDesc.Format,
                static_cast<unsigned>(motionDesc.Width), motionDesc.Height, depthDesc.Format,
                static_cast<unsigned>(depthDesc.Width), depthDesc.Height)) return true;
        RetireSpread(st, pwngx::SignalGate(st.realDevice, st.device));
    }
    auto s = std::make_unique<SpreadState>();
    s->stages = st.passesReady;
    s->every = std::max(Ctx().temporal.every, s->stages);
    const auto od = output->GetDesc(), md = motion->GetDesc(), dd = depth->GetDesc();
    char error[256] = {};
    for (int i = 0; i < s->stages - 1; ++i) {
        s->hidden[i] = std::make_unique<pwtemporal::Machine>();
        if (!s->hidden[i]->Initialize(st.device, Ctx().shaders, st.nativeWidth, st.nativeHeight,
                                      od.Format, TypedView(od.Format, false), static_cast<unsigned>(md.Width), md.Height,
                                      dd.Format, static_cast<unsigned>(dd.Width), dd.Height, error, sizeof(error))) {
            std::snprintf(Ctx().status.modelPassReason, sizeof(Ctx().status.modelPassReason), "Cannot spread model passes: %.140s", error);
            return false;
        }
    }
    if (!CreateTexture(st.device, st.nativeWidth, st.nativeHeight, od.Format, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, &s->raw) ||
        !CreateTexture(st.device, st.nativeWidth, st.nativeHeight, od.Format, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, &s->carried)) {
        std::snprintf(Ctx().status.modelPassReason, sizeof(Ctx().status.modelPassReason), "Cannot spread model passes: carry texture allocation failed; passes stay in one frame");
        return false;
    }
    st.temporal->Invalidate();
    st.passReset[0] = st.passReset[1] = true;
    st.spread = std::move(s);
    Log(false, "Optimizer FPS NGX hook: spreading %d passes across frames, N=%d; completed cycle only (host queue)", st.passesReady, st.spread->every);
    return true;
}

static pwtemporal::FrameInputs Quiet(pwtemporal::FrameInputs in)
{
    in.phaseInFrames = 0;
    in.residualBlend = 0;
    in.debugVis = 0;
    return in;
}

int SpreadEvaluate(FeatureState &st, ID3D12GraphicsCommandList *cmd, void *params, void *callback)
{
    auto *color = GetResource(params, "DLSSNR.Color"), *output = GetResource(params, "DLSSNR.Output");
    auto *motion = GetResource(params, "DLSSNR.MVec"), *depth = GetResource(params, "DLSSNR.Depth");
    if (!color || !output || !motion || !depth || cmd->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT) return kNotHandled;
    if (st.warped && !EnsureGpu(st, cmd, color, output)) return kNotHandled;
    if (!EnsureTemporal(st, cmd, output, motion, depth) || !EnsureSpread(st, output, motion, depth)) return kNotHandled;
    // Switching from asynchronous unspread work must settle that job before this queue uses its models.
    if (st.async) {
        pwngx::GateSet gate;
        BuryAsync(st, &gate);
        if (!WaitForGpu(st.realDevice, st.device)) return kNotHandled;
    }
    SpreadState &s = *st.spread;
    const auto cd = color->GetDesc(), md = motion->GetDesc(), dd = depth->GetDesc(), od = output->GetDesc();
    const auto cr = ReadSubrect(params, "Color", static_cast<unsigned>(cd.Width), cd.Height);
    const auto mr = ReadSubrect(params, "MVec", static_cast<unsigned>(md.Width), md.Height);
    const auto dr = ReadSubrect(params, "Depth", static_cast<unsigned>(dd.Width), dd.Height);
    const auto out = ReadSubrect(params, "Output", static_cast<unsigned>(od.Width), od.Height);
    float sx = 1, sy = 1;
    unsigned int reset = 0, inverted = 0;
    GetFloat(params, "DLSSNR.MVecScaleX", &sx); GetFloat(params, "DLSSNR.MVecScaleY", &sy);
    GetUInt(params, "DLSSNR.Reset", &reset); GetUInt(params, "DLSSNR.DepthInverted", &inverted);
    auto in = TemporalInputs(color, motion, depth, TypedView(cd.Format, false), TypedView(md.Format, false), TypedView(dd.Format, true), cr, mr, dr, sx, sy, inverted != 0);
    in.depthState = HostDepthState(depth);
    in.depthSubresource = pwngx::DepthBarrierSubresource(depth);
    if (reset) {
        st.temporal->Invalidate();
        for (auto &hidden : s.hidden) if (hidden) hidden->Invalidate();
        s.position = 0;
        st.passReset[0] = st.passReset[1] = true;
    }
    Barrier(cmd, s.raw, s.rawState, kHostInputState);
    Barrier(cmd, s.carried, s.carriedState, kHostInputState);
    // A separate immutable raw snapshot also handles hosts with Color == Output and differing formats.
    st.temporal->RecordRaw(cmd, in, s.raw, kHostInputState);
    in.color = s.raw;
    in.colorView = TypedView(od.Format, false);
    in.colorRect = {0, 0, st.nativeWidth, st.nativeHeight};
    if (st.temporal->HasResidual()) st.temporal->RecordAccumulate(cmd, in);
    for (auto &hidden : s.hidden) if (hidden && hidden->HasResidual()) hidden->RecordAccumulate(cmd, Quiet(in));
    if (s.position >= s.every) s.position = 0;

    Ctx().status.temporalMode = 1; // spreading is the synchronous, one-evaluate-per-host-frame schedule
    if (Ctx().temporal.mode == 3)
        std::snprintf(Ctx().status.temporalReason, sizeof(Ctx().status.temporalReason), "Spread passes use the host queue, one model pass per frame; background queue is inactive");
    else Ctx().status.temporalReason[0] = 0;
    const bool runs = s.position < s.stages;
    const bool last = s.position == s.stages - 1;
    int result = kNgxSuccess;
    if (runs) {
        auto &stage = last ? *st.temporal : *s.hidden[s.position];
        auto *input = s.raw;
        if (s.position > 0) {
            s.hidden[s.position - 1]->RecordReproject(cmd, Quiet(in), s.carried, kHostInputState, 0, 0);
            input = s.carried;
        }
        if (s.frames < 8)
            Log(false, "Optimizer FPS NGX hook: spread schedule frame %llu: stage %d/%d, publish=%d",
                static_cast<unsigned long long>(Ctx().evalCounter), s.position + 1, s.stages, last ? 1 : 0);
        st.activeModelStage = s.position;
        result = SpreadModel(st, cmd, params, callback, in, input, stage);
        st.activeModelStage = -1;
        if (result == kNgxSuccess) {
            auto residual = last ? in : Quiet(in);
            residual.residualBlend = last ? kResidualBlend : 0.0f;
            stage.RecordResidual(cmd, residual, output, kHostOutputState);
            if (last) stage.RecordApply(cmd, residual, output, kHostOutputState, out.x, out.y);
        }
    }
    if (!last || !runs || result != kNgxSuccess) {
        if (st.temporal->HasResidual()) st.temporal->RecordReproject(cmd, in, output, kHostOutputState, out.x, out.y);
        else st.temporal->RecordRaw(cmd, in, output, kHostOutputState, out.x, out.y);
    }
    if (result != kNgxSuccess) {
        std::snprintf(Ctx().status.modelPassReason, sizeof(Ctx().status.modelPassReason), "Spread stage %d failed (0x%X); cycle restarted", s.position + 1, result);
        Log(true, "Optimizer FPS NGX hook: %s", Ctx().status.modelPassReason);
        s.position = 0;
    } else ++s.position;
    ++s.frames;
    TemporalFrameDone(st, runs && last && result == kNgxSuccess);
    if (!runs || !st.warped) ++Ctx().status.evaluations;
    Ctx().status.active = true;
    Ctx().status.modelPassesRunning = s.stages;
    return kNgxSuccess;
}
} // namespace pwhook
