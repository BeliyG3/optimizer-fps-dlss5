#pragma once
// The mod's own starting layout: the SDK's DefaultConfigV2() plus the product choices that differ from
// the SDK default. Every place that starts from "no saved settings" uses this, so a missing ini key
// gets the same value as the settings schema's default (tests/test_settings_schema.cpp checks that).
#include "optimizer_fps/types_v2.h"

namespace ofps::core {

inline ofps::sdk::ConfigV2 ProductDefaultConfig() noexcept {
    ofps::sdk::ConfigV2 config = ofps::sdk::DefaultConfigV2();
    config.colorFilter = ofps::sdk::ColorFilter::DetailTransfer; // 2026.9.3: detail transfer on by default
    return config;
}

} // namespace ofps::core
