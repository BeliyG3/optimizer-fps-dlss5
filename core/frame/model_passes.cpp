#include "core/frame/callback_guard.h"
#include "core/frame/model_passes.h"
#include "core/frame/frame_inputs.h"

#include "core/frame/feature_state.h"
#include "core/context.h"
#include "core/frame/host_depth_state.h"
#include "core/frame/lifecycle.h"
#include "core/temporal/controller.h"
#include "core/frame/warp_recorder.h"

#include <algorithm>
#include <cstdio>

namespace ofps::core {

void RetireModelPasses(FeatureState &st, const ofps::core::gpu::GateSet &gate)
{
    const auto retirement = RetirementGate(st, gate);
    for (void *&handle : st.passHandles) {
        BuryReal(st.modelHost, handle, retirement);
        handle = nullptr;
    }
    RetireSpread(st, gate);
    st.passesRequested = st.passesReady = 1;
    st.passReset[0] = st.passReset[1] = true;
    st.passPending[0] = st.passPending[1] = false;
}

bool PrepareModelPasses(FeatureState &st, ID3D12GraphicsCommandList *cmd, const OfpsModelInputs &inputs)
{
    const int requested = st.modelResolution ? 1 : Ctx().temporal.modelPasses;
    if (st.passesRequested == requested) return false;
    // Settle the background job before changing its handles or staging storage.
    ofps::core::gpu::GateSet gate;
    BuryAsync(st, &gate);
    gate = RetirementGate(st, gate);
    if (requested < st.passesRequested) {
        for (int i = requested - 1; i < 2; ++i) {
            BuryReal(st.modelHost, st.passHandles[i], gate);
            st.passHandles[i] = nullptr;
            st.passPending[i] = false;
            st.passReset[i] = true;
        }
        st.passesReady = std::min(st.passesReady, requested);
    }
    RetireSpread(st, gate);
    if (st.temporal) st.temporal->Invalidate();
    st.passesRequested = requested;
    Ctx().status.modelPassReason[0] = 0;
    if (requested == 1) { Ctx().status.modelPassesRunning = 1; return false; }

    auto *output = inputs.output.res;
    auto *color = inputs.color.res;
    auto *motion = inputs.motion.res;
    auto *depth = inputs.depth.res;
    if (!output || !color || !motion || !depth || !EnsureTemporal(st, cmd, output, motion, depth)) {
        std::snprintf(Ctx().status.modelPassReason, sizeof(Ctx().status.modelPassReason), "Extra model passes unavailable: creation-frame carry resources could not be allocated");
        return false;
    }
    bool created = false;
    for (int i = 0; i < requested - 1; ++i) {
        if (st.passHandles[i]) continue;
        const int result = CreateModelGuarded(st, cmd, st.createdWidth, st.createdHeight,
            st.modelResolution ? st.withholdUi : st.warped, &st.passHandles[i]);
        created = true; // even a failed creation may have recorded GPU work
        if ((result != OFPS_OK && result != OFPS_S_MODEL_NEXT_FRAME) || !st.passHandles[i]) {
            if (st.passHandles[i]) BuryReal(st.modelHost, st.passHandles[i], RetirementGate(st));
            st.passHandles[i] = nullptr;
            st.passPending[i] = false;
            std::snprintf(Ctx().status.modelPassReason, sizeof(Ctx().status.modelPassReason),
                          "Pass %d could not be created (NGX 0x%X); running %d / %d passes. Check VRAM budget.", i + 2, result, i + 1, requested);
            Log(true, "Optimizer FPS NGX hook: %s", Ctx().status.modelPassReason);
            break;
        }
        st.passReset[i] = true;
        st.passPending[i] = result == OFPS_S_MODEL_NEXT_FRAME;
        st.passesReady = i + 2;
        Log(false, "Optimizer FPS NGX hook: model pass %d real feature %p created at %ux%u (evaluate-free frame)", i + 2, st.passHandles[i], st.createdWidth, st.createdHeight);
    }
    Ctx().status.modelPassesRunning = st.passesReady;
    if (created) Log(false, "Optimizer FPS NGX hook: model passes setup frame, no model evaluate recorded");
    return created;
}

static int RunPass(FeatureState &st, ID3D12GraphicsCommandList *cmd, void *handle, const OfpsModelInputs &inputs)
{
    OfpsModelInputs in = inputs;
    if (in.withholdUi) in.ui.res = in.uiAlpha.res = in.backbuffer.res = nullptr;
    st.frameModelCalled = true;
    st.frameModelResult = GuardCallback([&] { return st.modelHost->RunModel(cmd, handle, &in); });
    return st.frameModelResult;
}

int EvaluateModelPasses(FeatureState &st, ID3D12GraphicsCommandList *cmd, const OfpsModelInputs &inputs)
{
    OfpsModelInputs in = inputs;
    in.reset = in.reset || st.forcedReset;
    if (st.passesReady == 1 && st.activeModelStage < 0) return RunPass(st, cmd, st.realHandle, in);
    if (st.activeModelStage >= 0) {
        const int stage = st.activeModelStage;

        if ((stage > 0 && (st.passReset[stage - 1] || Ctx().diag.passNoHistory)) ||
            (stage == 0 && st.spread && st.spread->frames == 0))
            in.reset = 1;
        const int r = RunPass(st, cmd, stage == 0 ? st.realHandle : st.passHandles[stage - 1], in);
        if (r == OFPS_OK) {
            if (stage > 0) st.passReset[stage - 1] = false;
            ++Ctx().status.modelPassEvaluations[stage];
            if (Ctx().status.modelPassEvaluations[stage] <= 4 || Ctx().evalCounter % 60 == 0)
                Log(false, "Optimizer FPS NGX hook: spread frame %llu model stage %d/%d evaluated (one evaluate this frame)",
                    static_cast<unsigned long long>(Ctx().evalCounter), stage + 1, st.passesReady);
        }
        return r;
    }
    int result = RunPass(st, cmd, st.realHandle, in);
    if (result != OFPS_OK) return result;
    ++Ctx().status.modelPassEvaluations[0];
    if (st.passesReady <= 1) return result;
    auto *output = in.output.res;
    const auto desc = output->GetDesc();
    if (!st.passStaging) {
        if (!CreateTexture(st.device, static_cast<unsigned>(desc.Width), desc.Height, desc.Format,
                           D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, &st.passStaging)) {
            std::snprintf(Ctx().status.modelPassReason, sizeof(Ctx().status.modelPassReason), "Extra pass staging allocation failed; running 1 / %d passes", st.passesRequested);
            Ctx().status.modelPassesRunning = 1;
            return result;
        }
    }
    int running = 1;
    for (int i = 0; i < st.passesReady - 1; ++i) {
        BarrierExternal(cmd, output, inputs.output.restState, D3D12_RESOURCE_STATE_COPY_SOURCE, inputs.output.subresource);
        Barrier(cmd, st.passStaging, st.passStagingState, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION hostCopy{}, stagingCopy{};
        hostCopy.pResource = output; hostCopy.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        hostCopy.SubresourceIndex = CopySubresource(inputs.output);
        stagingCopy.pResource = st.passStaging; stagingCopy.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        cmd->CopyTextureRegion(&stagingCopy, 0, 0, 0, &hostCopy, nullptr);
        Barrier(cmd, st.passStaging, st.passStagingState, kModelInputState);
        BarrierExternal(cmd, output, D3D12_RESOURCE_STATE_COPY_SOURCE, inputs.output.restState, inputs.output.subresource);
        in = inputs;
        in.color = in.output;
        in.color.res = st.passStaging;
        in.color.restState = kModelInputState;
        in.color.subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        in.reset = inputs.reset || st.forcedReset || st.passReset[i] || Ctx().diag.passNoHistory;
        result = RunPass(st, cmd, st.passHandles[i], in);
        if (result != OFPS_OK) {
            // Preserve the last successful answer even if NGX wrote before reporting failure.
            Barrier(cmd, st.passStaging, st.passStagingState, D3D12_RESOURCE_STATE_COPY_SOURCE);
            BarrierExternal(cmd, output, inputs.output.restState, D3D12_RESOURCE_STATE_COPY_DEST, inputs.output.subresource);
            cmd->CopyTextureRegion(&hostCopy, 0, 0, 0, &stagingCopy, nullptr);
            BarrierExternal(cmd, output, D3D12_RESOURCE_STATE_COPY_DEST, inputs.output.restState, inputs.output.subresource);
            std::snprintf(Ctx().status.modelPassReason, sizeof(Ctx().status.modelPassReason), "Pass %d evaluate failed (0x%X); showing pass %d", i + 2, result, running);
            Log(true, "Optimizer FPS NGX hook: %s", Ctx().status.modelPassReason);
            break;
        }
        st.passReset[i] = false;
        ++running;
        if (++Ctx().status.modelPassEvaluations[i + 1] == 1)
            Log(false, "Optimizer FPS NGX hook: model pass %d evaluated successfully, independent feature %p", i + 2, st.passHandles[i]);
    }
    Ctx().status.modelPassesRunning = running;
    return result;
}
} // namespace ofps::core
