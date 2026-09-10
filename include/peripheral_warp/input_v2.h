#pragma once

#include "peripheral_warp/types.h"

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace pw {

inline constexpr std::uint32_t kInputDescriptionVersion = 2;

enum class MotionDirection : std::uint32_t {
    CurrentToPrevious = 0,
    PreviousToCurrent = 1,
};

enum class DepthConvention : std::uint32_t {
    Normal = 0,
    Reversed = 1,
};

enum class ColorEncoding : std::uint32_t {
    Unknown = 0,
    LinearLdr = 1,
    LinearHdr = 2,
    Srgb = 3,
    Pq = 4,
};

enum InputFlagsV2 : std::uint32_t {
    InputFlagNone = 0,
    InputFlagConfidenceValid = 1u << 0,
    InputFlagMotionJittered = 1u << 1,
};

struct RectU32 {
    std::uint32_t x;
    std::uint32_t y;
    std::uint32_t width;
    std::uint32_t height;
};

// Describes the semantics supplied by a standard DLSS-style input provider.
// motionScale converts stored vector components directly to native-output pixels.
// The adapter never guesses a format, sign, unit, depth convention, or colour encoding.
struct InputDescriptionV2 {
    std::uint32_t structSize;
    std::uint32_t version;
    RectU32 colorRect;
    RectU32 depthRect;
    RectU32 motionRect;
    RectU32 confidenceRect;
    float motionScaleX;
    float motionScaleY;
    float jitterX;
    float jitterY;
    MotionDirection motionDirection;
    DepthConvention depthConvention;
    ColorEncoding colorEncoding;
    std::uint32_t flags;
    std::uint32_t reserved[5];
};

struct InputResourceExtentsV2 {
    std::uint32_t colorWidth;
    std::uint32_t colorHeight;
    std::uint32_t depthWidth;
    std::uint32_t depthHeight;
    std::uint32_t motionWidth;
    std::uint32_t motionHeight;
    std::uint32_t confidenceWidth;
    std::uint32_t confidenceHeight;
};

// Mirrors cbuffer PwInputDescription at b1. Rects and extents are floats so the
// same blob can be consumed by DXBC and SPIR-V without backend-specific packing.
struct alignas(16) ShaderInputConstantsV2 {
    float colorRect[4];
    float depthRect[4];
    float motionRect[4];
    float confidenceRect[4];
    float colorDepthSize[4];
    float motionConfidenceSize[4];
    float motionScaleX;
    float motionScaleY;
    float motionDirectionSign;
    float flags;
};

[[nodiscard]] InputDescriptionV2 DefaultInputDescriptionV2(
    std::uint32_t nativeWidth, std::uint32_t nativeHeight) noexcept;
[[nodiscard]] Status ValidateInputDescriptionV2(const InputDescriptionV2 &description) noexcept;
[[nodiscard]] Status BuildShaderInputConstantsV2(
    const InputDescriptionV2 &description,
    const InputResourceExtentsV2 &extents,
    ShaderInputConstantsV2 *constants) noexcept;

static_assert(sizeof(RectU32) == 16);
static_assert(sizeof(InputDescriptionV2) == 124);
static_assert(sizeof(InputResourceExtentsV2) == 32);
static_assert(sizeof(ShaderInputConstantsV2) == 112);
static_assert(alignof(ShaderInputConstantsV2) == 16);
static_assert(std::is_standard_layout_v<InputDescriptionV2> &&
              std::is_trivially_copyable_v<InputDescriptionV2>);
static_assert(std::is_standard_layout_v<ShaderInputConstantsV2> &&
              std::is_trivially_copyable_v<ShaderInputConstantsV2>);

} // namespace pw
