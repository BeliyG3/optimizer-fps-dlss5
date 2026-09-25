#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "ini_store.h"
#include "ini_migration.h"
#include "shell_settings.h"
#include "addon_context.h"
#include "config_store.h"
#include "../direct_host.h"

#include <reshade.hpp>

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace ofps::reshade {
namespace {
IniSelection g_selection = IniSelection::Unavailable;
bool g_loaded = false;
bool g_initialPushPending = false;
thread_local int g_settingSaveFilter = -1;
thread_local int g_iniSaveSuppressionDepth = 0;
OfpsSettingsValues g_saved = SchemaDefaults();

struct ReShadeStore {
    bool GetInt(const char *key, int &out) const {
        return ::reshade::get_config_value(nullptr, ActiveIniSection(), key, out);
    }
    bool GetFloat(const char *key, float &out) const {
        return ::reshade::get_config_value(nullptr, ActiveIniSection(), key, out);
    }
};

void LogMigrationError(const char *reason) {
    char message[256];
    std::snprintf(message, sizeof(message),
                  "Optimizer FPS: ini migration BLOCKED: %s", reason);
    ::reshade::log::message(::reshade::log::level::error, message);
}

bool BackupBeforeMigration(const std::string &original) {
    wchar_t directory[MAX_PATH]{};
    const DWORD size = GetTempPathW(MAX_PATH, directory);
    if (size == 0 || size >= MAX_PATH) return false;
    wchar_t backup[MAX_PATH]{};
    if (GetTempFileNameW(directory, L"OFP", 0, backup) == 0) return false;
    // The verified input bytes are the snapshot that must be recoverable.
    std::ofstream output(backup, std::ios::binary | std::ios::trunc);
    output.write(original.data(), static_cast<std::streamsize>(original.size()));
    output.close();
    if (!output) {
        DeleteFileW(backup);
        return false;
    }
    const auto utf8 = std::filesystem::path(backup).u8string();
    const std::string backupPath(reinterpret_cast<const char *>(utf8.data()),
                                 utf8.size());
    const std::string message =
        "Optimizer FPS: original ReShade.ini backed up before migration at " +
        backupPath;
    ::reshade::log::message(::reshade::log::level::info, message.c_str());
    return true;
}

bool Explicit(const OfpsSettingsValues &values, std::uint32_t id) {
    return (values.explicitMask[id / 64u] & (1ull << (id % 64u))) != 0u;
}

void SaveChangedValue(std::uint32_t id, OfpsSettingValue value) {
    auto &values = State().values;
    if (id >= values.count) return;
    const bool same = kOfpsSettings[id].type == OFPS_TYPE_FLOAT
                          ? values.v[id].f == value.f : values.v[id].i == value.i;
    if (same) return;
    values.v[id] = value;
    values.explicitMask[id / 64u] |= 1ull << (id % 64u);
    SaveOneSettingToReShadeIni(id, values);
}

void SaveInt(std::uint32_t id, int number) {
    OfpsSettingValue value{};
    value.i = number;
    SaveChangedValue(id, value);
}

void SaveFloat(std::uint32_t id, float number) {
    OfpsSettingValue value{};
    value.f = number;
    SaveChangedValue(id, value);
}

void ReflectShellDebugLayer(OfpsSettingsValues &values) {
    const ShellSettings &shell = CurrentShellSettings();
    values.v[OFPS_SET_DEBUG_LAYER].i = shell.debugLayer ? 1 : 0;
    if (shell.debugLayerPresent)
        values.explicitMask[OFPS_SET_DEBUG_LAYER / 64u] |=
            1ull << (OFPS_SET_DEBUG_LAYER % 64u);
}
} // namespace

IniSelection CurrentIniSelection() { return g_selection; }
const char *ActiveIniSection() {
    return g_selection == IniSelection::Legacy ? "PeripheralWarp" : "OptimizerFPS";
}

IniSelection MigrateLegacyIniOnce() {
    g_selection = IniSelection::Unavailable;
    std::size_t length = 0;
    ::reshade::get_reshade_base_path(nullptr, &length);
    if (length == 0) { LogMigrationError("base path unavailable"); return g_selection; }
    std::vector<char> base(length);
    ::reshade::get_reshade_base_path(base.data(), &length);
    if (length == 0 || base.back() != '\0') {
        LogMigrationError("base path read failed");
        return g_selection;
    }
    const auto path = std::filesystem::path(
        std::u8string(reinterpret_cast<const char8_t *>(base.data()))) / "ReShade.ini";
    std::string bytes;
    if (!ReadIniForMigration(path, bytes)) {
        LogMigrationError("ReShade.ini cannot be read");
        return g_selection;
    }
    const MigrationResult scan = ScanIniForMigration(bytes);
    if (!scan.ShouldMigrate()) {
        g_selection = IniSelection::New;
        return g_selection;
    }
    if (scan.keys.empty()) {
        g_selection = IniSelection::Legacy;
        return g_selection;
    }
    std::string failedKey;
    const bool migrated = PreflightAndMigrate(
        scan,
        [](const std::string &key, std::string &value) {
            std::size_t length = 0;
            if (!::reshade::get_config_value(nullptr, "PeripheralWarp", key.c_str(),
                                             nullptr, &length) || length == 0)
                return false;
            std::vector<char> buffer(length);
            if (!::reshade::get_config_value(nullptr, "PeripheralWarp", key.c_str(),
                                             buffer.data(), &length))
                return false;
            value.assign(buffer.data());
            return true;
        },
        [&bytes] { return BackupBeforeMigration(bytes); },
        [](const std::string &key, const std::string &value) {
            ::reshade::set_config_value(nullptr, "OptimizerFPS", key.c_str(), value.c_str());
        }, failedKey);
    if (!migrated) {
        LogMigrationError(failedKey.c_str());
        g_selection = IniSelection::Legacy;
        return g_selection;
    }
    g_selection = IniSelection::New;
    char message[160];
    std::snprintf(message, sizeof(message),
                  "Optimizer FPS: migrated %zu known ReShade.ini keys to [OptimizerFPS]",
                  scan.keys.size());
    ::reshade::log::message(::reshade::log::level::info, message);
    return g_selection;
}

void LoadDiagnosticsFromReShadeIni() {
    State().values = SchemaDefaults();
    LoadValuesFromStore(ReShadeStore{}, State().values);
    ReflectShellDebugLayer(State().values);
}

bool LoadConfigFromReShadeIni(ofps::sdk::ConfigV2 &config) {
    return LoadConfigFromStore(ReShadeStore{}, config);
}

void LoadPersistedConfigOnce() {
    if (g_loaded) return;
    g_loaded = true;
    for (const char *key : {"OptiScalerTakeover", "ForceBridgeWarpOff"}) {
        bool present = false;
        for (const char *section : {"OptimizerFPS", "PeripheralWarp"}) {
            std::size_t size = 0;
            present |= ::reshade::get_config_value(nullptr, section, key, nullptr, &size);
        }
        if (present) {
            char message[160];
            std::snprintf(message, sizeof(message),
                          "ignored obsolete key %s; direct host ownership uses a named event", key);
            LogForNgxHook(false, message);
        }
    }
    LoadDiagnosticsFromReShadeIni();
    g_saved = State().values;
    g_initialPushPending = true;
    if (DirectHostActive()) return;
    AddonPersisted persisted;
    const bool anyLayoutKey = LoadFromStore(ReShadeStore{}, persisted);
    auto &state = State();
    state.showCenterOutline = persisted.showCenterOutline;
    state.showWorkOutline = persisted.showWorkOutline;
    state.workShiftEnabled = persisted.workShiftEnabled;
    state.brightnessPercent = persisted.brightnessPercent;
    state.gamma = persisted.gamma;
    state.temporal = persisted.temporal;
    if (!anyLayoutKey) return;
    if (ofps::sdk::ValidateConfig(persisted.config) != ofps::sdk::Status::Ok) {
        ::reshade::log::message(::reshade::log::level::warning,
            "Optimizer FPS: the layout saved in ReShade.ini is invalid; defaults stay active");
        return;
    }
    StoreConfig(persisted.config);
    ::reshade::log::message(::reshade::log::level::info,
        "Optimizer FPS: restored the layout saved in ReShade.ini");
}

void SaveOneSettingToReShadeIni(std::uint32_t id, const OfpsSettingsValues &values) {
    if (DirectHostActive() || g_selection != IniSelection::New ||
        id >= values.count || id >= OFPS_SET_COUNT) return;
    const OfpsSettingDesc &desc = kOfpsSettings[id];
    if ((desc.flags & OFPS_FLAG_PERSISTED) == 0u ||
        (desc.flags & OFPS_FLAG_DIAGNOSTIC) != 0u || !Explicit(values, id)) return;
    if (desc.type == OFPS_TYPE_FLOAT)
        ::reshade::set_config_value(nullptr, "OptimizerFPS", desc.iniKey, values.v[id].f);
    else
        ::reshade::set_config_value(nullptr, "OptimizerFPS", desc.iniKey, values.v[id].i);
    g_saved.v[id] = values.v[id];
    g_saved.explicitMask[id / 64u] |= 1ull << (id % 64u);
}

void SaveChangedExplicitValuesToReShadeIni(const OfpsSettingsValues &values) {
    if (DirectHostActive() || g_selection != IniSelection::New) return;
    for (std::uint32_t id = 0; id < values.count && id < OFPS_SET_COUNT; ++id) {
        if (!Explicit(values, id)) continue;
        const bool unchanged = kOfpsSettings[id].type == OFPS_TYPE_FLOAT
                                   ? g_saved.v[id].f == values.v[id].f
                                   : g_saved.v[id].i == values.v[id].i;
        if (Explicit(g_saved, id) && unchanged) continue;
        SaveOneSettingToReShadeIni(id, values);
    }
}

void SaveValuesToReShadeIni(const OfpsSettingsValues &values) {
    SaveChangedExplicitValuesToReShadeIni(values);
}

bool InitialSettingsPushPending() { return g_initialPushPending; }
void RememberInitialEffectiveValues(const OfpsSettingsValues &values) {
    if (values.count != OFPS_SET_COUNT) return;
    g_saved = values;
    g_initialPushPending = false;
}

ScopedIniSaveSuppression::ScopedIniSaveSuppression(bool enabled)
    : enabled_(enabled) {
    if (enabled_) ++g_iniSaveSuppressionDepth;
}
ScopedIniSaveSuppression::~ScopedIniSaveSuppression() {
    if (enabled_) --g_iniSaveSuppressionDepth;
}
bool IniSaveSuppressed() { return g_iniSaveSuppressionDepth != 0; }

ScopedSettingSaveFilter::ScopedSettingSaveFilter(std::uint32_t id)
    : previous_(g_settingSaveFilter) { g_settingSaveFilter = static_cast<int>(id); }
ScopedSettingSaveFilter::~ScopedSettingSaveFilter() { g_settingSaveFilter = previous_; }
int CurrentSettingSaveFilter() { return g_settingSaveFilter; }

void SaveWorkShiftEnabledToReShadeIni() {
    SaveInt(OFPS_SET_WORK_SHIFT_ENABLED, State().workShiftEnabled ? 1 : 0);
}
void SaveOutlinesToReShadeIni() {
    SaveInt(OFPS_SET_SHOW_CENTER_OUTLINE, State().showCenterOutline ? 1 : 0);
    SaveInt(OFPS_SET_SHOW_WORK_OUTLINE, State().showWorkOutline ? 1 : 0);
}
void SaveColorAdjustToReShadeIni() {
    SaveFloat(OFPS_SET_BRIGHTNESS, State().brightnessPercent);
    SaveFloat(OFPS_SET_GAMMA, State().gamma);
}
void SaveTemporalToReShadeIni() {
    const auto &temporal = State().temporal;
    SaveInt(OFPS_SET_TEMPORAL_MODE, temporal.mode);
    SaveInt(OFPS_SET_TEMPORAL_EVERY, temporal.every);
    SaveInt(OFPS_SET_TEMPORAL_MAX_QUEUE, temporal.maxQueue);
    SaveInt(OFPS_SET_MODEL_PASSES, temporal.modelPasses);
    SaveInt(OFPS_SET_SPREAD_PASSES, temporal.spreadPasses ? 1 : 0);
}
void SaveConfigToReShadeIni(const ofps::sdk::ConfigV2 &config) {
    SaveInt(OFPS_SET_MODE, static_cast<int>(config.mode));
    SaveInt(OFPS_SET_COLOR_FILTER, static_cast<int>(config.colorFilter));
    SaveFloat(OFPS_SET_CENTER_X, config.xAxis.centerPercent);
    SaveFloat(OFPS_SET_WORK_X, config.xAxis.workPercent);
    SaveFloat(OFPS_SET_CENTER_Y, config.yAxis.centerPercent);
    SaveFloat(OFPS_SET_WORK_Y, config.yAxis.workPercent);
    SaveFloat(OFPS_SET_GLOBAL_SCALE, config.globalScalePercent);
    SaveInt(OFPS_SET_FLAGS, static_cast<int>(config.flags));
    SaveFloat(OFPS_SET_OFFSET_X, config.centerOffsetXPercent);
    SaveFloat(OFPS_SET_OFFSET_Y, config.centerOffsetYPercent);
    SaveFloat(OFPS_SET_WORK_SHIFT_X, config.workShiftXPercent);
    SaveFloat(OFPS_SET_WORK_SHIFT_Y, config.workShiftYPercent);
}

} // namespace ofps::reshade
