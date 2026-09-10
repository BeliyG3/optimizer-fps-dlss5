#include "peripheral_warp/types.h"

#include <algorithm>
#include <cmath>
#include <iterator>

namespace pw {
namespace {

constexpr std::uint32_t kKnownConfigFlags =
    ConfigFlagExtendMotionAtEdge | ConfigFlagInputConfidenceValid;
constexpr float kAxisEpsilonPercent = 1.0e-4f;

bool IsFinite(float value) noexcept { return std::isfinite(value); }

bool IsKnownMode(WarpMode mode) noexcept {
    return mode == WarpMode::Off || mode == WarpMode::Uniform || mode == WarpMode::Peripheral;
}

bool IsKnownFilter(ColorFilter filter) noexcept {
    return filter == ColorFilter::Bilinear || filter == ColorFilter::AdaptiveFourTap;
}

bool AxisIsValid(const AxisConfig &axis) noexcept {
    if (!IsFinite(axis.centerPercent) || !IsFinite(axis.workPercent)) return false;
    if (axis.centerPercent <= 0.0f || axis.centerPercent > 100.0f) return false;
    if (axis.workPercent <= 0.0f || axis.workPercent > 100.0f) return false;
    if (axis.centerPercent > axis.workPercent) return false;
    if (axis.workPercent + kAxisEpsilonPercent <
        (100.0f + axis.centerPercent) * 0.5f) return false;
    return true;
}

bool NearlyEqual(float lhs, float rhs) noexcept {
    const float scale = std::max({1.0f, std::abs(lhs), std::abs(rhs)});
    return std::abs(lhs - rhs) <= 1.0e-5f * scale;
}

template <std::size_t Size>
bool AllZero(const std::uint32_t (&values)[Size]) noexcept {
    return std::all_of(std::begin(values), std::end(values),
                       [](std::uint32_t value) { return value == 0; });
}

std::uint32_t EvenExtent(std::uint32_t nativeExtent, float fraction) noexcept {
    if (fraction >= 1.0f) return nativeExtent;
    auto extent = static_cast<std::uint32_t>(std::ceil(static_cast<double>(nativeExtent) * fraction));
    if ((extent & 1u) != 0) ++extent;
    extent = std::max<std::uint32_t>(2, extent);
    const std::uint32_t largestEven = nativeExtent & ~1u;
    return std::min(extent, largestEven);
}

} // namespace

ConfigV1 DefaultConfigV1() noexcept {
    ConfigV1 config{};
    config.structSize = sizeof(ConfigV1);
    config.version = kAbiVersion;
    config.mode = WarpMode::Peripheral;
    config.colorFilter = ColorFilter::AdaptiveFourTap;
    config.xAxis = {80.0f, 90.0f};
    config.yAxis = {80.0f, 90.0f};
    config.flags = ConfigFlagExtendMotionAtEdge;
    return config;
}

Status ValidateConfig(const ConfigV1 &config) noexcept {
    if (config.structSize != sizeof(ConfigV1)) return Status::StructSizeMismatch;
    if (config.version != kAbiVersion) return Status::VersionMismatch;
    if (!IsKnownMode(config.mode)) return Status::InvalidMode;
    if (!IsKnownFilter(config.colorFilter)) return Status::InvalidFilter;
    if ((config.flags & ~kKnownConfigFlags) != 0 || !AllZero(config.reserved))
        return Status::InvalidFlags;
    if (config.mode == WarpMode::Off) return Status::Ok;
    if (config.mode == WarpMode::Uniform) {
        if (!IsFinite(config.xAxis.workPercent) || !IsFinite(config.yAxis.workPercent)) return Status::InvalidAxis;
        if (config.xAxis.workPercent <= 0.0f || config.xAxis.workPercent > 100.0f ||
            config.yAxis.workPercent <= 0.0f || config.yAxis.workPercent > 100.0f) return Status::InvalidAxis;
        return Status::Ok;
    }
    return AxisIsValid(config.xAxis) && AxisIsValid(config.yAxis) ? Status::Ok : Status::InvalidAxis;
}

Status ValidateLayout(const LayoutV1 &layout) noexcept {
    if (layout.structSize != sizeof(LayoutV1)) return Status::StructSizeMismatch;
    if (layout.version != kAbiVersion) return Status::VersionMismatch;
    if (!IsKnownMode(layout.mode)) return Status::InvalidMode;
    if (!IsKnownFilter(layout.colorFilter)) return Status::InvalidFilter;
    if ((layout.flags & ~kKnownConfigFlags) != 0 || !AllZero(layout.reserved))
        return Status::InvalidFlags;
    if (layout.nativeWidth < 2 || layout.nativeHeight < 2 ||
        layout.workWidth < 2 || layout.workHeight < 2 ||
        layout.workWidth > layout.nativeWidth || layout.workHeight > layout.nativeHeight)
        return Status::InvalidDimensions;

    const float values[] = {
        layout.centerFractionX, layout.centerFractionY,
        layout.workFractionX, layout.workFractionY,
        layout.compressionX, layout.compressionY,
        layout.edgeSlopeX, layout.edgeSlopeY};
    if (!std::all_of(std::begin(values), std::end(values), IsFinite))
        return Status::InvalidAxis;

    const float actualWorkX = static_cast<float>(layout.workWidth) /
                              static_cast<float>(layout.nativeWidth);
    const float actualWorkY = static_cast<float>(layout.workHeight) /
                              static_cast<float>(layout.nativeHeight);
    if (!NearlyEqual(layout.workFractionX, actualWorkX) ||
        !NearlyEqual(layout.workFractionY, actualWorkY))
        return Status::InvalidDimensions;

    if (layout.mode == WarpMode::Off) {
        if (layout.workWidth != layout.nativeWidth || layout.workHeight != layout.nativeHeight)
            return Status::InvalidDimensions;
        return NearlyEqual(layout.centerFractionX, 1.0f) &&
                       NearlyEqual(layout.centerFractionY, 1.0f) &&
                       NearlyEqual(layout.workFractionX, 1.0f) &&
                       NearlyEqual(layout.workFractionY, 1.0f) &&
                       NearlyEqual(layout.compressionX, 1.0f) &&
                       NearlyEqual(layout.compressionY, 1.0f) &&
                       NearlyEqual(layout.edgeSlopeX, 1.0f) &&
                       NearlyEqual(layout.edgeSlopeY, 1.0f)
                   ? Status::Ok : Status::InvalidAxis;
    }

    if ((layout.workWidth < layout.nativeWidth && (layout.workWidth & 1u) != 0) ||
        (layout.workHeight < layout.nativeHeight && (layout.workHeight & 1u) != 0))
        return Status::InvalidDimensions;

    if (layout.mode == WarpMode::Uniform) {
        const float compressionX = actualWorkX < 1.0f ? 0.0f : 1.0f;
        const float compressionY = actualWorkY < 1.0f ? 0.0f : 1.0f;
        return NearlyEqual(layout.centerFractionX, actualWorkX) &&
                       NearlyEqual(layout.centerFractionY, actualWorkY) &&
                       NearlyEqual(layout.compressionX, compressionX) &&
                       NearlyEqual(layout.compressionY, compressionY) &&
                       NearlyEqual(layout.edgeSlopeX, compressionX * compressionX) &&
                       NearlyEqual(layout.edgeSlopeY, compressionY * compressionY)
                   ? Status::Ok : Status::InvalidAxis;
    }

    auto validPeripheralAxis = [](float center, float work, float compression, float edgeSlope) {
        if (center <= 0.0f || center > 1.0f || work <= 0.0f || work > 1.0f || center > work)
            return false;
        if (work + 1.0e-6f < (1.0f + center) * 0.5f) return false;
        const float expectedCompression = center < 1.0f
            ? (work - center) / (1.0f - center) : 1.0f;
        return NearlyEqual(compression, expectedCompression) &&
               NearlyEqual(edgeSlope, expectedCompression * expectedCompression);
    };
    return validPeripheralAxis(layout.centerFractionX, actualWorkX,
                               layout.compressionX, layout.edgeSlopeX) &&
                   validPeripheralAxis(layout.centerFractionY, actualWorkY,
                                       layout.compressionY, layout.edgeSlopeY)
               ? Status::Ok : Status::InvalidAxis;
}

Status BuildLayout(const ConfigV1 &config, std::uint32_t nativeWidth, std::uint32_t nativeHeight, LayoutV1 *layout) noexcept {
    if (layout == nullptr) return Status::NullArgument;
    const Status status = ValidateConfig(config);
    if (status != Status::Ok) return status;
    if (nativeWidth < 2 || nativeHeight < 2) return Status::InvalidDimensions;

    float workX = 1.0f;
    float workY = 1.0f;
    float centerX = 1.0f;
    float centerY = 1.0f;
    if (config.mode == WarpMode::Uniform) {
        workX = config.xAxis.workPercent * 0.01f;
        workY = config.yAxis.workPercent * 0.01f;
        centerX = workX;
        centerY = workY;
    } else if (config.mode == WarpMode::Peripheral) {
        workX = config.xAxis.workPercent * 0.01f;
        workY = config.yAxis.workPercent * 0.01f;
        centerX = config.xAxis.centerPercent * 0.01f;
        centerY = config.yAxis.centerPercent * 0.01f;
    }

    const std::uint32_t workWidth = config.mode == WarpMode::Off ? nativeWidth : EvenExtent(nativeWidth, workX);
    const std::uint32_t workHeight = config.mode == WarpMode::Off ? nativeHeight : EvenExtent(nativeHeight, workY);
    const float actualWorkX = static_cast<float>(workWidth) / static_cast<float>(nativeWidth);
    const float actualWorkY = static_cast<float>(workHeight) / static_cast<float>(nativeHeight);
    if (config.mode == WarpMode::Peripheral &&
        (actualWorkX + 1.0e-6f < (1.0f + centerX) * 0.5f ||
         actualWorkY + 1.0e-6f < (1.0f + centerY) * 0.5f))
        return Status::InvalidAxis;

    LayoutV1 result{};
    result.structSize = sizeof(LayoutV1);
    result.version = kAbiVersion;
    result.mode = config.mode;
    result.colorFilter = config.colorFilter;
    result.nativeWidth = nativeWidth;
    result.nativeHeight = nativeHeight;
    result.workWidth = workWidth;
    result.workHeight = workHeight;
    result.centerFractionX = config.mode == WarpMode::Peripheral ? centerX : actualWorkX;
    result.centerFractionY = config.mode == WarpMode::Peripheral ? centerY : actualWorkY;
    result.workFractionX = actualWorkX;
    result.workFractionY = actualWorkY;
    result.compressionX = (1.0f - result.centerFractionX) > 0.0f
        ? (result.workFractionX - result.centerFractionX) / (1.0f - result.centerFractionX) : 1.0f;
    result.compressionY = (1.0f - result.centerFractionY) > 0.0f
        ? (result.workFractionY - result.centerFractionY) / (1.0f - result.centerFractionY) : 1.0f;
    result.edgeSlopeX = result.compressionX * result.compressionX;
    result.edgeSlopeY = result.compressionY * result.compressionY;
    result.flags = config.flags;
    *layout = result;
    return Status::Ok;
}

ShaderConstantsV1 BuildShaderConstants(const LayoutV1 &layout) noexcept {
    ShaderConstantsV1 constants{};
    constants.nativeWidth = static_cast<float>(layout.nativeWidth);
    constants.nativeHeight = static_cast<float>(layout.nativeHeight);
    constants.workWidth = static_cast<float>(layout.workWidth);
    constants.workHeight = static_cast<float>(layout.workHeight);
    constants.centerFractionX = layout.centerFractionX;
    constants.centerFractionY = layout.centerFractionY;
    constants.workFractionX = layout.workFractionX;
    constants.workFractionY = layout.workFractionY;
    constants.compressionX = layout.compressionX;
    constants.compressionY = layout.compressionY;
    constants.edgeSlopeX = layout.edgeSlopeX;
    constants.edgeSlopeY = layout.edgeSlopeY;
    constants.mode = static_cast<std::uint32_t>(layout.mode);
    constants.colorFilter = static_cast<std::uint32_t>(layout.colorFilter);
    constants.flags = layout.flags;
    return constants;
}

const char *StatusString(Status status) noexcept {
    switch (status) {
    case Status::Ok: return "ok";
    case Status::NullArgument: return "null argument";
    case Status::StructSizeMismatch: return "structure size mismatch";
    case Status::VersionMismatch: return "ABI version mismatch";
    case Status::InvalidDimensions: return "invalid dimensions";
    case Status::InvalidMode: return "invalid warp mode";
    case Status::InvalidFilter: return "invalid color filter";
    case Status::InvalidAxis: return "invalid axis configuration";
    case Status::NotFound: return "not found";
    case Status::NotReady: return "not ready";
    case Status::UnsupportedBackend: return "unsupported backend";
    case Status::InvalidFrame: return "invalid frame";
    case Status::InvalidFlags: return "invalid or nonzero reserved flags";
    }
    return "unknown status";
}

} // namespace pw
