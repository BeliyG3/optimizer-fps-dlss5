#pragma once

// The grid a feature's carried frame is reprojected on (TemporalGrid, 2026.10.1). The rule itself is
// core/temporal/grid.h; this applies it to the feature on the two routes that carry a warped frame
// (TemporalInputsWithBase and the background scheduler's carried frame) and logs a change of the size.
// Spread hidden machines, the native path and TemporalInputs itself stay native.

#include "core/frame/feature_state.h"
#include "core/temporal/machine.h"

namespace ofps::core {

// Sets tin.gridWidth / gridHeight from the feature's layout: the model's grid when the frame is warped (not on
// a model-resolution host), TemporalGrid is on and the three grid shaders are loaded, else native (0, 0). One log
// line when the size changes, reporting the grid that really runs (0x0 = native).
void SetCarriedGrid(FeatureState &st, ofps::core::temporal::FrameInputs &tin);

} // namespace ofps::core
