#pragma once

// The grid a carried frame's reprojection runs on (Machine::RecordReproject, FrameInputs::gridWidth/gridHeight).
//
// The model's edit never has more detail than the model's frame, so where the warp compresses the model's
// input the reprojection need not decide for every native pixel: a uniform grid matched to the model's
// densest region is enough. The rule is pure; the caller decides when it applies.

#include "optimizer_fps/types_v2.h"

#include <cstdint>

namespace ofps::core::temporal {

struct CarriedGrid {
    std::uint32_t width = 0, height = 0; // 0 = native (the grid is not used)
};

// The model's density per axis - Uniform: work / native; Peripheral: the 1:1 centre after Global scale,
// work / raw work (effectiveScale) - decides. Native (0, 0) when the grid is off (`enabled`), the frame is
// not warped, the mode is Off, a native extent is under 32 texels, or either density is not finite, <= 0 or
// above 0.8 (a quality gate: a smaller gain is not worth the coarser acceptance). Otherwise each extent is
// ceil(native * density) from the quantized integer extents, clamped to [16, native].
CarriedGrid CarriedGridFor(const ofps::sdk::LayoutV2 &layout, bool warped, bool enabled);

} // namespace ofps::core::temporal
