#pragma once

// The temporal modes (stage 24): which frame runs the model and which one is reprojected from the
// last residual, the machine's lifetime, and the host's inputs translated into the machine's terms.

#include "core/frame/feature_state.h"
#include "core/temporal/machine.h"

namespace ofps::core {

// ---------------------------------------------------------------------------------------------
// Temporal modes: frame planning and the machine's lifetime.
// ---------------------------------------------------------------------------------------------
struct TemporalPlan {
    bool active = false;
    bool full = true;
};

void TemporalReason(FeatureState &st, const char *fmt, ...);
bool EnsureTemporalDevice(FeatureState &st, ID3D12GraphicsCommandList *cmd, ID3D12Resource *output);
bool EnsureTemporal(FeatureState &st, ID3D12GraphicsCommandList *cmd, ID3D12Resource *output, ID3D12Resource *motion,
                    ID3D12Resource *depth);
bool BackgroundModeUsable(FeatureState &st);
// The background mode may run on this feature (mode 3 on a direct-list, frame-grid host): the texture
// sets it needs (pack slot 4, the machine's pending chains) are prepared. A superset of
// EffectiveTemporalMode() == 3, which also needs a registered host queue.
bool BackgroundCapable(const FeatureState &st);
int EffectiveTemporalMode(FeatureState &st);
TemporalPlan PlanTemporal(FeatureState &st, bool hostReset);
ofps::core::temporal::FrameInputs TemporalInputs(const OfpsResource &color, const OfpsResource &motion,
                                                 const OfpsResource &depth, float mvScaleX, float mvScaleY,
                                                 bool depthInverted, bool hostModelGrid);
bool TemporalExhausted(FeatureState &st);
void TemporalFrameDone(FeatureState &st, bool full);
// Temporal modes without the warp (Mode Off): kNotHandled falls back to the plain pass-through.
int NativeTemporalEvaluate(FeatureState &st, ID3D12GraphicsCommandList *cmd, const OfpsModelInputs &inputs, const OfpsFrameInputs &frame);
int TryTemporalRoute(FeatureState &st, ID3D12GraphicsCommandList *cmd,
                     const OfpsModelInputs &inputs, const OfpsFrameInputs &frame, OfpsEvalResult &result);

} // namespace ofps::core
