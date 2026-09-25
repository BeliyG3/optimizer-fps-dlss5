#pragma once

#include "ini_schema.h"

#include <cstdint>

namespace ofps::reshade {

enum class IniSelection { New, Legacy, Unavailable };

IniSelection MigrateLegacyIniOnce();
IniSelection CurrentIniSelection();
const char *ActiveIniSection();

void LoadPersistedConfigOnce();
void LoadDiagnosticsFromReShadeIni();
bool LoadConfigFromReShadeIni(ofps::sdk::ConfigV2 &config);

void SaveWorkShiftEnabledToReShadeIni();
void SaveOutlinesToReShadeIni();
void SaveColorAdjustToReShadeIni();
void SaveTemporalToReShadeIni();
void SaveConfigToReShadeIni(const ofps::sdk::ConfigV2 &config);
void SaveValuesToReShadeIni(const OfpsSettingsValues &values);
void SaveChangedExplicitValuesToReShadeIni(const OfpsSettingsValues &values);
void SaveOneSettingToReShadeIni(std::uint32_t id, const OfpsSettingsValues &values);
bool InitialSettingsPushPending();
void RememberInitialEffectiveValues(const OfpsSettingsValues &values);

class ScopedIniSaveSuppression {
public:
    explicit ScopedIniSaveSuppression(bool enabled);
    ~ScopedIniSaveSuppression();
    ScopedIniSaveSuppression(const ScopedIniSaveSuppression &) = delete;
    ScopedIniSaveSuppression &operator=(const ScopedIniSaveSuppression &) = delete;
private:
    bool enabled_;
};
bool IniSaveSuppressed();

class ScopedSettingSaveFilter {
public:
    explicit ScopedSettingSaveFilter(std::uint32_t id);
    ~ScopedSettingSaveFilter();
    ScopedSettingSaveFilter(const ScopedSettingSaveFilter &) = delete;
    ScopedSettingSaveFilter &operator=(const ScopedSettingSaveFilter &) = delete;
private:
    int previous_;
};
int CurrentSettingSaveFilter();

} // namespace ofps::reshade
