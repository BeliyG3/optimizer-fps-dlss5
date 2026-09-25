#pragma once

// The life of the model behind a host's feature-18 handle: created through the hook or adopted when
// it predates it, re-created at the extent the layout needs, released with the host's handle. The
// per-frame evaluate (hook_dispatch.cpp) calls into these.

#include "core/frame/feature_state.h"

#include <cstdint>

namespace ofps::core {

bool DesiredLayout(const ofps::sdk::ConfigV2 &config, std::uint32_t nativeW, std::uint32_t nativeH, ofps::sdk::LayoutV2 *layout);

// Creates (or re-creates) the real feature behind the host's handle at w x h. Returns an NGX result.
int RecreateReal(FeatureState &st, ID3D12GraphicsCommandList *cmd, std::uint32_t w, std::uint32_t h);
// Follows the overlay's layout; `force` applies it even when it matches the recorded one. The warp is
// taken only while the host's frame fits the feature (st.hostFits).
int ApplyLayout(FeatureState &st, ID3D12GraphicsCommandList *cmd, const ofps::sdk::ConfigV2 &config, bool force);
// True when the two configurations give the same layout (the fields the warp is built from).
bool SameLayoutConfig(const ofps::sdk::ConfigV2 &a, const ofps::sdk::ConfigV2 &b);
// Takes over a feature-18 handle created before the hooks were installed; null when it is not one.
int CreateModelGuarded(FeatureState &st, ID3D12GraphicsCommandList *cmd,
                      uint32_t w, uint32_t h, uint32_t withholdUi, void **handle);

} // namespace ofps::core
