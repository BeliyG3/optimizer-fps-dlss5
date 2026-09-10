#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace pw {

inline constexpr std::uint32_t kAbiVersion = 1;

enum class WarpMode : std::uint32_t {
    Off = 0,
    Uniform = 1,
    Peripheral = 2,
};

enum class ColorFilter : std::uint32_t {
    Bilinear = 0,
    AdaptiveFourTap = 1,
};

enum class Status : std::uint32_t {
    Ok = 0,
    NullArgument,
    StructSizeMismatch,
    VersionMismatch,
    InvalidDimensions,
    InvalidMode,
    InvalidFilter,
    InvalidAxis,
    NotFound,
    NotReady,
    UnsupportedBackend,
    InvalidFrame,
    InvalidFlags,
};

enum ConfigFlags : std::uint32_t {
    ConfigFlagNone = 0,
    ConfigFlagExtendMotionAtEdge = 1u << 0,
    ConfigFlagInputConfidenceValid = 1u << 1,
};

struct AxisConfig {
    float centerPercent;
    float workPercent;
};

struct ConfigV1 {
    std::uint32_t structSize;
    std::uint32_t version;
    WarpMode mode;
    ColorFilter colorFilter;
    AxisConfig xAxis;
    AxisConfig yAxis;
    std::uint32_t flags;
    std::uint32_t reserved[7];
};

struct LayoutV1 {
    std::uint32_t structSize;
    std::uint32_t version;
    WarpMode mode;
    ColorFilter colorFilter;
    std::uint32_t nativeWidth;
    std::uint32_t nativeHeight;
    std::uint32_t workWidth;
    std::uint32_t workHeight;
    float centerFractionX;
    float centerFractionY;
    float workFractionX;
    float workFractionY;
    float compressionX;
    float compressionY;
    float edgeSlopeX;
    float edgeSlopeY;
    std::uint32_t flags;
    std::uint32_t reserved[3];
};

struct Float2 {
    float x;
    float y;
};

struct alignas(16) ShaderConstantsV1 {
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
};

[[nodiscard]] ConfigV1 DefaultConfigV1() noexcept;
[[nodiscard]] Status ValidateConfig(const ConfigV1 &config) noexcept;
[[nodiscard]] Status ValidateLayout(const LayoutV1 &layout) noexcept;
[[nodiscard]] Status BuildLayout(
    const ConfigV1 &config,
    std::uint32_t nativeWidth,
    std::uint32_t nativeHeight,
    LayoutV1 *layout) noexcept;
[[nodiscard]] ShaderConstantsV1 BuildShaderConstants(const LayoutV1 &layout) noexcept;
[[nodiscard]] const char *StatusString(Status status) noexcept;

static_assert(sizeof(AxisConfig) == 8);
static_assert(sizeof(ConfigV1) == 64);
static_assert(sizeof(LayoutV1) == 80);
static_assert(sizeof(ShaderConstantsV1) == 64);
static_assert(alignof(ShaderConstantsV1) == 16);
static_assert(offsetof(ConfigV1, mode) == 8);
static_assert(offsetof(ConfigV1, xAxis) == 16);
static_assert(offsetof(ConfigV1, flags) == 32);
static_assert(offsetof(ConfigV1, reserved) == 36);
static_assert(offsetof(LayoutV1, nativeWidth) == 16);
static_assert(offsetof(LayoutV1, centerFractionX) == 32);
static_assert(offsetof(LayoutV1, compressionX) == 48);
static_assert(offsetof(LayoutV1, flags) == 64);
static_assert(offsetof(ShaderConstantsV1, nativeWidth) == 0);
static_assert(offsetof(ShaderConstantsV1, centerFractionX) == 16);
static_assert(offsetof(ShaderConstantsV1, compressionX) == 32);
static_assert(offsetof(ShaderConstantsV1, mode) == 48);
static_assert(std::is_standard_layout_v<ConfigV1> && std::is_trivially_copyable_v<ConfigV1>);
static_assert(std::is_standard_layout_v<LayoutV1> && std::is_trivially_copyable_v<LayoutV1>);

} // namespace pw
