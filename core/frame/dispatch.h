#pragma once

// The three hooked entry points of feature 18 and the state they drive: the model behind the
// host's handle (created, re-created at the layout's extent, adopted when it predates the hook)
// and the evaluate that chooses between the warp, the temporal paths and the plain pass-through.

#include "core/frame/feature_state.h"

namespace ofps::core {

int EvaluateFrameBody(FeatureState &st, ID3D12GraphicsCommandList *cmd, const OfpsFrameInputs &frame, OfpsEvalResult &result);
int EvaluateFrameGuarded(FeatureState &st, ID3D12GraphicsCommandList *cmd, const OfpsFrameInputs &frame, OfpsEvalResult &result);

} // namespace ofps::core
