#include "optimizer_fps/types_v2.h"

#include "sides_v2.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>

namespace ofps::sdk {
using detail::IsKnownMode;
using detail::IsKnownFilter;
using detail::MaximumCenterOffsetPercent;
using detail::WorkShiftLimitsFraction;
namespace {
constexpr std::uint32_t kKnownConfigFlags =
    ConfigFlagExtendMotionAtEdge | ConfigFlagInputConfidenceValid;
constexpr float kPercentEpsilon = 1.0e-4f;

bool IsFinite(float value) noexcept { return std::isfinite(value); }

template <std::size_t Size>
bool AllZero(const std::uint32_t (&values)[Size]) noexcept
{
    return std::all_of(std::begin(values), std::end(values),
                       [](std::uint32_t value) { return value == 0; });
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

bool OffsetIsValid(float offsetPercent, const AxisConfig &axis, WarpMode mode) noexcept
{
    if (!IsFinite(offsetPercent)) return false;
    if (mode != WarpMode::Peripheral) return true;
    return std::abs(offsetPercent) <= MaximumCenterOffsetPercent(axis.centerPercent) + kPercentEpsilon;
}

// Work-shift bounds from the configuration's fractions (no quantisation; BuildLayout clamps the
// shift to the exact pixel bounds anyway).
bool WorkShiftIsValid(float shiftPercent, const AxisConfig &axis, float offsetPercent, WarpMode mode) noexcept
{
    if (!IsFinite(shiftPercent)) return false;
    if (mode != WarpMode::Peripheral) return true;
    float lo = 0.0f, hi = 0.0f;
    WorkShiftLimitsFraction(axis, offsetPercent * 0.01f, &lo, &hi);
    return shiftPercent * 0.01f >= lo - kPercentEpsilon && shiftPercent * 0.01f <= hi + kPercentEpsilon;
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


} // namespace ofps::sdk
