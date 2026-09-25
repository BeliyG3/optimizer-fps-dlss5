#include "optimizer_fps/types_v2.h"

#include "sides_v2.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>


namespace ofps::sdk::detail {

bool IsKnownMode(WarpMode mode) noexcept
{
    return mode == WarpMode::Off || mode == WarpMode::Uniform ||
           mode == WarpMode::Peripheral;
}

bool IsKnownFilter(ColorFilter filter) noexcept
{
    return filter == ColorFilter::Bilinear || filter == ColorFilter::AdaptiveFourTap;
}

} // namespace ofps::sdk::detail

namespace ofps::sdk {
using detail::IsKnownMode;
using detail::IsKnownFilter;
using detail::EvenExtent;
using detail::SidesConsistent;
using detail::SafeReciprocal;
using detail::ExpectedDiagnostics;
using detail::MaximumCenterOffsetPercent;

namespace {
constexpr std::uint32_t kKnownConfigFlags = ConfigFlagExtendMotionAtEdge | ConfigFlagInputConfidenceValid;
constexpr float kPercentEpsilon = 1.0e-4f;
constexpr std::uint32_t kKnownDiagnosticFlags =
    LayoutDiagnosticAggressivePeripheralX | LayoutDiagnosticAggressivePeripheralY |
    LayoutDiagnosticAliasingRiskX | LayoutDiagnosticAliasingRiskY;
bool IsFinite(float value) noexcept { return std::isfinite(value); }
bool NearlyEqual(float lhs, float rhs, float relativeTolerance = 1.0e-5f) noexcept
{
    const float scale = std::max({1.0f, std::abs(lhs), std::abs(rhs)});
    return std::abs(lhs - rhs) <= relativeTolerance * scale;
}

} // namespace

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

} // namespace ofps::sdk
