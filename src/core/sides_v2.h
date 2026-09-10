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

#include "peripheral_warp/types_v2.h"

#include <algorithm>
#include <cmath>

namespace pw {
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

// Geometry that does not depend on the split: band centre, half-band, peripheries, half-spans.
inline AxisSidesV2 AxisGeometryV2(std::uint32_t nativeExtent, std::uint32_t rawWorkExtent,
                                  std::uint32_t workExtent, float centerFraction,
                                  float offsetFraction) noexcept
{
    AxisSidesV2 s{};
    const float N = static_cast<float>(nativeExtent);
    s.nativeExtent = N;
    s.workExtent = static_cast<float>(workExtent);
    s.scale = static_cast<float>(workExtent) / static_cast<float>(rawWorkExtent);
    s.halfBand = 0.5f * centerFraction * N;
    s.bandCenter = 0.5f * N + offsetFraction * N;
    s.halfSpan[0] = s.bandCenter;
    s.halfSpan[1] = N - s.bandCenter;
    s.periphery[0] = std::max(0.0f, s.halfSpan[0] - s.halfBand);
    s.periphery[1] = std::max(0.0f, s.halfSpan[1] - s.halfBand);
    return s;
}

// The base split (half/half, capped by each side's native pixels) before any Work shift.
inline void BaseAllottedV2(const AxisSidesV2 &s, std::uint32_t rawWorkExtent, float centerFraction,
                           float out[2]) noexcept
{
    const float budget = std::max(0.0f, static_cast<float>(rawWorkExtent) - centerFraction * s.nativeExtent);
    const int narrow = s.periphery[0] <= s.periphery[1] ? 0 : 1;
    const int wide = 1 - narrow;
    out[narrow] = std::min(0.5f * budget, s.periphery[narrow]);
    out[wide] = std::min(budget - out[narrow], s.periphery[wide]);
}

// Work shift bounds in native pixels for this geometry: the contour may slide right (positive)
// while the left periphery still has work pixels to give and the right one native pixels to
// take, and vice versa.
inline void WorkShiftBoundsV2(const AxisSidesV2 &s, const float base[2], float *minShift,
                              float *maxShift) noexcept
{
    *maxShift = std::max(0.0f, std::min(base[0], s.periphery[1] - base[1]));
    *minShift = -std::max(0.0f, std::min(base[1], s.periphery[0] - base[0]));
}

// Fills allotted[] from the split rule, the Work shift and the whole-texel snap of the band.
inline void ComputeAllottedV2(AxisSidesV2 &s, std::uint32_t rawWorkExtent, std::uint32_t workExtent,
                              float centerFraction, float shiftFraction) noexcept
{
    float base[2];
    BaseAllottedV2(s, rawWorkExtent, centerFraction, base);
    float minShift = 0.0f, maxShift = 0.0f;
    WorkShiftBoundsV2(s, base, &minShift, &maxShift);
    const float shift = std::clamp(shiftFraction * s.nativeExtent, minShift, maxShift);
    s.allotted[0] = std::clamp(base[0] - shift, 0.0f, s.periphery[0]);
    s.allotted[1] = std::clamp(base[1] + shift, 0.0f, s.periphery[1]);
    // The 1:1 band must land on whole texels: with Global scale 100 the band is translated by
    // (workCenter - bandCenter); a fractional translation would make both Pack and Unpack sample
    // between texels and blur the band. Nudge the split between the two sides (by less than a
    // texel) so that translation is an integer, keeping every side within its native pixels.
    if (rawWorkExtent == workExtent) {
        const float wantedCenter = s.halfBand + s.allotted[0];
        const float translation = wantedCenter - s.bandCenter;
        const float candidates[3] = {std::round(translation), std::floor(translation), std::ceil(translation)};
        for (float candidate : candidates) {
            const float center = s.bandCenter + candidate;
            const float left = center - s.halfBand;
            const float right = static_cast<float>(rawWorkExtent) - center - s.halfBand;
            if (left < -1.0e-3f || right < -1.0e-3f) continue;
            if (left > s.periphery[0] + 1.0e-3f || right > s.periphery[1] + 1.0e-3f) continue;
            s.allotted[0] = std::clamp(left, 0.0f, s.periphery[0]);
            s.allotted[1] = std::clamp(right, 0.0f, s.periphery[1]);
            break;
        }
    }
}

// Derives the per-side curve parameters from allotted[].
inline void FinishSidesV2(AxisSidesV2 &s) noexcept
{
    for (int side = 0; side < 2; ++side) {
        const float H = std::max(s.halfSpan[side], 1.0e-6f);
        s.center[side] = s.halfBand / H;
        s.work[side] = (s.halfBand + s.allotted[side]) / H;
        s.compression[side] = s.periphery[side] > 0.0f ? s.allotted[side] / s.periphery[side] : 1.0f;
        s.edgeSlope[side] = s.compression[side] * s.compression[side];
    }
    s.workCenter = (s.halfBand + s.allotted[0]) * s.scale;
}

// Build time: from the configuration's fractions.
inline AxisSidesV2 ComputeSidesV2(std::uint32_t nativeExtent, std::uint32_t rawWorkExtent,
                                  std::uint32_t workExtent, float centerFraction,
                                  float offsetFraction, float shiftFraction) noexcept
{
    AxisSidesV2 s = AxisGeometryV2(nativeExtent, rawWorkExtent, workExtent, centerFraction, offsetFraction);
    ComputeAllottedV2(s, rawWorkExtent, workExtent, centerFraction, shiftFraction);
    FinishSidesV2(s);
    return s;
}

// Run time: from a built layout, whose per-side compression carries the split.
inline AxisSidesV2 AxisSides(const LayoutV2 &layout, std::uint32_t axis) noexcept
{
    AxisSidesV2 s = axis == 0
        ? AxisGeometryV2(layout.nativeWidth, layout.rawWorkWidth, layout.workWidth,
                         layout.centerFractionX, layout.centerOffsetX)
        : AxisGeometryV2(layout.nativeHeight, layout.rawWorkHeight, layout.workHeight,
                         layout.centerFractionY, layout.centerOffsetY);
    const float compression[2] = {axis == 0 ? layout.compressionXNeg : layout.compressionYNeg,
                                  axis == 0 ? layout.compressionXPos : layout.compressionYPos};
    if (layout.mode == WarpMode::Peripheral) {
        s.allotted[0] = compression[0] * s.periphery[0];
        s.allotted[1] = compression[1] * s.periphery[1];
    } else {
        s.allotted[0] = s.periphery[0];
        s.allotted[1] = s.periphery[1];
    }
    FinishSidesV2(s);
    return s;
}

} // namespace detail
} // namespace pw
