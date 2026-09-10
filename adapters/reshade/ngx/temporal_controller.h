#pragma once

// The temporal modes (stage 24): which frame runs the model and which one is reprojected from the
// last residual, the machine's lifetime, and the host's inputs translated into the machine's terms.

#include "feature_state.h"
#include "ngx_temporal.h"

namespace pwhook {

// ---------------------------------------------------------------------------------------------
// Temporal modes: frame planning and the machine's lifetime.
// ---------------------------------------------------------------------------------------------
struct TemporalPlan {
    bool active = false;
    bool full = true;
};

void TemporalReason(FeatureState &st, const char *fmt, ...);
bool EnsureTemporal(FeatureState &st, ID3D12GraphicsCommandList *cmd, ID3D12Resource *output, ID3D12Resource *motion,
                    ID3D12Resource *depth);
bool BackgroundModeUsable(FeatureState &st);
int EffectiveTemporalMode(FeatureState &st);
TemporalPlan PlanTemporal(FeatureState &st, bool hostReset);
pwtemporal::FrameInputs TemporalInputs(ID3D12Resource *color, ID3D12Resource *motion, ID3D12Resource *depth, DXGI_FORMAT colorView,
                                       DXGI_FORMAT motionView, DXGI_FORMAT depthView, const Subrect &colorRect, const Subrect &motionRect,
                                       const Subrect &depthRect, float mvScaleX, float mvScaleY, bool depthInverted);
void TemporalFrameDone(FeatureState &st, bool full);
// Temporal modes without the warp (Mode Off): kNotHandled falls back to the plain pass-through.
int NativeTemporalEvaluate(FeatureState &st, ID3D12GraphicsCommandList *cmd, void *params, void *callback);

} // namespace pwhook
