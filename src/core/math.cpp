#include "peripheral_warp/math.h"

#include "sides_v2.h"

#include <algorithm>
#include <cmath>

namespace pw {
namespace {

float Sign(float value) noexcept { return value < 0.0f ? -1.0f : 1.0f; }

float ExtendPack(float radius, float center, float work) noexcept {
    const float edge = PackRadius(1.0f, center, work);
    const float c = (work - center) / (1.0f - center);
    const float edgeSlope = c * c;
    return edge + (radius - 1.0f) * edgeSlope;
}

float ExtendUnpack(float packedRadius, float center, float work) noexcept {
    const float c = (work - center) / (1.0f - center);
    const float edgeSlope = c * c;
    return 1.0f + (packedRadius - work) / edgeSlope;
}

void AxisData(std::uint32_t axis, const LayoutV1 &layout, float &nativeExtent, float &workExtent,
              float &center, float &work) noexcept {
    if (axis == 0) {
        nativeExtent = static_cast<float>(layout.nativeWidth);
        workExtent = static_cast<float>(layout.workWidth);
        center = layout.centerFractionX;
        work = layout.workFractionX;
    } else {
        nativeExtent = static_cast<float>(layout.nativeHeight);
        workExtent = static_cast<float>(layout.workHeight);
        center = layout.centerFractionY;
        work = layout.workFractionY;
    }
}

void AxisData(std::uint32_t axis, const LayoutV2 &layout, float &nativeExtent, float &workExtent,
              float &center, float &work) noexcept {
    if (axis == 0) {
        nativeExtent = static_cast<float>(layout.nativeWidth);
        workExtent = static_cast<float>(layout.workWidth);
        center = layout.centerFractionX;
        work = layout.rawWorkFractionX;
    } else {
        nativeExtent = static_cast<float>(layout.nativeHeight);
        workExtent = static_cast<float>(layout.workHeight);
        center = layout.centerFractionY;
        work = layout.rawWorkFractionY;
    }
}

Float2 ClampPixelCenter(Float2 value, float width, float height) noexcept {
    return {
        std::clamp(value.x, 0.5f, width - 0.5f),
        std::clamp(value.y, 0.5f, height - 0.5f)};
}

} // namespace

float PackRadius(float radius, float center, float work) noexcept {
    radius = std::abs(radius);
    if (work >= 1.0f || center >= 1.0f) return radius;
    if (radius > 1.0f) return ExtendPack(radius, center, work);
    if (radius <= center) return radius;
    const float c = (work - center) / (1.0f - center);
    const float t = (radius - center) / (1.0f - center);
    return center + (work - center) * t / (c + (1.0f - c) * t);
}

float UnpackRadius(float packedRadius, float center, float work) noexcept {
    packedRadius = std::abs(packedRadius);
    if (work >= 1.0f || center >= 1.0f) return packedRadius;
    if (packedRadius > work) return ExtendUnpack(packedRadius, center, work);
    if (packedRadius <= center) return packedRadius;
    const float y = (packedRadius - center) / (work - center);
    const float c = (work - center) / (1.0f - center);
    const float denominator = 1.0f - (1.0f - c) * y;
    const float t = c * y / denominator;
    return center + (1.0f - center) * t;
}

float PackCoordinate(float nativePixel, std::uint32_t axis, const LayoutV1 &layout) noexcept {
    float nativeExtent, workExtent, center, work;
    AxisData(axis, layout, nativeExtent, workExtent, center, work);
    if (layout.mode == WarpMode::Off) return nativePixel;
    if (layout.mode == WarpMode::Uniform) return nativePixel * workExtent / nativeExtent;
    const float signedRadius = nativePixel * 2.0f / nativeExtent - 1.0f;
    const float packedOriginalSpace = Sign(signedRadius) * PackRadius(signedRadius, center, work);
    return (packedOriginalSpace / work + 1.0f) * 0.5f * workExtent;
}

float UnpackCoordinate(float workPixel, std::uint32_t axis, const LayoutV1 &layout) noexcept {
    float nativeExtent, workExtent, center, work;
    AxisData(axis, layout, nativeExtent, workExtent, center, work);
    if (layout.mode == WarpMode::Off) return workPixel;
    if (layout.mode == WarpMode::Uniform) return workPixel * nativeExtent / workExtent;
    const float packedSigned = (workPixel * 2.0f / workExtent - 1.0f) * work;
    const float nativeSigned = Sign(packedSigned) * UnpackRadius(packedSigned, center, work);
    return (nativeSigned + 1.0f) * 0.5f * nativeExtent;
}

Float2 PackPosition(Float2 nativePixel, const LayoutV1 &layout) noexcept {
    return {PackCoordinate(nativePixel.x, 0, layout), PackCoordinate(nativePixel.y, 1, layout)};
}

Float2 UnpackPosition(Float2 workPixel, const LayoutV1 &layout) noexcept {
    return {UnpackCoordinate(workPixel.x, 0, layout), UnpackCoordinate(workPixel.y, 1, layout)};
}

Float2 PackMotion(Float2 nativeCurrentPixel, Float2 nativeMotionPixels, const LayoutV1 &layout) noexcept {
    Float2 nativePreviousPixel{nativeCurrentPixel.x + nativeMotionPixels.x,
                               nativeCurrentPixel.y + nativeMotionPixels.y};
    if ((layout.flags & ConfigFlagExtendMotionAtEdge) == 0) {
        nativeCurrentPixel = ClampPixelCenter(nativeCurrentPixel,
                                              static_cast<float>(layout.nativeWidth),
                                              static_cast<float>(layout.nativeHeight));
        nativePreviousPixel = ClampPixelCenter(nativePreviousPixel,
                                               static_cast<float>(layout.nativeWidth),
                                               static_cast<float>(layout.nativeHeight));
    }
    const Float2 current = PackPosition(nativeCurrentPixel, layout);
    const Float2 previous = PackPosition(nativePreviousPixel, layout);
    return {previous.x - current.x, previous.y - current.y};
}

Float2 UnpackMotion(Float2 workCurrentPixel, Float2 workMotionPixels, const LayoutV1 &layout) noexcept {
    Float2 workPreviousPixel{workCurrentPixel.x + workMotionPixels.x,
                             workCurrentPixel.y + workMotionPixels.y};
    if ((layout.flags & ConfigFlagExtendMotionAtEdge) == 0) {
        workCurrentPixel = ClampPixelCenter(workCurrentPixel,
                                           static_cast<float>(layout.workWidth),
                                           static_cast<float>(layout.workHeight));
        workPreviousPixel = ClampPixelCenter(workPreviousPixel,
                                            static_cast<float>(layout.workWidth),
                                            static_cast<float>(layout.workHeight));
    }
    const Float2 current = UnpackPosition(workCurrentPixel, layout);
    const Float2 previous = UnpackPosition(workPreviousPixel, layout);
    return {previous.x - current.x, previous.y - current.y};
}

// v2 Peripheral mapping is per side of the (possibly offset) centre band: the side's own
// half-span, centre/work fractions and curve; see sides_v2.h. With no offset it equals the v1
// symmetric mapping.
float PackCoordinate(float nativePixel, std::uint32_t axis, const LayoutV2 &layout) noexcept {
    float nativeExtent, workExtent, center, work;
    AxisData(axis, layout, nativeExtent, workExtent, center, work);
    if (layout.mode == WarpMode::Off) return nativePixel * workExtent / nativeExtent;
    if (layout.mode == WarpMode::Uniform) return nativePixel * workExtent / nativeExtent;
    const detail::AxisSidesV2 s = detail::AxisSides(layout, axis);
    const float delta = nativePixel - s.bandCenter;
    const int side = delta < 0.0f ? 0 : 1;
    const float radius = std::abs(delta) / s.halfSpan[side];
    const float packed = PackRadius(radius, s.center[side], s.work[side]);
    return s.workCenter + Sign(delta) * packed * s.halfSpan[side] * s.scale;
}

float UnpackCoordinate(float workPixel, std::uint32_t axis, const LayoutV2 &layout) noexcept {
    float nativeExtent, workExtent, center, work;
    AxisData(axis, layout, nativeExtent, workExtent, center, work);
    if (layout.mode == WarpMode::Off || layout.mode == WarpMode::Uniform)
        return workPixel * nativeExtent / workExtent;
    const detail::AxisSidesV2 s = detail::AxisSides(layout, axis);
    const float delta = workPixel - s.workCenter;
    const int side = delta < 0.0f ? 0 : 1;
    const float packed = std::abs(delta) / (s.halfSpan[side] * s.scale);
    const float radius = UnpackRadius(packed, s.center[side], s.work[side]);
    return s.bandCenter + Sign(delta) * radius * s.halfSpan[side];
}

Float2 PackPosition(Float2 nativePixel, const LayoutV2 &layout) noexcept {
    return {PackCoordinate(nativePixel.x, 0, layout), PackCoordinate(nativePixel.y, 1, layout)};
}

Float2 UnpackPosition(Float2 workPixel, const LayoutV2 &layout) noexcept {
    return {UnpackCoordinate(workPixel.x, 0, layout), UnpackCoordinate(workPixel.y, 1, layout)};
}

Float2 PackMotion(Float2 nativeCurrentPixel, Float2 nativeMotionPixels, const LayoutV2 &layout) noexcept {
    Float2 nativePreviousPixel{nativeCurrentPixel.x + nativeMotionPixels.x,
                               nativeCurrentPixel.y + nativeMotionPixels.y};
    if ((layout.flags & ConfigFlagExtendMotionAtEdge) == 0) {
        nativeCurrentPixel = ClampPixelCenter(nativeCurrentPixel,
                                              static_cast<float>(layout.nativeWidth),
                                              static_cast<float>(layout.nativeHeight));
        nativePreviousPixel = ClampPixelCenter(nativePreviousPixel,
                                               static_cast<float>(layout.nativeWidth),
                                               static_cast<float>(layout.nativeHeight));
    }
    const Float2 current = PackPosition(nativeCurrentPixel, layout);
    const Float2 previous = PackPosition(nativePreviousPixel, layout);
    return {previous.x - current.x, previous.y - current.y};
}

Float2 UnpackMotion(Float2 workCurrentPixel, Float2 workMotionPixels, const LayoutV2 &layout) noexcept {
    Float2 workPreviousPixel{workCurrentPixel.x + workMotionPixels.x,
                             workCurrentPixel.y + workMotionPixels.y};
    if ((layout.flags & ConfigFlagExtendMotionAtEdge) == 0) {
        workCurrentPixel = ClampPixelCenter(workCurrentPixel,
                                           static_cast<float>(layout.workWidth),
                                           static_cast<float>(layout.workHeight));
        workPreviousPixel = ClampPixelCenter(workPreviousPixel,
                                            static_cast<float>(layout.workWidth),
                                            static_cast<float>(layout.workHeight));
    }
    const Float2 current = UnpackPosition(workCurrentPixel, layout);
    const Float2 previous = UnpackPosition(workPreviousPixel, layout);
    return {previous.x - current.x, previous.y - current.y};
}

} // namespace pw
