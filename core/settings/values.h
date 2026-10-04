#pragma once
#include "core/api/ofps_settings_schema.h"
#include "core/context.h"
#include "optimizer_fps/types_v2.h"
#include <cstdint>
namespace ofps::core {
struct CoreSettings {
    ofps::sdk::ConfigV2 config = ProductDefaultConfig();
    TemporalSettings temporal{};
    DiagnosticsConfig diag{};
    int debugWarpPath = 0;
    bool workShiftEnabled = false;
    bool showCenterOutline = false;
    bool showWorkOutline = false;
    float brightnessPercent = 0.0f;
    float gamma = 1.0f;
    bool menuMode = false; // the ReShade shell's menu mode (OFPS_SET_MENU_MODE); the core only carries it
};
void DefaultSettingsValues(OfpsSettingsValues *out);
void SettingsToValues(const CoreSettings &in, OfpsSettingsValues *out);
int ValuesToSettings(const OfpsSettingsValues &in, CoreSettings *io);
void ClampTemporal(TemporalSettings *t);
int TemporalModeFromValue(int value);
bool SettingRange(const CoreSettings &s, std::uint32_t id, float *lo, float *hi);
} // namespace ofps::core
