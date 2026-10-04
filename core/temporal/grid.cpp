#include "core/temporal/grid.h"

#include <algorithm>
#include <cmath>

namespace ofps::core::temporal {
namespace {

constexpr float kMaxDensity = 0.8f;
constexpr std::uint32_t kMinNative = 32; // below this a frame keeps the native reprojection
constexpr std::uint32_t kMinGrid = 16;

bool Gated(float density) { return !std::isfinite(density) || density <= 0.0f || density > kMaxDensity; }

// ceil(native * work / raw), from the integers (the float densities are rounded); raw > 0.
std::uint32_t GridExtent(std::uint32_t native, std::uint32_t work, std::uint32_t raw)
{
    const std::uint64_t texels = (static_cast<std::uint64_t>(native) * work + raw - 1) / raw;
    return static_cast<std::uint32_t>(std::clamp<std::uint64_t>(texels, kMinGrid, native));
}

} // namespace

CarriedGrid CarriedGridFor(const ofps::sdk::LayoutV2 &layout, bool warped, bool enabled)
{
    using ofps::sdk::WarpMode;
    if (!enabled || !warped || layout.nativeWidth < kMinNative || layout.nativeHeight < kMinNative) return {};
    const bool uniform = layout.mode == WarpMode::Uniform;
    if (!uniform && layout.mode != WarpMode::Peripheral) return {};
    // Uniform: work over native. Peripheral: the 1:1 centre after Global scale, work over raw work - not the
    // compressed sides (minimumLocalScale) nor the pixel share.
    const std::uint32_t rawWidth = uniform ? layout.nativeWidth : layout.rawWorkWidth;
    const std::uint32_t rawHeight = uniform ? layout.nativeHeight : layout.rawWorkHeight;
    const float densityX = uniform ? static_cast<float>(layout.workWidth) / static_cast<float>(layout.nativeWidth)
                                   : layout.effectiveScaleX;
    const float densityY = uniform ? static_cast<float>(layout.workHeight) / static_cast<float>(layout.nativeHeight)
                                   : layout.effectiveScaleY;
    if (Gated(densityX) || Gated(densityY) || rawWidth == 0 || rawHeight == 0) return {};
    return {GridExtent(layout.nativeWidth, layout.workWidth, rawWidth), GridExtent(layout.nativeHeight, layout.workHeight, rawHeight)};
}

} // namespace ofps::core::temporal
