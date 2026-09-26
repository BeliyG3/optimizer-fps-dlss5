#include "core/temporal/controller.h"

#include "core/context.h"
#include "core/gpu/queues.h"

#include <atomic>
#include <cstdio>

// Which temporal mode a frame actually runs: the background mode (3) falls back to the synchronous
// one (1) where it cannot work.
namespace ofps::core {

// 26.24: the background mode needs the host's command queue registered (it signals the queue after the host
// submits our copies and waits on it). Hosts whose D3D12 device was created before ReShade loaded (OptiScaler
// in Fallen Order) register no queue: every pass then ran into the forced wait at the age limit, 0.3 passes/s
// and a stall on each - "async does not start, everything stutters". Without a queue the synchronous
// interpolation runs instead, and the tab/log say so.
bool BackgroundModeUsable(FeatureState &st)
{
    if (Ctx().temporal.mode != 3) return false;
    std::size_t total = 0;
    const bool ok = ofps::core::gpu::CountQueues(st.realDevice, st.device, &total) > 0;
    static bool s_said = false;
    if (!ok && !s_said) {
        s_said = true;
        Log(true, "Optimizer FPS NGX hook: background mode needs the host's D3D12 queue and none is "
                  "registered in this process (device created before ReShade loaded); the synchronous "
                  "interpolation runs instead");
        TemporalReason(st, "background mode unavailable here (no registered host queue); running the synchronous interpolation");
    }
    return ok;
}

int EffectiveTemporalMode(FeatureState &st) {
    // The model grid host still carries frames on the feature grid. Its background
    // codec resources need a separate private-list protocol; run mode 3 synchronously.
    if (st.modelResolution && Ctx().temporal.mode == 3) {
        std::snprintf(Ctx().status.temporalReason, sizeof(Ctx().status.temporalReason),
                      "background mode needs a private-list model-grid codec; using synchronous mode 1");
        static std::atomic<bool> s_modelGridReported{false};
        if (!s_modelGridReported.exchange(true)) {
            Log(true, "Optimizer FPS NGX hook: %s", Ctx().status.temporalReason);
        }
        return 1;
    }
    // A host that evaluates on a COMPUTE list (DLSS5-Reshade-AIO) already runs the model off the
    // graphics queue; the background pass copies inputs on a direct list, so mode 3 runs as mode 1.
    if (st.hostListCompute && Ctx().temporal.mode == 3) {
        std::snprintf(Ctx().status.temporalReason, sizeof(Ctx().status.temporalReason),
                      "the host evaluates on a compute list; background mode runs as the synchronous one");
        static std::atomic<bool> s_computeReported{false};
        if (!s_computeReported.exchange(true)) Log(true, "Optimizer FPS NGX hook: %s", Ctx().status.temporalReason);
        return 1;
    }
    return (Ctx().temporal.mode == 3 && !BackgroundModeUsable(st)) ? 1 : Ctx().temporal.mode;
}

} // namespace ofps::core
