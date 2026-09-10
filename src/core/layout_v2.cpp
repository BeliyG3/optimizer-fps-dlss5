#include "peripheral_warp/types_v2.h"

#include "sides_v2.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>

namespace pw {
namespace {

constexpr std::uint32_t kKnownConfigFlags =
    ConfigFlagExtendMotionAtEdge | ConfigFlagInputConfidenceValid;
constexpr std::uint32_t kKnownDiagnosticFlags =
    LayoutDiagnosticAggressivePeripheralX | LayoutDiagnosticAggressivePeripheralY |
    LayoutDiagnosticAliasingRiskX | LayoutDiagnosticAliasingRiskY;
constexpr float kPercentEpsilon = 1.0e-4f;

bool IsFinite(float value) noexcept { return std::isfinite(value); }

bool IsKnownMode(WarpMode mode) noexcept
{
    return mode == WarpMode::Off || mode == WarpMode::Uniform ||
           mode == WarpMode::Peripheral;
}

bool IsKnownFilter(ColorFilter filter) noexcept
{
    return filter == ColorFilter::Bilinear || filter == ColorFilter::AdaptiveFourTap;
}

template <std::size_t Size>
bool AllZero(const std::uint32_t (&values)[Size]) noexcept
{
    return std::all_of(std::begin(values), std::end(values),
                       [](std::uint32_t value) { return value == 0; });
}

bool NearlyEqual(float lhs, float rhs, float relativeTolerance = 1.0e-5f) noexcept
{
    const float scale = std::max({1.0f, std::abs(lhs), std::abs(rhs)});
    return std::abs(lhs - rhs) <= relativeTolerance * scale;
}

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

bool ConfigAxisIsValid(const AxisConfig &axis, WarpMode mode) noexcept
{
    if (!IsFinite(axis.workPercent) || axis.workPercent < kMinimumWorkPercentV2 ||
        axis.workPercent > 100.0f)
        return false;
    if (mode != WarpMode::Peripheral) return true;
    return IsFinite(axis.centerPercent) && axis.centerPercent > 0.0f &&
           axis.centerPercent < axis.workPercent;
}

float MaximumCenterOffsetPercent(float centerPercent) noexcept
{
    return std::max(0.0f, (100.0f - centerPercent) * 0.5f - kMinimumSidePeripheryPercentV2);
}

bool OffsetIsValid(float offsetPercent, const AxisConfig &axis, WarpMode mode) noexcept
{
    if (!IsFinite(offsetPercent)) return false;
    if (mode != WarpMode::Peripheral) return true;
    return std::abs(offsetPercent) <= MaximumCenterOffsetPercent(axis.centerPercent) + kPercentEpsilon;
}

// Work-shift bounds from the configuration's fractions (no quantisation; BuildLayout clamps the
// shift to the exact pixel bounds anyway).
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

bool WorkShiftIsValid(float shiftPercent, const AxisConfig &axis, float offsetPercent, WarpMode mode) noexcept
{
    if (!IsFinite(shiftPercent)) return false;
    if (mode != WarpMode::Peripheral) return true;
    float lo = 0.0f, hi = 0.0f;
    WorkShiftLimitsFraction(axis, offsetPercent * 0.01f, &lo, &hi);
    return shiftPercent * 0.01f >= lo - kPercentEpsilon && shiftPercent * 0.01f <= hi + kPercentEpsilon;
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

} // namespace

ConfigV2 DefaultConfigV2() noexcept
{
    ConfigV2 config{};
    config.structSize = sizeof(config);
    config.version = kAbiVersionV2;
    config.mode = WarpMode::Peripheral;
    config.colorFilter = ColorFilter::AdaptiveFourTap;
    config.xAxis = {80.0f, 90.0f};
    config.yAxis = {80.0f, 90.0f};
    config.globalScalePercent = 100.0f;
    config.flags = ConfigFlagExtendMotionAtEdge;
    return config;
}

float MinimumGlobalScalePercent(const ConfigV2 &config) noexcept
{
    if (!IsFinite(config.xAxis.workPercent) || !IsFinite(config.yAxis.workPercent) ||
        config.xAxis.workPercent <= 0.0f || config.yAxis.workPercent <= 0.0f)
        return 100.0f;
    if (config.mode == WarpMode::Off) return kMinimumEffectiveScalePercentV2;
    const float minimumX = 100.0f * kMinimumEffectiveScalePercentV2 /
                           config.xAxis.workPercent;
    const float minimumY = 100.0f * kMinimumEffectiveScalePercentV2 /
                           config.yAxis.workPercent;
    return std::clamp(std::max(minimumX, minimumY),
                      kMinimumEffectiveScalePercentV2, 100.0f);
}

Status ValidateConfig(const ConfigV2 &config) noexcept
{
    if (config.structSize != sizeof(ConfigV2)) return Status::StructSizeMismatch;
    if (config.version != kAbiVersionV2) return Status::VersionMismatch;
    if (!IsKnownMode(config.mode)) return Status::InvalidMode;
    if (!IsKnownFilter(config.colorFilter)) return Status::InvalidFilter;
    if ((config.flags & ~kKnownConfigFlags) != 0 || !AllZero(config.reserved))
        return Status::InvalidFlags;
    if (!ConfigAxisIsValid(config.xAxis, config.mode) ||
        !ConfigAxisIsValid(config.yAxis, config.mode) ||
        !IsFinite(config.globalScalePercent) ||
        config.globalScalePercent < kMinimumEffectiveScalePercentV2 ||
        config.globalScalePercent > 100.0f)
        return Status::InvalidAxis;
    if (config.globalScalePercent + kPercentEpsilon <
        MinimumGlobalScalePercent(config))
        return Status::InvalidAxis;
    if (!OffsetIsValid(config.centerOffsetXPercent, config.xAxis, config.mode) ||
        !OffsetIsValid(config.centerOffsetYPercent, config.yAxis, config.mode))
        return Status::InvalidAxis;
    if (!WorkShiftIsValid(config.workShiftXPercent, config.xAxis, config.centerOffsetXPercent, config.mode) ||
        !WorkShiftIsValid(config.workShiftYPercent, config.yAxis, config.centerOffsetYPercent, config.mode))
        return Status::InvalidAxis;
    return Status::Ok;
}

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

Status ValidateLayout(const LayoutV2 &layout) noexcept
{
    if (layout.structSize != sizeof(LayoutV2)) return Status::StructSizeMismatch;
    if (layout.version != kAbiVersionV2) return Status::VersionMismatch;
    if (!IsKnownMode(layout.mode)) return Status::InvalidMode;
    if (!IsKnownFilter(layout.colorFilter)) return Status::InvalidFilter;
    if ((layout.flags & ~kKnownConfigFlags) != 0 ||
        (layout.diagnosticFlags & ~kKnownDiagnosticFlags) != 0)
        return Status::InvalidFlags;
    if (layout.nativeWidth < 2 || layout.nativeHeight < 2 ||
        layout.rawWorkWidth < 2 || layout.rawWorkHeight < 2 ||
        layout.workWidth < 2 || layout.workHeight < 2 ||
        layout.rawWorkWidth > layout.nativeWidth ||
        layout.rawWorkHeight > layout.nativeHeight ||
        layout.workWidth > layout.rawWorkWidth || layout.workHeight > layout.rawWorkHeight)
        return Status::InvalidDimensions;
    if ((layout.rawWorkWidth < layout.nativeWidth && (layout.rawWorkWidth & 1u) != 0) ||
        (layout.rawWorkHeight < layout.nativeHeight && (layout.rawWorkHeight & 1u) != 0) ||
        (layout.workWidth < layout.nativeWidth && (layout.workWidth & 1u) != 0) ||
        (layout.workHeight < layout.nativeHeight && (layout.workHeight & 1u) != 0))
        return Status::InvalidDimensions;

    const float values[] = {
        layout.centerFractionX, layout.centerFractionY,
        layout.configuredWorkFractionX, layout.configuredWorkFractionY,
        layout.rawWorkFractionX, layout.rawWorkFractionY,
        layout.globalScalePercent, layout.effectiveScaleX, layout.effectiveScaleY,
        layout.compressionX, layout.compressionY, layout.edgeSlopeX, layout.edgeSlopeY,
        layout.minimumLocalScaleX, layout.minimumLocalScaleY,
        layout.maximumSourceFootprintX, layout.maximumSourceFootprintY,
        layout.pixelPercent, layout.centerOffsetX, layout.centerOffsetY,
        layout.compressionXNeg, layout.compressionXPos, layout.compressionYNeg,
        layout.compressionYPos, layout.edgeSlopeXNeg, layout.edgeSlopeXPos,
        layout.edgeSlopeYNeg, layout.edgeSlopeYPos};
    if (!std::all_of(std::begin(values), std::end(values), IsFinite))
        return Status::InvalidAxis;
    if (layout.configuredWorkFractionX < 0.25f ||
        layout.configuredWorkFractionX > 1.0f ||
        layout.configuredWorkFractionY < 0.25f ||
        layout.configuredWorkFractionY > 1.0f ||
        layout.globalScalePercent < 25.0f || layout.globalScalePercent > 100.0f)
        return Status::InvalidAxis;
    const float globalFraction = layout.globalScalePercent * 0.01f;
    if (globalFraction * layout.configuredWorkFractionX + 1.0e-6f < 0.25f ||
        globalFraction * layout.configuredWorkFractionY + 1.0e-6f < 0.25f)
        return Status::InvalidAxis;

    const auto expectedRawWidth = EvenExtent(layout.nativeWidth,
                                              layout.configuredWorkFractionX);
    const auto expectedRawHeight = EvenExtent(layout.nativeHeight,
                                               layout.configuredWorkFractionY);
    const double global = static_cast<double>(layout.globalScalePercent) * 0.01;
    const auto expectedWorkWidth = EvenExtent(
        layout.nativeWidth, global * layout.configuredWorkFractionX);
    const auto expectedWorkHeight = EvenExtent(
        layout.nativeHeight, global * layout.configuredWorkFractionY);
    if (layout.rawWorkWidth != expectedRawWidth || layout.rawWorkHeight != expectedRawHeight ||
        layout.workWidth != expectedWorkWidth || layout.workHeight != expectedWorkHeight)
        return Status::InvalidDimensions;
    if (static_cast<double>(layout.workWidth) / layout.nativeWidth + 1.0e-9 < 0.25 ||
        static_cast<double>(layout.workHeight) / layout.nativeHeight + 1.0e-9 < 0.25)
        return Status::InvalidDimensions;

    const float rawFractionX = static_cast<float>(layout.rawWorkWidth) /
                               static_cast<float>(layout.nativeWidth);
    const float rawFractionY = static_cast<float>(layout.rawWorkHeight) /
                               static_cast<float>(layout.nativeHeight);
    const float effectiveScaleX = static_cast<float>(layout.workWidth) /
                                  static_cast<float>(layout.rawWorkWidth);
    const float effectiveScaleY = static_cast<float>(layout.workHeight) /
                                  static_cast<float>(layout.rawWorkHeight);
    const float pixelPercent = 100.0f * static_cast<float>(
        (static_cast<double>(layout.workWidth) * layout.workHeight) /
        (static_cast<double>(layout.nativeWidth) * layout.nativeHeight));
    if (!NearlyEqual(layout.rawWorkFractionX, rawFractionX) ||
        !NearlyEqual(layout.rawWorkFractionY, rawFractionY) ||
        !NearlyEqual(layout.effectiveScaleX, effectiveScaleX) ||
        !NearlyEqual(layout.effectiveScaleY, effectiveScaleY) ||
        !NearlyEqual(layout.pixelPercent, pixelPercent))
        return Status::InvalidDimensions;

    float expectedCompressionX = rawFractionX < 1.0f ? 0.0f : 1.0f;
    float expectedCompressionY = rawFractionY < 1.0f ? 0.0f : 1.0f;
    float expectedSidesX[2] = {expectedCompressionX, expectedCompressionX};
    float expectedSidesY[2] = {expectedCompressionY, expectedCompressionY};
    float expectedMinimumX = static_cast<float>(layout.workWidth) /
                             static_cast<float>(layout.nativeWidth);
    float expectedMinimumY = static_cast<float>(layout.workHeight) /
                             static_cast<float>(layout.nativeHeight);
    if (layout.mode == WarpMode::Peripheral) {
        if (layout.centerFractionX <= 0.0f || layout.centerFractionY <= 0.0f ||
            layout.centerFractionX >= rawFractionX || layout.centerFractionY >= rawFractionY)
            return Status::InvalidAxis;
        const float limitX = MaximumCenterOffsetPercent(layout.centerFractionX * 100.0f) * 0.01f;
        const float limitY = MaximumCenterOffsetPercent(layout.centerFractionY * 100.0f) * 0.01f;
        if (std::abs(layout.centerOffsetX) > limitX + kPercentEpsilon ||
            std::abs(layout.centerOffsetY) > limitY + kPercentEpsilon)
            return Status::InvalidAxis;
        // The per-side split is the layout's own truth (it carries the Work shift); check its
        // invariants instead of re-deriving it, then hold the symmetric fields to it.
        if (!SidesConsistent(layout, 0) || !SidesConsistent(layout, 1)) return Status::InvalidAxis;
        expectedSidesX[0] = layout.compressionXNeg;
        expectedSidesX[1] = layout.compressionXPos;
        expectedSidesY[0] = layout.compressionYNeg;
        expectedSidesY[1] = layout.compressionYPos;
        expectedCompressionX = std::min(layout.compressionXNeg, layout.compressionXPos);
        expectedCompressionY = std::min(layout.compressionYNeg, layout.compressionYPos);
        expectedMinimumX = effectiveScaleX * expectedCompressionX * expectedCompressionX;
        expectedMinimumY = effectiveScaleY * expectedCompressionY * expectedCompressionY;
    } else if (!NearlyEqual(layout.centerFractionX, rawFractionX) ||
               !NearlyEqual(layout.centerFractionY, rawFractionY) ||
               layout.centerOffsetX != 0.0f || layout.centerOffsetY != 0.0f) {
        return Status::InvalidAxis;
    }
    if (!NearlyEqual(layout.compressionXNeg, expectedSidesX[0]) ||
        !NearlyEqual(layout.compressionXPos, expectedSidesX[1]) ||
        !NearlyEqual(layout.compressionYNeg, expectedSidesY[0]) ||
        !NearlyEqual(layout.compressionYPos, expectedSidesY[1]) ||
        !NearlyEqual(layout.edgeSlopeXNeg, expectedSidesX[0] * expectedSidesX[0]) ||
        !NearlyEqual(layout.edgeSlopeXPos, expectedSidesX[1] * expectedSidesX[1]) ||
        !NearlyEqual(layout.edgeSlopeYNeg, expectedSidesY[0] * expectedSidesY[0]) ||
        !NearlyEqual(layout.edgeSlopeYPos, expectedSidesY[1] * expectedSidesY[1]))
        return Status::InvalidAxis;
    if (!NearlyEqual(layout.compressionX, expectedCompressionX) ||
        !NearlyEqual(layout.compressionY, expectedCompressionY) ||
        !NearlyEqual(layout.edgeSlopeX, expectedCompressionX * expectedCompressionX) ||
        !NearlyEqual(layout.edgeSlopeY, expectedCompressionY * expectedCompressionY) ||
        !NearlyEqual(layout.minimumLocalScaleX, expectedMinimumX) ||
        !NearlyEqual(layout.minimumLocalScaleY, expectedMinimumY) ||
        !NearlyEqual(layout.maximumSourceFootprintX, SafeReciprocal(expectedMinimumX)) ||
        !NearlyEqual(layout.maximumSourceFootprintY, SafeReciprocal(expectedMinimumY)) ||
        layout.diagnosticFlags != ExpectedDiagnostics(layout))
        return Status::InvalidAxis;
    return Status::Ok;
}

ShaderConstantsV2 BuildShaderConstants(const LayoutV2 &layout) noexcept
{
    ShaderConstantsV2 constants{};
    constants.nativeWidth = static_cast<float>(layout.nativeWidth);
    constants.nativeHeight = static_cast<float>(layout.nativeHeight);
    constants.workWidth = static_cast<float>(layout.workWidth);
    constants.workHeight = static_cast<float>(layout.workHeight);
    constants.centerFractionX = layout.centerFractionX;
    constants.centerFractionY = layout.centerFractionY;
    constants.workFractionX = layout.rawWorkFractionX;
    constants.workFractionY = layout.rawWorkFractionY;
    constants.compressionX = layout.compressionX;
    constants.compressionY = layout.compressionY;
    constants.edgeSlopeX = layout.edgeSlopeX;
    constants.edgeSlopeY = layout.edgeSlopeY;
    constants.mode = static_cast<std::uint32_t>(layout.mode);
    constants.colorFilter = static_cast<std::uint32_t>(layout.colorFilter);
    constants.flags = layout.flags;
    const detail::AxisSidesV2 sx = detail::AxisSides(layout, 0);
    const detail::AxisSidesV2 sy = detail::AxisSides(layout, 1);
    constants.bandCenterX = sx.bandCenter;
    constants.bandCenterY = sy.bandCenter;
    constants.workCenterX = sx.workCenter;
    constants.workCenterY = sy.workCenter;
    constants.halfSpanNegX = sx.halfSpan[0];
    constants.halfSpanNegY = sy.halfSpan[0];
    constants.sideCenterNegX = sx.center[0];
    constants.sideCenterNegY = sy.center[0];
    constants.halfSpanPosX = sx.halfSpan[1];
    constants.halfSpanPosY = sy.halfSpan[1];
    constants.sideCenterPosX = sx.center[1];
    constants.sideCenterPosY = sy.center[1];
    constants.sideWorkNegX = sx.work[0];
    constants.sideWorkNegY = sy.work[0];
    constants.sideCompressionNegX = sx.compression[0];
    constants.sideCompressionNegY = sy.compression[0];
    constants.sideWorkPosX = sx.work[1];
    constants.sideWorkPosY = sy.work[1];
    constants.sideCompressionPosX = sx.compression[1];
    constants.sideCompressionPosY = sy.compression[1];
    constants.sideEdgeSlopeNegX = sx.edgeSlope[0];
    constants.sideEdgeSlopeNegY = sy.edgeSlope[0];
    constants.sideEdgeSlopePosX = sx.edgeSlope[1];
    constants.sideEdgeSlopePosY = sy.edgeSlope[1];
    constants.workScaleX = sx.scale;
    constants.workScaleY = sy.scale;
    constants.centerOffsetX = layout.centerOffsetX;
    constants.centerOffsetY = layout.centerOffsetY;
    return constants;
}

Status UpgradeConfig(const ConfigV1 &source, ConfigV2 *destination) noexcept
{
    if (destination == nullptr) return Status::NullArgument;
    const Status status = ValidateConfig(source);
    if (status != Status::Ok) return status;
    ConfigV2 result{};
    result.structSize = sizeof(result);
    result.version = kAbiVersionV2;
    result.mode = source.mode;
    result.colorFilter = source.colorFilter;
    result.xAxis = source.xAxis;
    result.yAxis = source.yAxis;
    result.globalScalePercent = 100.0f;
    result.flags = source.flags;
    const Status v2Status = ValidateConfig(result);
    if (v2Status != Status::Ok) return v2Status;
    *destination = result;
    return Status::Ok;
}

Status DowngradeConfig(const ConfigV2 &source, ConfigV1 *destination) noexcept
{
    if (destination == nullptr) return Status::NullArgument;
    const Status status = ValidateConfig(source);
    if (status != Status::Ok) return status;
    if (!NearlyEqual(source.globalScalePercent, 100.0f)) return Status::InvalidAxis;
    if (source.mode == WarpMode::Peripheral &&
        (source.centerOffsetXPercent != 0.0f || source.centerOffsetYPercent != 0.0f ||
         source.workShiftXPercent != 0.0f || source.workShiftYPercent != 0.0f))
        return Status::InvalidAxis;
    ConfigV1 result{};
    result.structSize = sizeof(result);
    result.version = kAbiVersion;
    result.mode = source.mode;
    result.colorFilter = source.colorFilter;
    result.xAxis = source.xAxis;
    result.yAxis = source.yAxis;
    result.flags = source.flags;
    const Status legacyStatus = ValidateConfig(result);
    if (legacyStatus != Status::Ok) return legacyStatus;
    *destination = result;
    return Status::Ok;
}

Status UpgradeLayout(const LayoutV1 &source, LayoutV2 *destination) noexcept
{
    if (destination == nullptr) return Status::NullArgument;
    const Status status = ValidateLayout(source);
    if (status != Status::Ok) return status;
    LayoutV2 result{};
    result.structSize = sizeof(result);
    result.version = kAbiVersionV2;
    result.mode = source.mode;
    result.colorFilter = source.colorFilter;
    result.nativeWidth = source.nativeWidth;
    result.nativeHeight = source.nativeHeight;
    result.rawWorkWidth = source.workWidth;
    result.rawWorkHeight = source.workHeight;
    result.workWidth = source.workWidth;
    result.workHeight = source.workHeight;
    result.centerFractionX = source.centerFractionX;
    result.centerFractionY = source.centerFractionY;
    result.configuredWorkFractionX = source.workFractionX;
    result.configuredWorkFractionY = source.workFractionY;
    result.rawWorkFractionX = source.workFractionX;
    result.rawWorkFractionY = source.workFractionY;
    result.globalScalePercent = 100.0f;
    result.effectiveScaleX = 1.0f;
    result.effectiveScaleY = 1.0f;
    result.compressionX = source.compressionX;
    result.compressionY = source.compressionY;
    result.edgeSlopeX = source.edgeSlopeX;
    result.edgeSlopeY = source.edgeSlopeY;
    result.compressionXNeg = result.compressionXPos = source.compressionX;
    result.compressionYNeg = result.compressionYPos = source.compressionY;
    result.edgeSlopeXNeg = result.edgeSlopeXPos = source.edgeSlopeX;
    result.edgeSlopeYNeg = result.edgeSlopeYPos = source.edgeSlopeY;
    result.minimumLocalScaleX = source.mode == WarpMode::Peripheral
        ? source.edgeSlopeX : source.workFractionX;
    result.minimumLocalScaleY = source.mode == WarpMode::Peripheral
        ? source.edgeSlopeY : source.workFractionY;
    result.maximumSourceFootprintX = SafeReciprocal(result.minimumLocalScaleX);
    result.maximumSourceFootprintY = SafeReciprocal(result.minimumLocalScaleY);
    result.pixelPercent = 100.0f * source.workFractionX * source.workFractionY;
    result.flags = source.flags;
    result.diagnosticFlags = ExpectedDiagnostics(result);
    const Status v2Status = ValidateLayout(result);
    if (v2Status != Status::Ok) return v2Status;
    *destination = result;
    return Status::Ok;
}

Status DowngradeLayout(const LayoutV2 &source, LayoutV1 *destination) noexcept
{
    if (destination == nullptr) return Status::NullArgument;
    const Status status = ValidateLayout(source);
    if (status != Status::Ok) return status;
    if (!NearlyEqual(source.globalScalePercent, 100.0f) ||
        source.rawWorkWidth != source.workWidth ||
        source.rawWorkHeight != source.workHeight ||
        source.centerOffsetX != 0.0f || source.centerOffsetY != 0.0f ||
        !NearlyEqual(source.compressionXNeg, source.compressionXPos) ||
        !NearlyEqual(source.compressionYNeg, source.compressionYPos))
        return Status::InvalidAxis;
    LayoutV1 result{};
    result.structSize = sizeof(result);
    result.version = kAbiVersion;
    result.mode = source.mode;
    result.colorFilter = source.colorFilter;
    result.nativeWidth = source.nativeWidth;
    result.nativeHeight = source.nativeHeight;
    result.workWidth = source.workWidth;
    result.workHeight = source.workHeight;
    result.centerFractionX = source.centerFractionX;
    result.centerFractionY = source.centerFractionY;
    result.workFractionX = source.rawWorkFractionX;
    result.workFractionY = source.rawWorkFractionY;
    result.compressionX = source.compressionX;
    result.compressionY = source.compressionY;
    result.edgeSlopeX = source.edgeSlopeX;
    result.edgeSlopeY = source.edgeSlopeY;
    result.flags = source.flags;
    const Status legacyStatus = ValidateLayout(result);
    if (legacyStatus != Status::Ok) return legacyStatus;
    *destination = result;
    return Status::Ok;
}

} // namespace pw
