#pragma once

#include "peripheral_warp/types.h"

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace pw {

inline constexpr std::uint32_t kAbiVersionV2 = 2;
inline constexpr float kMinimumWorkPercentV2 = 25.0f;
inline constexpr float kMinimumEffectiveScalePercentV2 = 25.0f;
inline constexpr float kAggressivePeripheralCompressionV2 = 0.5f;
// A centre offset may move the 1:1 band up to the frame edge minus this much periphery (percent
// of the axis) on the narrow side: |offset| <= (100 - center) / 2 - margin.
inline constexpr float kMinimumSidePeripheryPercentV2 = 0.5f;

enum LayoutDiagnosticFlagsV2 : std::uint32_t {
    LayoutDiagnosticNone = 0,
    LayoutDiagnosticAggressivePeripheralX = 1u << 0,
    LayoutDiagnosticAggressivePeripheralY = 1u << 1,
    LayoutDiagnosticAliasingRiskX = 1u << 2,
    LayoutDiagnosticAliasingRiskY = 1u << 3,
};

// ABI v2 configuration. The v1 structure remains binary- and behavior-compatible;
// the wider Work range and Global scale are intentionally available only here.
struct ConfigV2 {
    std::uint32_t structSize;
    std::uint32_t version;
    WarpMode mode;
    ColorFilter colorFilter;
    AxisConfig xAxis;
    AxisConfig yAxis;
    float globalScalePercent;
    std::uint32_t flags;
    // Signed offset of the 1:1 centre band from the frame centre, percent of the axis
    // (Peripheral mode only; ignored by Uniform/Off). Work stays the same size: the wider
    // periphery is compressed harder, the narrower one never below 1:1.
    float centerOffsetXPercent;
    float centerOffsetYPercent;
    // Signed shift of the raw Work rectangle (the outer contour) along each axis, percent of the
    // axis, while the centre band stays: work pixels move from one periphery to the other, so the
    // side the contour moves towards is compressed less and the opposite side more. Peripheral
    // only; bounded by WorkShiftLimitsPercentV2.
    float workShiftXPercent;
    float workShiftYPercent;
    std::uint32_t reserved[2];
};

// rawWork* describes Peripheral/Uniform Work before Global scale. work* is the
// actual NR input. configuredWorkFraction* preserves the requested value while
// rawWorkFraction* records the exact fraction after ceil-to-even quantization.
struct LayoutV2 {
    std::uint32_t structSize;
    std::uint32_t version;
    WarpMode mode;
    ColorFilter colorFilter;
    std::uint32_t nativeWidth;
    std::uint32_t nativeHeight;
    std::uint32_t rawWorkWidth;
    std::uint32_t rawWorkHeight;
    std::uint32_t workWidth;
    std::uint32_t workHeight;
    float centerFractionX;
    float centerFractionY;
    float configuredWorkFractionX;
    float configuredWorkFractionY;
    float rawWorkFractionX;
    float rawWorkFractionY;
    float globalScalePercent;
    float effectiveScaleX;
    float effectiveScaleY;
    float compressionX;
    float compressionY;
    float edgeSlopeX;
    float edgeSlopeY;
    float minimumLocalScaleX;
    float minimumLocalScaleY;
    float maximumSourceFootprintX;
    float maximumSourceFootprintY;
    float pixelPercent;
    std::uint32_t flags;
    std::uint32_t diagnosticFlags;
    // Centre offset as a fraction of the axis (0 outside Peripheral) and the per-side curve
    // parameters it produces. compressionX/edgeSlopeX above hold the worse (smaller) side.
    float centerOffsetX;
    float centerOffsetY;
    float compressionXNeg;
    float compressionXPos;
    float compressionYNeg;
    float compressionYPos;
    float edgeSlopeXNeg;
    float edgeSlopeXPos;
    float edgeSlopeYNeg;
    float edgeSlopeYPos;
};

// Shader constants with the per-side mapping (176 bytes). The first 64 bytes are the v1 block,
// so a consumer reading only those still sees the symmetric values.
struct alignas(16) ShaderConstantsV3 {
    float nativeWidth;
    float nativeHeight;
    float workWidth;
    float workHeight;
    float centerFractionX;
    float centerFractionY;
    float workFractionX;
    float workFractionY;
    float compressionX;
    float compressionY;
    float edgeSlopeX;
    float edgeSlopeY;
    std::uint32_t mode;
    std::uint32_t colorFilter;
    std::uint32_t flags;
    std::uint32_t reserved;
    float bandCenterX;        // PwBandCenter: band centre (native px) X/Y, work centre (work px) X/Y
    float bandCenterY;
    float workCenterX;
    float workCenterY;
    float halfSpanNegX;       // PwSideNeg: half-span X/Y, centre fraction X/Y (negative side)
    float halfSpanNegY;
    float sideCenterNegX;
    float sideCenterNegY;
    float halfSpanPosX;       // PwSidePos
    float halfSpanPosY;
    float sideCenterPosX;
    float sideCenterPosY;
    float sideWorkNegX;       // PwSideWorkNeg: work fraction X/Y, compression X/Y (negative side)
    float sideWorkNegY;
    float sideCompressionNegX;
    float sideCompressionNegY;
    float sideWorkPosX;       // PwSideWorkPos
    float sideWorkPosY;
    float sideCompressionPosX;
    float sideCompressionPosY;
    float sideEdgeSlopeNegX;  // PwSideEdgeSlope: negative X/Y, positive X/Y
    float sideEdgeSlopeNegY;
    float sideEdgeSlopePosX;
    float sideEdgeSlopePosY;
    float workScaleX;         // PwWorkScale: work px per native-scaled px X/Y, centre offset X/Y
    float workScaleY;
    float centerOffsetX;
    float centerOffsetY;
};

using ShaderConstantsV2 = ShaderConstantsV3;

[[nodiscard]] ConfigV2 DefaultConfigV2() noexcept;
[[nodiscard]] Status ValidateConfig(const ConfigV2 &config) noexcept;
[[nodiscard]] Status ValidateLayout(const LayoutV2 &layout) noexcept;
[[nodiscard]] Status BuildLayout(
    const ConfigV2 &config,
    std::uint32_t nativeWidth,
    std::uint32_t nativeHeight,
    LayoutV2 *layout) noexcept;
[[nodiscard]] ShaderConstantsV2 BuildShaderConstants(const LayoutV2 &layout) noexcept;
[[nodiscard]] Status UpgradeConfig(const ConfigV1 &source, ConfigV2 *destination) noexcept;
// Downgrade succeeds only when Global scale is 100% and the v2 axes satisfy the
// historical v1 compression guard.
[[nodiscard]] Status DowngradeConfig(const ConfigV2 &source, ConfigV1 *destination) noexcept;
[[nodiscard]] Status UpgradeLayout(const LayoutV1 &source, LayoutV2 *destination) noexcept;
[[nodiscard]] Status DowngradeLayout(const LayoutV2 &source, LayoutV1 *destination) noexcept;

// Returns the dynamic lower bound for ConfigV2::globalScalePercent. A non-finite
// or otherwise invalid Work value returns 100, leaving validation fail-closed.
[[nodiscard]] float MinimumGlobalScalePercent(const ConfigV2 &config) noexcept;
// Largest |centerOffset*Percent| accepted for a given centerPercent in Peripheral mode.
[[nodiscard]] float MaximumCenterOffsetPercentV2(float centerPercent) noexcept;
// Accepted range of ConfigV2::workShift{X,Y}Percent (axis 0 = X, 1 = Y) for the configuration's
// centre, work and offset values; both bounds are 0 when nothing can move.
void WorkShiftLimitsPercentV2(const ConfigV2 &config, std::uint32_t axis, float *minPercent,
                              float *maxPercent) noexcept;

static_assert(sizeof(ConfigV2) == 64);
static_assert(sizeof(LayoutV2) == 160);
static_assert(offsetof(ConfigV2, mode) == 8);
static_assert(offsetof(ConfigV2, xAxis) == 16);
static_assert(offsetof(ConfigV2, globalScalePercent) == 32);
static_assert(offsetof(ConfigV2, flags) == 36);
static_assert(offsetof(LayoutV2, nativeWidth) == 16);
static_assert(offsetof(LayoutV2, rawWorkWidth) == 24);
static_assert(offsetof(LayoutV2, workWidth) == 32);
static_assert(offsetof(LayoutV2, centerFractionX) == 40);
static_assert(offsetof(LayoutV2, globalScalePercent) == 64);
static_assert(offsetof(LayoutV2, compressionX) == 76);
static_assert(offsetof(LayoutV2, pixelPercent) == 108);
static_assert(offsetof(LayoutV2, flags) == 112);
static_assert(offsetof(LayoutV2, diagnosticFlags) == 116);
static_assert(offsetof(ConfigV2, centerOffsetXPercent) == 40);
static_assert(offsetof(ConfigV2, workShiftXPercent) == 48);
static_assert(offsetof(LayoutV2, centerOffsetX) == 120);
static_assert(sizeof(ShaderConstantsV3) == 176);
static_assert(alignof(ShaderConstantsV3) == 16);
static_assert(offsetof(ShaderConstantsV3, mode) == 48);
static_assert(offsetof(ShaderConstantsV3, bandCenterX) == 64);
static_assert(offsetof(ShaderConstantsV3, workScaleX) == 160);
static_assert(std::is_standard_layout_v<ShaderConstantsV3> && std::is_trivially_copyable_v<ShaderConstantsV3>);
static_assert(std::is_standard_layout_v<ConfigV2> && std::is_trivially_copyable_v<ConfigV2>);
static_assert(std::is_standard_layout_v<LayoutV2> && std::is_trivially_copyable_v<LayoutV2>);

} // namespace pw
