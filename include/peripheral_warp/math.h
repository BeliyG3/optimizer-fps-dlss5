#pragma once

#include "peripheral_warp/types.h"
#include "peripheral_warp/types_v2.h"

namespace pw {

// Coordinates are continuous pixel-center coordinates: the first texel center is 0.5.
[[nodiscard]] float PackRadius(float radius, float centerFraction, float workFraction) noexcept;
[[nodiscard]] float UnpackRadius(float packedRadius, float centerFraction, float workFraction) noexcept;
[[nodiscard]] float PackCoordinate(float nativePixel, std::uint32_t axis, const LayoutV1 &layout) noexcept;
[[nodiscard]] float UnpackCoordinate(float workPixel, std::uint32_t axis, const LayoutV1 &layout) noexcept;
[[nodiscard]] Float2 PackPosition(Float2 nativePixel, const LayoutV1 &layout) noexcept;
[[nodiscard]] Float2 UnpackPosition(Float2 workPixel, const LayoutV1 &layout) noexcept;
[[nodiscard]] Float2 PackMotion(Float2 nativeCurrentPixel, Float2 nativeMotionPixels, const LayoutV1 &layout) noexcept;
[[nodiscard]] Float2 UnpackMotion(Float2 workCurrentPixel, Float2 workMotionPixels, const LayoutV1 &layout) noexcept;
[[nodiscard]] float PackCoordinate(float nativePixel, std::uint32_t axis, const LayoutV2 &layout) noexcept;
[[nodiscard]] float UnpackCoordinate(float workPixel, std::uint32_t axis, const LayoutV2 &layout) noexcept;
[[nodiscard]] Float2 PackPosition(Float2 nativePixel, const LayoutV2 &layout) noexcept;
[[nodiscard]] Float2 UnpackPosition(Float2 workPixel, const LayoutV2 &layout) noexcept;
[[nodiscard]] Float2 PackMotion(Float2 nativeCurrentPixel, Float2 nativeMotionPixels, const LayoutV2 &layout) noexcept;
[[nodiscard]] Float2 UnpackMotion(Float2 workCurrentPixel, Float2 workMotionPixels, const LayoutV2 &layout) noexcept;

} // namespace pw
