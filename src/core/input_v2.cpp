#include "peripheral_warp/input_v2.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace pw {
namespace {

bool Known(MotionDirection value) noexcept
{
    return value == MotionDirection::CurrentToPrevious ||
           value == MotionDirection::PreviousToCurrent;
}

bool Known(DepthConvention value) noexcept
{
    return value == DepthConvention::Normal || value == DepthConvention::Reversed;
}

bool Known(ColorEncoding value) noexcept
{
    return value == ColorEncoding::LinearLdr || value == ColorEncoding::LinearHdr ||
           value == ColorEncoding::Srgb || value == ColorEncoding::Pq;
}

bool ValidRect(RectU32 rect) noexcept
{
    return rect.width != 0 && rect.height != 0 &&
           rect.x <= (std::numeric_limits<std::uint32_t>::max)() - rect.width &&
           rect.y <= (std::numeric_limits<std::uint32_t>::max)() - rect.height;
}

bool Fits(RectU32 rect, std::uint32_t width, std::uint32_t height) noexcept
{
    return ValidRect(rect) && rect.x + rect.width <= width && rect.y + rect.height <= height;
}

void StoreRect(float (&output)[4], RectU32 rect) noexcept
{
    output[0] = static_cast<float>(rect.x);
    output[1] = static_cast<float>(rect.y);
    output[2] = static_cast<float>(rect.width);
    output[3] = static_cast<float>(rect.height);
}

} // namespace

InputDescriptionV2 DefaultInputDescriptionV2(
    std::uint32_t nativeWidth, std::uint32_t nativeHeight) noexcept
{
    InputDescriptionV2 result{};
    result.structSize = sizeof(result);
    result.version = kInputDescriptionVersion;
    const RectU32 full{0, 0, nativeWidth, nativeHeight};
    result.colorRect = full;
    result.depthRect = full;
    result.motionRect = full;
    result.confidenceRect = full;
    result.motionScaleX = 1.0f;
    result.motionScaleY = 1.0f;
    result.motionDirection = MotionDirection::CurrentToPrevious;
    result.depthConvention = DepthConvention::Normal;
    result.colorEncoding = ColorEncoding::LinearLdr;
    return result;
}

Status ValidateInputDescriptionV2(const InputDescriptionV2 &description) noexcept
{
    if (description.structSize != sizeof(description)) return Status::StructSizeMismatch;
    if (description.version != kInputDescriptionVersion) return Status::VersionMismatch;
    if (!Known(description.motionDirection) || !Known(description.depthConvention) ||
        !Known(description.colorEncoding))
        return Status::InvalidFlags;
    constexpr std::uint32_t knownFlags = InputFlagConfidenceValid | InputFlagMotionJittered;
    if ((description.flags & ~knownFlags) != 0) return Status::InvalidFlags;
    if (!ValidRect(description.colorRect) || !ValidRect(description.depthRect) ||
        !ValidRect(description.motionRect) ||
        ((description.flags & InputFlagConfidenceValid) != 0 &&
         !ValidRect(description.confidenceRect)))
        return Status::InvalidDimensions;
    if (!std::isfinite(description.motionScaleX) || !std::isfinite(description.motionScaleY) ||
        !std::isfinite(description.jitterX) || !std::isfinite(description.jitterY) ||
        description.motionScaleX == 0.0f || description.motionScaleY == 0.0f)
        return Status::InvalidFlags;
    for (std::uint32_t value : description.reserved)
        if (value != 0) return Status::InvalidFlags;
    return Status::Ok;
}

Status BuildShaderInputConstantsV2(
    const InputDescriptionV2 &description,
    const InputResourceExtentsV2 &extents,
    ShaderInputConstantsV2 *constants) noexcept
{
    if (constants == nullptr) return Status::NullArgument;
    const Status status = ValidateInputDescriptionV2(description);
    if (status != Status::Ok) return status;
    if (!Fits(description.colorRect, extents.colorWidth, extents.colorHeight) ||
        !Fits(description.depthRect, extents.depthWidth, extents.depthHeight) ||
        !Fits(description.motionRect, extents.motionWidth, extents.motionHeight) ||
        ((description.flags & InputFlagConfidenceValid) != 0 &&
         !Fits(description.confidenceRect, extents.confidenceWidth, extents.confidenceHeight)))
        return Status::InvalidDimensions;

    ShaderInputConstantsV2 result{};
    StoreRect(result.colorRect, description.colorRect);
    StoreRect(result.depthRect, description.depthRect);
    StoreRect(result.motionRect, description.motionRect);
    StoreRect(result.confidenceRect, description.confidenceRect);
    result.colorDepthSize[0] = static_cast<float>(extents.colorWidth);
    result.colorDepthSize[1] = static_cast<float>(extents.colorHeight);
    result.colorDepthSize[2] = static_cast<float>(extents.depthWidth);
    result.colorDepthSize[3] = static_cast<float>(extents.depthHeight);
    result.motionConfidenceSize[0] = static_cast<float>(extents.motionWidth);
    result.motionConfidenceSize[1] = static_cast<float>(extents.motionHeight);
    result.motionConfidenceSize[2] = static_cast<float>(extents.confidenceWidth);
    result.motionConfidenceSize[3] = static_cast<float>(extents.confidenceHeight);
    result.motionScaleX = description.motionScaleX;
    result.motionScaleY = description.motionScaleY;
    result.motionDirectionSign = description.motionDirection == MotionDirection::CurrentToPrevious
                                     ? 1.0f : -1.0f;
    result.flags = static_cast<float>(description.flags);
    *constants = result;
    return Status::Ok;
}

} // namespace pw
