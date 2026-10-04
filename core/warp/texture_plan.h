#pragma once

// Which optional textures the warp's GPU set owns. Decided when the set is prepared (EnsureGpu), from
// inputs that are all known there; the plan is part of the set's cache key, so a change of any input
// rebuilds the set through the retirement path. Nothing is created in the middle of a frame.

#include "core/warp/path_policy.h"

#include <cstdint>

namespace ofps::core::warp {

// The host's round-robin pack slots; one more slot is the background job's (FeatureState::kBgPackSlot).
constexpr std::uint32_t kHostPackSlots = 4;

struct TextureNeeds {
    PackPath path = PackPath::None;
    bool temporal = false;             // a temporal mode is on: temporal frames unpack into the native target
    bool warpBase = false;             // the temporal base (Pack -> Unpack without the model) is enabled
    bool transferReplacesBase = false; // detail transfer stands in for the base (TransferReplacesBase)
    bool background = false;           // the background job can run: it packs into its own slot
    bool copyFallback = false;         // a host output that cannot take a direct UAV write was seen
};

struct TexturePlan {
    std::uint32_t packSlots = 0;    // packed texture triplets: the host's slots, plus the background one
    bool copyIntermediates = false; // compute: one native intermediate per slot for UnpackPath::CopyFromUav
    bool unpackTarget = false;      // native target: pixel Unpack always, compute temporal frames
    bool unpackBase = false;        // native target of the temporal base
    friend bool operator==(const TexturePlan &, const TexturePlan &) = default;
};

[[nodiscard]] TexturePlan PlanTextures(const TextureNeeds &needs) noexcept;

} // namespace ofps::core::warp
