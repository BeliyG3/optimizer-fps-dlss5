#pragma once

#include "core/api/ofps_settings_schema.h"

namespace ofps::reshade {
// One status line per group whose text changes, never wrapped (nothing below it moves); the counters
// that only matter for diagnosis are drawn with showAdvanced.
void DrawTemporalStatus(const OfpsStatus &status, std::uint32_t group, bool showAdvanced);
}
