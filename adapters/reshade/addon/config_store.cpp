#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <reshade.hpp>

#include "config_store.h"

#include "addon_context.h"
#include "ini_schema.h"
#include "../ngx_hook.h"

#include <algorithm>
#include <cmath>
#include <mutex>

namespace pw_addon {
namespace {

std::mutex g_mutex;
pw::ConfigV2 g_config = pw::DefaultConfigV2();
pw::Status g_lastConfigStatus = pw::Status::Ok;
bool g_persistLoaded = false;

// The ReShade end of the ini schema (ini_schema.h): every key of the [PeripheralWarp] section goes
// through this one binding, so the schema itself - the key list, the defaults and the clamps - stays
// testable without ReShade (tests/test_addon_ini.cpp).
struct ReShadeStore {
    bool GetInt(const char *key, int &out) const
    {
        return reshade::get_config_value(nullptr, kIniSection, key, out);
    }
    bool GetFloat(const char *key, float &out) const
    {
        return reshade::get_config_value(nullptr, kIniSection, key, out);
    }
    void SetInt(const char *key, int value)
    {
        reshade::set_config_value(nullptr, kIniSection, key, value);
    }
    void SetFloat(const char *key, float value)
    {
        reshade::set_config_value(nullptr, kIniSection, key, value);
    }
};

} // namespace

pw::Status StoreConfig(const pw::ConfigV2 &config)
{
    const pw::Status status = pw::ValidateConfig(config);
    if (status != pw::Status::Ok) return status;
    std::scoped_lock lock(g_mutex);
    g_config = config;
    g_lastConfigStatus = pw::Status::Ok;
    return pw::Status::Ok;
}

pw::ConfigV2 ConfigForNgxHook()
{
    std::scoped_lock lock(g_mutex);
    return g_config;
}

pw::ConfigV2 CurrentConfig(pw::Status &statusOut)
{
    std::scoped_lock lock(g_mutex);
    statusOut = g_lastConfigStatus;
    return g_config;
}

void SetLastConfigStatus(pw::Status status)
{
    std::scoped_lock lock(g_mutex);
    g_lastConfigStatus = status;
}

void LogForNgxHook(bool warning, const char *message)
{
    reshade::log::message(warning ? reshade::log::level::warning : reshade::log::level::info, message);
}

void SaveWorkShiftEnabledToReShadeIni()
{
    ReShadeStore store;
    SaveWorkShiftEnabledToStore(store, State().workShiftEnabled);
}

void SaveOutlinesToReShadeIni()
{
    ReShadeStore store;
    SaveOutlinesToStore(store, State().showCenterOutline, State().showWorkOutline);
}

void SaveColorAdjustToReShadeIni()
{
    ReShadeStore store;
    SaveColorAdjustToStore(store, State().brightnessPercent, State().gamma);
}

void SaveTemporalToReShadeIni()
{
    ReShadeStore store;
    SaveTemporalToStore(store, State().temporal);
}

void SaveOptiScalerTakeoverToReShadeIni(bool takeover)
{
    ReShadeStore store;
    SaveOptiScalerTakeoverToStore(store, takeover);
}

void SaveConfigToReShadeIni(const pw::ConfigV2 &config)
{
    ReShadeStore store;
    SaveConfigToStore(store, config);
}

bool LoadConfigFromReShadeIni(pw::ConfigV2 &config)
{
    const ReShadeStore store;
    return LoadConfigFromStore(store, config);
}

void LoadPersistedConfigOnce()
{
    if (g_persistLoaded) return;
    g_persistLoaded = true;
    const ReShadeStore store;
    AddonPersisted persisted;
    const bool anyLayoutKey = LoadFromStore(store, persisted);
    {
        AddonState &state = State();
        state.showCenterOutline = persisted.showCenterOutline;
        state.showWorkOutline = persisted.showWorkOutline;
        state.workShiftEnabled = persisted.workShiftEnabled;
        state.optiTakeover = persisted.optiTakeover;
        state.brightnessPercent = persisted.brightnessPercent;
        state.gamma = persisted.gamma;
        state.temporal = persisted.temporal;
    }
    if (!anyLayoutKey) return;
    if (pw::ValidateConfig(persisted.config) != pw::Status::Ok) {
        reshade::log::message(reshade::log::level::warning,
            "Optimizer FPS: the layout saved in ReShade.ini is invalid; defaults stay active");
        return;
    }
    StoreConfig(persisted.config);
    reshade::log::message(reshade::log::level::info,
        "Optimizer FPS: restored the layout saved in ReShade.ini");
}

// 26.26: the NGX hook's diagnostic switches used to be PW_NGX_* environment variables. They are now
// read here, once, from [PeripheralWarp] Debug* keys in ReShade.ini and handed to the hook. These keys
// are READ-ONLY: SaveToReShadeIni() must never write one, so a saved layout cannot resurrect a
// diagnostic switch (26.6.J: the diagnostic switches are never persisted). The read-only debug keys are
// DebugLayer, DebugTiming, DebugTemporalReadback, DebugTemporalKeepOutput, DebugTemporalBlend,
// DebugTemporalDepth, DebugTemporalSmooth, DebugAsyncCompute, DebugAsyncNormalPriority,
// DebugAsyncNoRealtime, DebugAsyncLog, DebugAsyncShowPass, DebugHookDelayMs, DebugKeepBackbuffer and
// DebugDepthState (plus the non-hook TraceExit / Passive / CrashGuard, which are handled inline).
void LoadDiagnosticsFromReShadeIni(bool debugLayer)
{
    pw_ngx::DiagnosticsConfig d{};
    int integer = 0;
    float number = 0.0f;
    auto flag = [&](const char *key, bool &out) {
        if (reshade::get_config_value(nullptr, kIniSection, key, integer)) out = integer != 0;
    };
    auto number_key = [&](const char *key, float &out, float lo, float hi) {
        if (reshade::get_config_value(nullptr, kIniSection, key, number) && std::isfinite(number)) out = std::clamp(number, lo, hi);
    };
    d.debugLayerLog = debugLayer; // DebugLayer=1 turns the layer on (above) and its message dump on
    flag("DebugTiming", d.timing);
    flag("DebugTemporalReadback", d.temporalReadback);
    flag("DebugTemporalKeepOutput", d.temporalKeepOutput);
    number_key("DebugTemporalBlend", d.temporalBlend, 0.0f, 0.9f);
    number_key("DebugTemporalDepth", d.temporalDepth, 0.0f, 1.0f);
    number_key("DebugTemporalSmooth", d.temporalSmooth, 0.0f, 128.0f);
    flag("DebugAsyncCompute", d.asyncCompute);
    flag("DebugAsyncNormalPriority", d.asyncNormalPriority);
    flag("DebugAsyncNoRealtime", d.asyncNoRealtime);
    flag("DebugAsyncLog", d.asyncLog);
    flag("DebugAsyncShowPass", d.asyncShowPass);
    if (reshade::get_config_value(nullptr, kIniSection, "DebugHookDelayMs", integer) && integer > 0)
        d.hookDelayMs = static_cast<unsigned>(std::min(integer, 600000));
    flag("DebugKeepBackbuffer", d.keepBackbuffer);
    if (reshade::get_config_value(nullptr, kIniSection, "DebugDepthState", integer) && integer >= 0 && integer <= 4)
        d.depthState = integer;
    pw_ngx::SetDiagnostics(d);
}

} // namespace pw_addon
