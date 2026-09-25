#pragma once

// Per-side axis parameters for ABI v2 layouts with a centre offset and a Work shift. Internal to
// the SDK: the same numbers are produced for the CPU reference math (math.cpp), the shader
// constants (layout_v2.cpp) and the validation, so all three stay in step.
//
// On one axis of native extent N with centre band cN (kept 1:1), raw work extent wN and a signed
// offset oN of the band centre, the two peripheries are P- = Xc - cN/2 and P+ = (N - Xc) - cN/2
// with Xc = N/2 + oN. The work periphery budget B = wN - cN is split half/half, but a side never
// receives more work pixels than it has native pixels; the remainder goes to the wider side, which
// is therefore compressed harder. A Work shift sN then moves work pixels from one periphery to the
// other (the raw Work rectangle slides by sN while the band stays), within the same bounds. With
// Global scale 100 the split is finally nudged by less than a texel so the band is translated by a
// whole number of texels (a fractional translation would blur it). Each side then has its own
// normalised curve parameters (centre fraction, work fraction, compression, edge slope) over its
// half-span H (from the band centre to the frame edge). With o = s = 0 both sides equal the
// classic symmetric values.
//
// The layout stores the result as the per-side compression (allotted / periphery); everything
// else is re-derived from it, so the layout, not the split rule, is the source of truth.

#include "optimizer_fps/types_v2.h"

#include <algorithm>
#include <cmath>

namespace ofps::sdk {
namespace detail {

struct AxisSidesV2 {
    float nativeExtent = 0.0f;   // N
    float workExtent = 0.0f;     // W (after Global scale)
    float bandCenter = 0.0f;     // Xc, native pixels
    float workCenter = 0.0f;     // Wc, work pixels
    float halfBand = 0.0f;       // cN/2
    float periphery[2] = {};     // P- , P+ (native pixels)
    float allotted[2] = {};      // b- , b+ (work pixels handed to each periphery)
    float halfSpan[2] = {};      // H- , H+ (native pixels)
    float center[2] = {};        // c_s = (cN/2) / H_s
    float work[2] = {};          // w_s = (cN/2 + b_s) / H_s
    float compression[2] = {};   // k_s = b_s / P_s
    float edgeSlope[2] = {};     // k_s^2
    float scale = 1.0f;          // W / (wN): work pixels per native-scaled pixel
};

AxisSidesV2 ComputeSidesV2(std::uint32_t nativeExtent, std::uint32_t rawWorkExtent,
                           std::uint32_t workExtent, float centerFraction,
                           float offsetFraction, float shiftFraction) noexcept;
AxisSidesV2 AxisSides(const LayoutV2 &layout, std::uint32_t axis) noexcept;

// Helpers shared by layout construction, validation and configuration checks.
bool IsKnownMode(WarpMode mode) noexcept;
bool IsKnownFilter(ColorFilter filter) noexcept;
std::uint32_t EvenExtent(std::uint32_t nativeExtent, double fraction) noexcept;
float MaximumCenterOffsetPercent(float centerPercent) noexcept;
void WorkShiftLimitsFraction(const AxisConfig &axis, float offsetFraction,
                             float *minFraction, float *maxFraction) noexcept;
bool SidesConsistent(const LayoutV2 &layout, std::uint32_t axis) noexcept;
float SafeReciprocal(float value) noexcept;
std::uint32_t ExpectedDiagnostics(const LayoutV2 &layout) noexcept;

} // namespace detail
} // namespace ofps::sdk
