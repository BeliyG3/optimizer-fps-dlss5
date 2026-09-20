#include "model_passes.h"

#include "feature_state.h"
#include "hook_context.h"
#include "host_depth_state.h"
#include "ngx_params.h"
#include "temporal_controller.h"

#include <algorithm>
#include <cstdio>

namespace pwhook {

void RetireModelPasses(FeatureState &st, const pwngx::GateSet &gate)
{
    for (void *&handle : st.passHandles) {
        BuryReal(handle, gate);
        handle = nullptr;
    }
    RetireSpread(st, gate);
    st.passesRequested = st.passesReady = 1;
    st.passReset[0] = st.passReset[1] = true;
}

bool PrepareModelPasses(FeatureState &st, ID3D12GraphicsCommandList *cmd, void *params)
{
    const int requested = Ctx().temporal.modelPasses;
    if (st.passesRequested == requested) return false;
    // Settle the background job before changing its handles or staging storage.
    pwngx::GateSet gate;
    BuryAsync(st, &gate);
    if (gate.Empty()) gate = pwngx::SignalGate(st.realDevice, st.device);
    if (requested < st.passesRequested) {
        for (int i = requested - 1; i < 2; ++i) {
            BuryReal(st.passHandles[i], gate);
            st.passHandles[i] = nullptr;
            st.passReset[i] = true;
        }
        st.passesReady = std::min(st.passesReady, requested);
    }
    RetireSpread(st, gate);
    if (st.temporal) st.temporal->Invalidate();
    st.passesRequested = requested;
    Ctx().status.modelPassReason[0] = 0;
    if (requested == 1) { Ctx().status.modelPassesRunning = 1; return false; }

    auto *output = GetResource(params, "DLSSNR.Output");
    auto *color = GetResource(params, "DLSSNR.Color");
    auto *motion = GetResource(params, "DLSSNR.MVec");
    auto *depth = GetResource(params, "DLSSNR.Depth");
    if (!output || !color || !motion || !depth || !EnsureTemporal(st, cmd, output, motion, depth)) {
        std::snprintf(Ctx().status.modelPassReason, sizeof(Ctx().status.modelPassReason), "Extra model passes unavailable: creation-frame carry resources could not be allocated");
        return false;
    }
    bool created = false;
    WriteSizes(st.params, st.createdWidth, st.createdHeight);
    unsigned int ui = 0;
    const bool hadUi = GetUInt(st.params, "DLSSNR.UICorrection", &ui);
    if (hadUi && st.warped) SetUInt(st.params, "DLSSNR.UICorrection", 0);
    for (int i = 0; i < requested - 1; ++i) {
        if (st.passHandles[i]) continue;
        const int result = CallCreate(cmd, kFeatureNeuralRendering, st.params, &st.passHandles[i]);
        created = true; // even a failed creation may have recorded GPU work
        if (result != kNgxSuccess || !st.passHandles[i]) {
            if (st.passHandles[i]) BuryReal(st.passHandles[i]);
            st.passHandles[i] = nullptr;
            std::snprintf(Ctx().status.modelPassReason, sizeof(Ctx().status.modelPassReason),
                          "Pass %d could not be created (NGX 0x%X); running %d / %d passes. Check VRAM budget.", i + 2, result, i + 1, requested);
            Log(true, "Optimizer FPS NGX hook: %s", Ctx().status.modelPassReason);
            break;
        }
        st.passReset[i] = true;
        st.passesReady = i + 2;
        Log(false, "Optimizer FPS NGX hook: model pass %d real feature %p created at %ux%u (evaluate-free frame)", i + 2, st.passHandles[i], st.createdWidth, st.createdHeight);
    }
    WriteSizes(st.params, st.nativeWidth, st.nativeHeight);
    if (hadUi) SetUInt(st.params, "DLSSNR.UICorrection", ui);
    Ctx().status.modelPassesRunning = st.passesReady;
    if (!created) return false;

    const auto cd = color->GetDesc(), md = motion->GetDesc(), dd = depth->GetDesc(), od = output->GetDesc();
    const auto cr = ReadSubrect(params, "Color", static_cast<unsigned>(cd.Width), cd.Height);
    const auto mr = ReadSubrect(params, "MVec", static_cast<unsigned>(md.Width), md.Height);
    const auto dr = ReadSubrect(params, "Depth", static_cast<unsigned>(dd.Width), dd.Height);
    const auto out = ReadSubrect(params, "Output", static_cast<unsigned>(od.Width), od.Height);
    float sx = 1.0f, sy = 1.0f;
    unsigned int inverted = 0;
    GetFloat(params, "DLSSNR.MVecScaleX", &sx); GetFloat(params, "DLSSNR.MVecScaleY", &sy);
    GetUInt(params, "DLSSNR.DepthInverted", &inverted);
    auto in = TemporalInputs(color, motion, depth, TypedView(cd.Format, false), TypedView(md.Format, false), TypedView(dd.Format, true), cr, mr, dr, sx, sy, inverted != 0);
    in.depthState = HostDepthState(depth);
    in.depthSubresource = pwngx::DepthBarrierSubresource(depth);
    if (st.temporal->HasResidual()) st.temporal->RecordAccumulate(cmd, in);
    else in.debugVis = 3; // initialization only: there is no completed cycle yet
    st.temporal->RecordReproject(cmd, in, output, kHostOutputState, out.x, out.y);
    Log(false, "Optimizer FPS NGX hook: model passes setup frame, no model evaluate recorded");
    return true;
}

int EvaluateModelPasses(FeatureState &st, ID3D12GraphicsCommandList *cmd, void *params, void *callback)
{
    if (st.passesReady == 1 && st.activeModelStage < 0) return CallEvaluate(cmd, st.realHandle, params, callback);
    if (st.activeModelStage >= 0) {
        const int stage = st.activeModelStage;
        unsigned int reset = 0; GetUInt(params, "DLSSNR.Reset", &reset);
        if ((stage > 0 && st.passReset[stage - 1]) || (stage == 0 && st.spread && st.spread->frames == 0))
            SetUInt(params, "DLSSNR.Reset", 1);
        const int r = CallEvaluate(cmd, stage == 0 ? st.realHandle : st.passHandles[stage - 1], params, callback);
        SetUInt(params, "DLSSNR.Reset", reset);
        if (r == kNgxSuccess) {
            if (stage > 0) st.passReset[stage - 1] = false;
            ++Ctx().status.modelPassEvaluations[stage];
            if (Ctx().status.modelPassEvaluations[stage] <= 4 || Ctx().evalCounter % 60 == 0)
                Log(false, "Optimizer FPS NGX hook: spread frame %llu model stage %d/%d evaluated (one evaluate this frame)",
                    static_cast<unsigned long long>(Ctx().evalCounter), stage + 1, st.passesReady);
        }
        return r;
    }
    int result = CallEvaluate(cmd, st.realHandle, params, callback);
    if (result != kNgxSuccess) return result;
    ++Ctx().status.modelPassEvaluations[0];
    if (st.passesReady <= 1) return result;
    auto *output = GetResource(params, "DLSSNR.Output");
    auto *color = GetResource(params, "DLSSNR.Color");
    const auto desc = output->GetDesc();
    if (!st.passStaging) {
        if (!CreateTexture(st.device, static_cast<unsigned>(desc.Width), desc.Height, desc.Format,
                           D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, &st.passStaging)) {
            std::snprintf(Ctx().status.modelPassReason, sizeof(Ctx().status.modelPassReason), "Extra pass staging allocation failed; running 1 / %d passes", st.passesRequested);
            Ctx().status.modelPassesRunning = 1;
            return result;
        }
    }
    const auto rect = ReadSubrect(params, "Color", st.createdWidth, st.createdHeight);
    const auto outRect = ReadSubrect(params, "Output", st.createdWidth, st.createdHeight);
    unsigned int reset = 0; GetUInt(params, "DLSSNR.Reset", &reset);
    int running = 1;
    for (int i = 0; i < st.passesReady - 1; ++i) {
        BarrierExternal(cmd, output, kHostOutputState, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Barrier(cmd, st.passStaging, st.passStagingState, D3D12_RESOURCE_STATE_COPY_DEST);
        cmd->CopyResource(st.passStaging, output);
        Barrier(cmd, st.passStaging, st.passStagingState, kHostInputState);
        BarrierExternal(cmd, output, D3D12_RESOURCE_STATE_COPY_SOURCE, kHostOutputState);
        SetResource(params, "DLSSNR.Color", st.passStaging);
        WriteSubrect(params, "Color", outRect.x, outRect.y, outRect.w, outRect.h);
        SetUInt(params, "DLSSNR.Reset", reset || st.passReset[i] ? 1u : 0u);
        result = CallEvaluate(cmd, st.passHandles[i], params, callback);
        if (result != kNgxSuccess) {
            // Preserve the last successful answer even if NGX wrote before reporting failure.
            Barrier(cmd, st.passStaging, st.passStagingState, D3D12_RESOURCE_STATE_COPY_SOURCE);
            BarrierExternal(cmd, output, kHostOutputState, D3D12_RESOURCE_STATE_COPY_DEST);
            cmd->CopyResource(output, st.passStaging);
            BarrierExternal(cmd, output, D3D12_RESOURCE_STATE_COPY_DEST, kHostOutputState);
            std::snprintf(Ctx().status.modelPassReason, sizeof(Ctx().status.modelPassReason), "Pass %d evaluate failed (0x%X); showing pass %d", i + 2, result, running);
            Log(true, "Optimizer FPS NGX hook: %s", Ctx().status.modelPassReason);
            break;
        }
        st.passReset[i] = false;
        ++running;
        if (++Ctx().status.modelPassEvaluations[i + 1] == 1)
            Log(false, "Optimizer FPS NGX hook: model pass %d evaluated successfully, independent feature %p", i + 2, st.passHandles[i]);
    }
    SetResource(params, "DLSSNR.Color", color);
    WriteSubrect(params, "Color", rect.x, rect.y, rect.w, rect.h);
    SetUInt(params, "DLSSNR.Reset", reset);
    Ctx().status.modelPassesRunning = running;
    return kNgxSuccess;
}
} // namespace pwhook
