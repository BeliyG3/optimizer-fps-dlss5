#include "core/gpu/host_list.h"
#include "core/frame/spread_passes.h"
#include "core/temporal/diagnostics.h"

#include "core/frame/feature_state.h"
#include "core/frame/frame_inputs.h"
#include "core/context.h"
#include "core/frame/host_depth_state.h"
#include "core/temporal/controller.h"
#include "core/frame/warp_recorder.h"

#include <algorithm>
#include <cstdio>

namespace ofps::core {
SpreadState::~SpreadState()
{
    if (raw) raw->Release();
    if (carried) carried->Release();
}

bool SpreadRequested(const FeatureState &st)
{
    // Its carry textures and residuals live on the feature's grid: a host frame that does not fit it
    // (host_shape.h) never reaches the spread schedule, which would otherwise write past its output region.
    return !st.modelResolution && st.hostFits && !st.disabled && st.passesReady > 1 &&
           Ctx().temporal.spreadPasses && Ctx().temporal.mode != 0;
}

void RetireSpread(FeatureState &st, const ofps::core::gpu::GateSet &gate)
{
    if (!st.spread) return;
    ofps::core::gpu::Grave g;
    g.modelHost = st.modelHost;
        g.gate = RetirementGate(st, gate);
    g.disposables.push_back(std::move(st.spread));
    Ctx().graveyard.Add(std::move(g), Ctx().evalCounter);
}

static bool EnsureSpread(FeatureState &st, ID3D12Resource *output, ID3D12Resource *motion, ID3D12Resource *depth)
{
    const auto outputDesc = output->GetDesc(), motionDesc = motion->GetDesc(), depthDesc = depth->GetDesc();
    const auto pictureDivisor = static_cast<std::uint32_t>(ofps::temporal::Value(
        Ctx().temporalDiagnostics, ofps::temporal::DiagnosticId::HistoryDiv, 3.0f));
    if (st.spread) {
        if (st.spread->hidden[0]->Matches(st.nativeWidth, st.nativeHeight, outputDesc.Format,
                static_cast<unsigned>(motionDesc.Width), motionDesc.Height, depthDesc.Format,
                static_cast<unsigned>(depthDesc.Width), depthDesc.Height, pictureDivisor,
                ofps::core::temporal::MachineRole::Hidden)) return true;
        RetireSpread(st, ofps::core::gpu::SignalGate(st.realDevice, st.device));
    }
    auto s = std::make_unique<SpreadState>();
    s->stages = st.passesReady;
    s->every = std::max(Ctx().temporal.every, s->stages);
    const auto od = output->GetDesc(), md = motion->GetDesc(), dd = depth->GetDesc();
    char error[256] = {};
    // The hidden stages are never shown and record their residual without a blend or a phase-in (Quiet):
    // a Hidden machine keeps one residual and no history (machine_role.h).
    for (int i = 0; i < s->stages - 1; ++i) {
        s->hidden[i] = std::make_unique<ofps::core::temporal::Machine>();
        if (!s->hidden[i]->Initialize(st.device, Ctx().shaders, st.nativeWidth, st.nativeHeight,
                                      od.Format, TypedView(od.Format, false), static_cast<unsigned>(md.Width), md.Height,
                                      dd.Format, static_cast<unsigned>(dd.Width), dd.Height, error, sizeof(error), pictureDivisor,
                                      ofps::core::temporal::MachineRole::Hidden)) {
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
    st.spread = std::move(s);
    Log(false, "Optimizer FPS NGX hook: spreading %d passes across frames, N=%d; completed cycle only (host queue)", st.passesReady, st.spread->every);
    return true;
}

static ofps::core::temporal::FrameInputs Quiet(ofps::core::temporal::FrameInputs in)
{
    in.phaseInFrames = 0;
    in.residualBlend = 0;
    in.debugVis = 0;
    in.carryChainRule = 0; // a hidden stage's carried frame is the next stage's model input: the main chain only
    return in;
}

// Drops every stage's chain and restarts the cycle (a host reset, or a declined detail transfer). Each stage's NR
// feature then meets motion that no longer reaches back to its last evaluate and resets (model_passes.h).
static void ResetSpreadHistory(FeatureState &st, SpreadState &s)
{
    st.temporal->Invalidate();
    for (auto &hidden : s.hidden) if (hidden) hidden->Invalidate();
    s.position = 0;
}

// The raw snapshot's route, logged when it changes: a plain copy, or the format-converting reprojection. A frame
// whose descriptors ran out wrote no snapshot and changes nothing.
static void NoteRawSnapshot(SpreadState &s, ofps::core::temporal::RawRoute route, const OfpsModelInputs &inputs, DXGI_FORMAT outputFormat)
{
    using ofps::core::temporal::RawRoute;
    if (route == RawRoute::None) return;
    const bool copied = route == RawRoute::Copy;
    if (s.rawCopy == (copied ? 1 : 0)) return;
    s.rawCopy = copied ? 1 : 0;
    if (copied) {
        Log(false, "Optimizer FPS NGX hook: spread raw snapshot: plain copy of the colour");
        return;
    }
    Log(false, "Optimizer FPS NGX hook: spread raw snapshot: format-converting reprojection (colour fmt %d view %d, output fmt %d, ratio %.3f)",
        static_cast<int>(inputs.color.res->GetDesc().Format), static_cast<int>(inputs.color.view), static_cast<int>(outputFormat),
        static_cast<double>(Ctx().frameProfile.ratioScale));
}

int SpreadEvaluate(FeatureState &st, ID3D12GraphicsCommandList *cmd, const OfpsModelInputs &inputs, const OfpsFrameInputs &frame)
{
    auto *color = inputs.color.res, *output = inputs.output.res;
    auto *motion = inputs.motion.res, *depth = inputs.depth.res;
    if (!color || !output || !motion || !depth || !gpu::HostListRecordable(cmd)) return kNotHandled;
    if (st.warped && !EnsureGpu(st, cmd, color, output, inputs.color.view, inputs.output.view, CopySubresource(inputs.output)))
        return kNotHandled;
    if (!EnsureTemporal(st, cmd, output, motion, depth) || !EnsureSpread(st, output, motion, depth)) return kNotHandled;
    // Switching from asynchronous unspread work must settle that job before this queue uses its models.
    if (st.async) {
        ofps::core::gpu::GateSet gate;
        BuryAsync(st, &gate);
        if (!WaitForGpu(st.realDevice, st.device)) return kNotHandled;
    }
    SpreadState &s = *st.spread;
    // No pass timing on the hidden stages: only the shown machine's reaches the status (core_feature.cpp).
    for (auto &hidden : s.hidden) if (hidden) hidden->SetUsePoint(HostUsePoint(cmd), Ctx().evalCounter, false);
    const auto od = output->GetDesc();
    const OfpsRect out{inputs.output.rect.x, inputs.output.rect.y, inputs.output.rect.w, inputs.output.rect.h};
    const float sx = inputs.mvScaleX, sy = inputs.mvScaleY;
    const bool reset = inputs.reset != 0 || st.forcedReset;
    const bool inverted = inputs.depthInverted != 0;
    auto in = TemporalInputs(inputs.color, inputs.motion, inputs.depth, sx, sy, inverted != 0, st.modelResolution,
                             ofps::core::temporal::kModeNoProbe); // the probe stays off the spread stages (one cadence per machine)
    auto fallbackInputs = in;
    fallbackInputs.mvScaleX = fallbackInputs.mvScaleY = 1.0f;
    fallbackInputs.depthInverted = false;
    if (reset) ResetSpreadHistory(st, s);
    Barrier(cmd, s.raw, s.rawState, kModelInputState);
    Barrier(cmd, s.carried, s.carriedState, kModelInputState);
    // A separate immutable raw snapshot also handles hosts with Color == Output and differing formats. A plain
    // copy where that writes the same bytes as the converting pass, NaN payloads aside (raw_snapshot.h).
    NoteRawSnapshot(s, st.temporal->RecordRaw(cmd, in, s.raw, kModelInputState), inputs, od.Format);
    in.color = s.raw;
    in.hostInputState = kModelInputState;
    in.colorSubresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    in.colorView = TypedView(od.Format, false);
    in.colorRect = {0, 0, st.nativeWidth, st.nativeHeight};
    if (s.position >= s.every) s.position = 0;
    // The shown machine's own full pass records no carry chain (FullPassInputs): its residual restarts both chains.
    const bool shownFull = s.position == s.stages - 1;
    if (st.temporal->HasResidual())
        st.temporal->RecordAccumulate(cmd, shownFull ? ofps::core::temporal::FullPassInputs(in) : in);
    for (auto &hidden : s.hidden) if (hidden && hidden->HasResidual()) hidden->RecordAccumulate(cmd, Quiet(in));

    Ctx().status.temporalMode = 1; // spreading is the synchronous, one-evaluate-per-host-frame schedule
    if (Ctx().temporal.mode == 3)
        std::snprintf(Ctx().status.temporalReason, sizeof(Ctx().status.temporalReason), "Spread passes use the host queue, one model pass per frame; background queue is inactive");
    else Ctx().status.temporalReason[0] = 0;
    const bool runs = s.position < s.stages;
    const bool last = s.position == s.stages - 1;
    int result = OFPS_OK;
    bool declined = false; // the stage's expected detail transfer was declined
    if (runs) {
        auto &stage = last ? *st.temporal : *s.hidden[s.position];
        auto *input = s.raw;
        if (s.position > 0) {
            s.hidden[s.position - 1]->RecordReproject(cmd, Quiet(in), s.carried, kModelInputState, 0, 0);
            input = s.carried;
        }
        if (s.frames < 8)
            Log(false, "Optimizer FPS NGX hook: spread schedule frame %llu: stage %d/%d, publish=%d",
                static_cast<unsigned long long>(Ctx().evalCounter), s.position + 1, s.stages, last ? 1 : 0);
        st.activeModelStage = s.position;
        result = SpreadModel(st, cmd, inputs, frame, in, input, stage, &declined);
        st.activeModelStage = -1;
        if (result == OFPS_S_MODEL_NEXT_FRAME) {
            s.position = 0;
            FallbackOutput(st, cmd, frame.color, frame.output, &fallbackInputs, "spread model creation frame");
            return result;
        }
        if (result == kPackFailed) {
            s.position = 0;
            FallbackOutput(st, cmd, frame.color, frame.output, &fallbackInputs, "spread pack stage unavailable");
            return OFPS_OK;
        }
        // A declined transfer leaves this stage's plain, stretched unpack: no residual from it, every
        // stage's history dropped and the cycle restarted; the frame goes out as the raw colour below.
        if (declined) ResetSpreadHistory(st, s);
        if (result == OFPS_OK && !declined) {
            auto residual = last ? in : Quiet(in);
            residual.residualBlend = last ? kResidualBlend : 0.0f;
            stage.RecordResidual(cmd, residual, output, inputs.output.restState, inputs.output.subresource);
            if (last) stage.RecordApply(cmd, residual, output, inputs.output.restState, out.x, out.y, inputs.output.subresource);
        }
    }
    if (!last || !runs || result != OFPS_OK || declined) {
        if (st.temporal->HasResidual()) st.temporal->RecordReproject(cmd, in, output, inputs.output.restState, out.x, out.y, inputs.output.subresource);
        else st.temporal->RecordRaw(cmd, in, output, inputs.output.restState, out.x, out.y, inputs.output.subresource);
    }
    if (TemporalExhausted(st)) {
        FallbackOutput(st, cmd, frame.color, frame.output, &fallbackInputs, "spread descriptor pool exhausted");
        return OFPS_OK;
    }
    if (result != OFPS_OK) {
        std::snprintf(Ctx().status.modelPassReason, sizeof(Ctx().status.modelPassReason), "Spread stage %d failed (0x%X); cycle restarted", s.position + 1, result);
        Log(true, "Optimizer FPS NGX hook: %s", Ctx().status.modelPassReason);
        s.position = 0;
    } else if (!declined) ++s.position;
    ++s.frames;
    TemporalFrameDone(st, runs && last && result == OFPS_OK && !declined);
    if (!runs || !st.warped) ++Ctx().status.evaluations;
    Ctx().status.active = true;
    Ctx().status.modelPassesRunning = s.stages;
    return OFPS_OK;
}
} // namespace ofps::core
