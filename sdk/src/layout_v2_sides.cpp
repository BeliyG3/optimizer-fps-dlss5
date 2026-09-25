#include "optimizer_fps/types_v2.h"

#include "sides_v2.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>


namespace ofps::sdk::detail {

// Geometry that does not depend on the split: band centre, half-band, peripheries, half-spans.
AxisSidesV2 AxisGeometryV2(std::uint32_t nativeExtent, std::uint32_t rawWorkExtent,
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
void BaseAllottedV2(const AxisSidesV2 &s, std::uint32_t rawWorkExtent, float centerFraction,
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
void WorkShiftBoundsV2(const AxisSidesV2 &s, const float base[2], float *minShift,
                              float *maxShift) noexcept
{
    *maxShift = std::max(0.0f, std::min(base[0], s.periphery[1] - base[1]));
    *minShift = -std::max(0.0f, std::min(base[1], s.periphery[0] - base[0]));
}

// Fills allotted[] from the split rule, the Work shift and the whole-texel snap of the band.
void ComputeAllottedV2(AxisSidesV2 &s, std::uint32_t rawWorkExtent, std::uint32_t workExtent,
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
void FinishSidesV2(AxisSidesV2 &s) noexcept
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
AxisSidesV2 ComputeSidesV2(std::uint32_t nativeExtent, std::uint32_t rawWorkExtent,
                                  std::uint32_t workExtent, float centerFraction,
                                  float offsetFraction, float shiftFraction) noexcept
{
    AxisSidesV2 s = AxisGeometryV2(nativeExtent, rawWorkExtent, workExtent, centerFraction, offsetFraction);
    ComputeAllottedV2(s, rawWorkExtent, workExtent, centerFraction, shiftFraction);
    FinishSidesV2(s);
    return s;
}

// Run time: from a built layout, whose per-side compression carries the split.
AxisSidesV2 AxisSides(const LayoutV2 &layout, std::uint32_t axis) noexcept
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

bool IsFinite(float value) noexcept { return std::isfinite(value); }
std::uint32_t EvenExtent(std::uint32_t nativeExtent, double fraction) noexcept
{
    if (fraction >= 1.0) return nativeExtent;
    double scaled = static_cast<double>(nativeExtent) * fraction;
    const double nearestInteger = std::round(scaled);
    if (std::abs(scaled - nearestInteger) <=
        1.0e-6 * std::max(1.0, std::abs(scaled)))
        scaled = nearestInteger;
    auto extent = static_cast<std::uint32_t>(std::ceil(scaled));
    if ((extent & 1u) != 0) ++extent;
    extent = std::max<std::uint32_t>(2, extent);
    const std::uint32_t largestEven = nativeExtent & ~1u;
    return std::min(extent, largestEven);
}

float MaximumCenterOffsetPercent(float centerPercent) noexcept
{
    return std::max(0.0f, (100.0f - centerPercent) * 0.5f - kMinimumSidePeripheryPercentV2);
}

void WorkShiftLimitsFraction(const AxisConfig &axis, float offsetFraction, float *minFraction,
                             float *maxFraction) noexcept
{
    const float c = axis.centerPercent * 0.01f;
    const float w = axis.workPercent * 0.01f;
    const float halfBand = 0.5f * c;
    const float bandCenter = 0.5f + offsetFraction;
    const float periphery[2] = {std::max(0.0f, bandCenter - halfBand), std::max(0.0f, 1.0f - bandCenter - halfBand)};
    const float budget = std::max(0.0f, w - c);
    const int narrow = periphery[0] <= periphery[1] ? 0 : 1;
    float base[2] = {};
    base[narrow] = std::min(0.5f * budget, periphery[narrow]);
    base[1 - narrow] = std::min(budget - base[narrow], periphery[1 - narrow]);
    *maxFraction = std::max(0.0f, std::min(base[0], periphery[1] - base[1]));
    *minFraction = -std::max(0.0f, std::min(base[1], periphery[0] - base[0]));
}

// Invariants of a built layout's per-side split for one axis: every periphery keeps between zero
// and all of its native pixels, the two together use exactly the raw Work periphery budget, and
// at Global scale 100 the band is translated by whole texels.
bool SidesConsistent(const LayoutV2 &layout, std::uint32_t axis) noexcept
{
    const detail::AxisSidesV2 s = detail::AxisSides(layout, axis);
    const float rawWork = axis == 0 ? static_cast<float>(layout.rawWorkWidth) : static_cast<float>(layout.rawWorkHeight);
    const float centerFraction = axis == 0 ? layout.centerFractionX : layout.centerFractionY;
    const float budget = std::max(0.0f, rawWork - centerFraction * s.nativeExtent);
    for (int side = 0; side < 2; ++side) {
        if (s.allotted[side] < -1.0e-3f || s.allotted[side] > s.periphery[side] + 1.0e-3f) return false;
        if (!IsFinite(s.compression[side]) || s.compression[side] < 0.0f || s.compression[side] > 1.0f + 1.0e-5f)
            return false;
    }
    if (std::abs(s.allotted[0] + s.allotted[1] - budget) > 0.05f) return false;
    if (s.scale == 1.0f) {
        const float translation = s.workCenter - s.bandCenter;
        if (std::abs(translation - std::round(translation)) > 1.0e-2f) return false;
    }
    return true;
}

float SafeReciprocal(float value) noexcept
{
    return value > 0.0f ? 1.0f / value : std::numeric_limits<float>::infinity();
}

std::uint32_t ExpectedDiagnostics(const LayoutV2 &layout) noexcept
{
    if (layout.mode != WarpMode::Peripheral) return LayoutDiagnosticNone;
    std::uint32_t result = LayoutDiagnosticNone;
    if (layout.compressionX + 1.0e-6f < kAggressivePeripheralCompressionV2)
        result |= LayoutDiagnosticAggressivePeripheralX;
    if (layout.compressionY + 1.0e-6f < kAggressivePeripheralCompressionV2)
        result |= LayoutDiagnosticAggressivePeripheralY;
    if (layout.minimumLocalScaleX + 1.0e-6f < 0.25f)
        result |= LayoutDiagnosticAliasingRiskX;
    if (layout.minimumLocalScaleY + 1.0e-6f < 0.25f)
        result |= LayoutDiagnosticAliasingRiskY;
    return result;
}

} // namespace ofps::sdk::detail

namespace ofps::sdk {
using detail::EvenExtent;
using detail::SafeReciprocal;
using detail::ExpectedDiagnostics;
using detail::MaximumCenterOffsetPercent;
using detail::WorkShiftLimitsFraction;

namespace {
bool IsFinite(float value) noexcept { return std::isfinite(value); }
} // namespace

float MaximumCenterOffsetPercentV2(float centerPercent) noexcept
{
    return MaximumCenterOffsetPercent(centerPercent);
}

void WorkShiftLimitsPercentV2(const ConfigV2 &config, std::uint32_t axis, float *minPercent,
                              float *maxPercent) noexcept
{
    float lo = 0.0f, hi = 0.0f;
    if (config.mode == WarpMode::Peripheral) {
        const AxisConfig &a = axis == 0 ? config.xAxis : config.yAxis;
        const float offset = (axis == 0 ? config.centerOffsetXPercent : config.centerOffsetYPercent) * 0.01f;
        if (IsFinite(a.centerPercent) && IsFinite(a.workPercent) && IsFinite(offset))
            WorkShiftLimitsFraction(a, offset, &lo, &hi);
    }
    if (minPercent) *minPercent = lo * 100.0f;
    if (maxPercent) *maxPercent = hi * 100.0f;
}

Status BuildLayout(const ConfigV2 &config, std::uint32_t nativeWidth,
                   std::uint32_t nativeHeight, LayoutV2 *layout) noexcept
{
    if (layout == nullptr) return Status::NullArgument;
    const Status status = ValidateConfig(config);
    if (status != Status::Ok) return status;
    if (nativeWidth < 2 || nativeHeight < 2) return Status::InvalidDimensions;

    const float configuredWorkX = config.mode == WarpMode::Off
        ? 1.0f : config.xAxis.workPercent * 0.01f;
    const float configuredWorkY = config.mode == WarpMode::Off
        ? 1.0f : config.yAxis.workPercent * 0.01f;
    const double globalScale = static_cast<double>(config.globalScalePercent) * 0.01;
    const std::uint32_t rawWorkWidth = EvenExtent(nativeWidth, configuredWorkX);
    const std::uint32_t rawWorkHeight = EvenExtent(nativeHeight, configuredWorkY);
    const std::uint32_t workWidth = EvenExtent(
        nativeWidth, globalScale * static_cast<double>(configuredWorkX));
    const std::uint32_t workHeight = EvenExtent(
        nativeHeight, globalScale * static_cast<double>(configuredWorkY));

    LayoutV2 result{};
    result.structSize = sizeof(result);
    result.version = kAbiVersionV2;
    result.mode = config.mode;
    result.colorFilter = config.colorFilter;
    result.nativeWidth = nativeWidth;
    result.nativeHeight = nativeHeight;
    result.rawWorkWidth = rawWorkWidth;
    result.rawWorkHeight = rawWorkHeight;
    result.workWidth = workWidth;
    result.workHeight = workHeight;
    result.configuredWorkFractionX = configuredWorkX;
    result.configuredWorkFractionY = configuredWorkY;
    result.rawWorkFractionX = static_cast<float>(rawWorkWidth) /
                              static_cast<float>(nativeWidth);
    result.rawWorkFractionY = static_cast<float>(rawWorkHeight) /
                              static_cast<float>(nativeHeight);
    result.centerFractionX = config.mode == WarpMode::Peripheral
        ? config.xAxis.centerPercent * 0.01f : result.rawWorkFractionX;
    result.centerFractionY = config.mode == WarpMode::Peripheral
        ? config.yAxis.centerPercent * 0.01f : result.rawWorkFractionY;
    result.globalScalePercent = config.globalScalePercent;
    result.effectiveScaleX = static_cast<float>(workWidth) /
                             static_cast<float>(rawWorkWidth);
    result.effectiveScaleY = static_cast<float>(workHeight) /
                             static_cast<float>(rawWorkHeight);

    if (config.mode == WarpMode::Peripheral) {
        if (result.centerFractionX >= result.rawWorkFractionX ||
            result.centerFractionY >= result.rawWorkFractionY)
            return Status::InvalidDimensions;
        result.centerOffsetX = config.centerOffsetXPercent * 0.01f;
        result.centerOffsetY = config.centerOffsetYPercent * 0.01f;
        const detail::AxisSidesV2 sx = detail::ComputeSidesV2(nativeWidth, rawWorkWidth, workWidth,
                                                              result.centerFractionX, result.centerOffsetX,
                                                              config.workShiftXPercent * 0.01f);
        const detail::AxisSidesV2 sy = detail::ComputeSidesV2(nativeHeight, rawWorkHeight, workHeight,
                                                              result.centerFractionY, result.centerOffsetY,
                                                              config.workShiftYPercent * 0.01f);
        result.compressionXNeg = sx.compression[0];
        result.compressionXPos = sx.compression[1];
        result.compressionYNeg = sy.compression[0];
        result.compressionYPos = sy.compression[1];
        result.edgeSlopeXNeg = sx.edgeSlope[0];
        result.edgeSlopeXPos = sx.edgeSlope[1];
        result.edgeSlopeYNeg = sy.edgeSlope[0];
        result.edgeSlopeYPos = sy.edgeSlope[1];
        // The symmetric fields report the harder-compressed side (the one that matters for
        // aliasing and footprint diagnostics); with no offset both sides are equal.
        result.compressionX = std::min(sx.compression[0], sx.compression[1]);
        result.compressionY = std::min(sy.compression[0], sy.compression[1]);
        result.edgeSlopeX = result.compressionX * result.compressionX;
        result.edgeSlopeY = result.compressionY * result.compressionY;
        result.minimumLocalScaleX = result.effectiveScaleX * result.edgeSlopeX;
        result.minimumLocalScaleY = result.effectiveScaleY * result.edgeSlopeY;
    } else {
        result.compressionX = result.rawWorkFractionX < 1.0f ? 0.0f : 1.0f;
        result.compressionY = result.rawWorkFractionY < 1.0f ? 0.0f : 1.0f;
        result.edgeSlopeX = result.compressionX * result.compressionX;
        result.edgeSlopeY = result.compressionY * result.compressionY;
        result.compressionXNeg = result.compressionXPos = result.compressionX;
        result.compressionYNeg = result.compressionYPos = result.compressionY;
        result.edgeSlopeXNeg = result.edgeSlopeXPos = result.edgeSlopeX;
        result.edgeSlopeYNeg = result.edgeSlopeYPos = result.edgeSlopeY;
        result.minimumLocalScaleX = static_cast<float>(workWidth) /
                                    static_cast<float>(nativeWidth);
        result.minimumLocalScaleY = static_cast<float>(workHeight) /
                                    static_cast<float>(nativeHeight);
    }
    result.maximumSourceFootprintX = SafeReciprocal(result.minimumLocalScaleX);
    result.maximumSourceFootprintY = SafeReciprocal(result.minimumLocalScaleY);
    result.pixelPercent = 100.0f * static_cast<float>(
        (static_cast<double>(workWidth) * static_cast<double>(workHeight)) /
        (static_cast<double>(nativeWidth) * static_cast<double>(nativeHeight)));
    result.flags = config.flags;
    result.diagnosticFlags = ExpectedDiagnostics(result);
    *layout = result;
    return Status::Ok;
}


} // namespace ofps::sdk
