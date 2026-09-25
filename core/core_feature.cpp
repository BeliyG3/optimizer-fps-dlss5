#include "core/core_impl.h"
#include "core/frame/dispatch.h"
#include "core/frame/frame_inputs.h"
#include "core/frame/model_passes.h"
#include "core/frame/spread_passes.h"
#include "core/temporal/diagnostics.h"
#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace ofps::core {
Feature::Feature(Core &core, FeatureState *state, void *handle, bool adopted)
    : core_(core), state_(state), hostHandle_(handle) {
    state_->adoptedPending = adopted;
}
int Feature::Evaluate(ID3D12GraphicsCommandList *cmd, const OfpsFrameInputs *inputs, OfpsEvalResult *out) try {
    if (core_.InCallback())
        return OFPS_E_STATE;
    if (!cmd || !inputs || inputs->size < sizeof(*inputs) || !out || out->size < sizeof(uint32_t) ||
        cmd->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT)
        return OFPS_E_ARG;
    std::lock_guard lock(Ctx().mutex);
    const InsideCoreScope scope;
    if (!state_)
        return OFPS_E_STATE;
    DrainSubmissions(gpu::Submission::Signal::All);
    ++Ctx().evalCounter;
    DrainGraveyard(false);
    core_.DrainReleased();
    auto &st = *state_;
    st.frameModelCalled = false;
    st.frameModelResult = OFPS_OK;
    st.frameCreation = false;
    OfpsFrameInputs frame = *inputs;
    ResolveFrameViews(frame);
    if (ofps::temporal::Value(Ctx().temporalDiagnostics,
            ofps::temporal::DiagnosticId::Reset, 0.0f) != 0.0f)
        frame.hostReset = 1;
    const auto& diagnostics = Ctx().temporalDiagnostics;
    ofps::temporal::TemporalOverrides overrides{};
    using ofps::temporal::DiagnosticId;
    using ofps::temporal::Explicit;
    using ofps::temporal::Value;
    if (Explicit(diagnostics, DiagnosticId::ColourTolerance))
        overrides.colourTolerance = Value(diagnostics, DiagnosticId::ColourTolerance, 0.0f);
    if (Explicit(diagnostics, DiagnosticId::DepthTolerance))
        overrides.depthTolerance = Value(diagnostics, DiagnosticId::DepthTolerance, 0.0f);
    if (Ctx().diag.temporalBlend >= 0.0f)
        overrides.residualBlend = Ctx().diag.temporalBlend;
    if (Explicit(diagnostics, DiagnosticId::LumaLimit))
        overrides.lumaBand = Value(diagnostics, DiagnosticId::LumaLimit, 0.0f) != 0.0f;
    if (Value(diagnostics, DiagnosticId::NoLimit, 0.0f) != 0.0f)
        overrides.lumaBand = false;
    if (Explicit(diagnostics, DiagnosticId::FillFloor))
        overrides.fillFloor = Value(diagnostics, DiagnosticId::FillFloor, 0.0f) != 0.0f;
    ofps::temporal::TemporalProfile profile;
    try { profile = ofps::temporal::ResolveProfile(frame.colorDomain, false, overrides); }
    catch (const std::invalid_argument&) { return OFPS_E_ARG; }
    if (st.profileValid && !ofps::temporal::SameProfile(st.lastProfile, profile)) {
        RetireSpread(st, RetirementGate(st));
        if (st.temporal) st.temporal->Invalidate();
        if (st.async) { gpu::GateSet gate; BuryAsync(st, &gate); }
        st.sinceFull = 0;
    }
    st.lastProfile = profile;
    st.profileValid = true;
    Ctx().frameProfile = profile;
    OfpsEvalResult result{};
    result.size = sizeof(result);
    st.frameResult = &result;
    const int rc = EvaluateFrameGuarded(st, cmd, frame, result);
    Ctx().status.temporalStats = st.temporal ? st.temporal->Stats() : ofps::core::temporal::StatsSnapshot{};
    Ctx().status.temporalPassTiming = st.temporal ? st.temporal->Timing() : ofps::core::temporal::PassTimingSnapshot{};
    Ctx().status.temporalTimingEnabled = Ctx().diag.timing;
    Ctx().status.temporalGuideProbes = ofps::temporal::Value(Ctx().temporalDiagnostics,
        ofps::temporal::DiagnosticId::GuideProbes, 0.0f) != 0.0f;
    st.frameResult = nullptr;
    st.frameMotion = nullptr;
    Ctx().status.lastNgxResult = result.modelResult;
    const auto size = std::min<uint32_t>(out->size, sizeof(result));
    std::memcpy(out, &result, size);
    out->size = size;
    return rc;
} catch (...) {
    return OFPS_E_STATE;
}
void *Feature::CurrentModelHandle() {
    if (core_.InCallback())
        return nullptr;
    std::lock_guard lock(Ctx().mutex);
    return state_ ? state_->realHandle : nullptr;
}
void Feature::RequestModelRebuild() {
    if (core_.InCallback())
        return;
    std::lock_guard lock(Ctx().mutex);
    if (state_)
        state_->rebuildRequested = true;
}
void Feature::Release() {
    if (core_.InCallback())
        return;
    std::lock_guard lock(Ctx().mutex);
    const InsideCoreScope scope;
    if (!state_)
        return;
    auto &st = *state_;
    gpu::GateSet background;
    BuryAsync(st, &background);
    const auto gate = RetirementGate(st, background);
    RetireModelPasses(st, gate);
    BuryGpu(st, gate);
    if (st.realHandle)
        BuryReal(st.modelHost, st.realHandle, gate);
    core_.QueueReleased(st.modelHost, hostHandle_);
    Ctx().features.erase(hostHandle_);
    state_ = nullptr;
    Ctx().status.featureCreated = !Ctx().features.empty();
    core_.ForgetFeature(this);
}
} // namespace ofps::core
