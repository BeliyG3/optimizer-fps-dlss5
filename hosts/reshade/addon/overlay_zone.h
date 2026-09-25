#pragma once

// The zone pad of the Optimizer FPS tab: a miniature of the frame with the raw Work region (orange)
// and the 1:1 centre band (cyan), which the user drags to move the band.

#include "core/api/ofps_core.h"

namespace ofps::reshade {

// Draws the pad at the cursor and applies a drag to the centre offset. True when the offset changed.
// layout may be null (no feature yet): the pad then works from the configuration's percentages.
bool DrawZonePad(float &offsetX, float &offsetY, const OfpsLayoutPreview *layout);

} // namespace ofps::reshade
