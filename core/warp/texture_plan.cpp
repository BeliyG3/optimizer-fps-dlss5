#include "core/warp/texture_plan.h"

namespace ofps::core::warp {

TexturePlan PlanTextures(const TextureNeeds &n) noexcept {
    TexturePlan plan;
    if (n.path == PackPath::None) return plan;
    plan.packSlots = kHostPackSlots + (n.background ? 1u : 0u);
    // Only the compute Unpack copies through an intermediate; the pixel path draws into a render target.
    plan.copyIntermediates = n.path == PackPath::Compute && n.copyFallback;
    // The pixel Unpack always draws into the native target; the compute one writes the host's output
    // (or the codec's answer) directly and needs the target only for a temporal frame.
    plan.unpackTarget = n.path == PackPath::Pixel || n.temporal;
    // The base is recorded only on temporal frames, and not where the detail transfer replaces it.
    plan.unpackBase = n.temporal && n.warpBase && !n.transferReplacesBase;
    return plan;
}

} // namespace ofps::core::warp
