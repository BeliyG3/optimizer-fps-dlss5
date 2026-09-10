#pragma once

// The add-on's ReShade.ini schema, without ReShade (stage 27.T).
//
// Everything about the persisted settings that does not need the ReShade API lives here: the section
// name, the complete list of keys the add-on writes, the sanitising of the values read back, and the
// mapping between the [PeripheralWarp] keys and the add-on's settings. config_store.cpp is the thin
// adapter that binds these to reshade::get_config_value / set_config_value; tests bind them to an
// in-memory ini instead (tests/test_addon_ini.cpp), so the persistence is covered without ReShade.
//
// A Store is anything with:
//     bool GetInt(const char *key, int &out) const;      // false when the key is absent
//     bool GetFloat(const char *key, float &out) const;
//     void SetInt(const char *key, int value);
//     void SetFloat(const char *key, float value);
// Reads and writes go to the [PeripheralWarp] section (kIniSection); the Store knows which section
// it is bound to, so the key alone identifies a value here.

#include "../ngx_hook.h" // pw_ngx::TemporalSettings

#include "peripheral_warp/types_v2.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iterator>

namespace pw_addon {

// The one ini section the add-on owns.
inline constexpr const char *kIniSection = "PeripheralWarp";

// The complete set of keys the add-on WRITES, in one place. Anything not listed here is either
// read-only (CrashGuard, FloatingWindow, Passive, TraceExit, DebugLayer and the Debug* diagnostics
// of LoadDiagnosticsFromReShadeIni, deliberately never written back) or belongs to somebody else's
// add-on in the same ReShade.ini and is never touched. Removing the producer role in 27.C changed
// nothing here: the effect's Preview and Input source were per-runtime UI state and were never
// persisted, and ColorFilter is the D3D12 unpack filter (bilinear/adaptive), not an effect setting.
inline constexpr const char *kIniKeys[] = {
    "Mode", "ColorFilter", "CenterX", "WorkX", "CenterY", "WorkY", "GlobalScale", "Flags",
    "OffsetX", "OffsetY", "WorkShiftX", "WorkShiftY",              // SaveConfigToStore
    "ShowCenterOutline", "ShowWorkOutline",                        // SaveOutlinesToStore
    "WorkShiftEnabled",                                            // SaveWorkShiftEnabledToStore
    "Brightness", "Gamma",                                         // SaveColorAdjustToStore
    "TemporalMode", "TemporalEvery", "TemporalMaxQueue",           // SaveTemporalToStore
    "OptiScalerTakeover",                                          // the takeover checkbox
};
static_assert(std::size(kIniKeys) == 21, "keep the list in step with the Save* functions below");

// Only the mode, N and the queue cap are user settings (26.6.J). Everything else of the temporal machine
// is fixed at the values that were verified on the bench and in the game (depth 0.05, colour 0.08, no
// motion limit, hole fill, Catmull-Rom, warp base, residual age limit 8); the diagnostic switches are
// never persisted (a stale MotionInvert=1 in an ini once cost a day). Old keys left in the ini are ignored.
// The ini's mode, sanitised. Mode 2 (centre every frame) was withdrawn in 26.26 and maps to 1;
// the ABI value itself is frozen, so an old ini keeps working.
constexpr int TemporalModeFromIni(int value)
{
    return value == 3 ? 3 : (value < 0 ? 0 : (value > 1 ? 1 : value));
}
static_assert(TemporalModeFromIni(2) == 1, "ini TemporalMode=2 (withdrawn centre-every-frame) must fall back to 1");
static_assert(TemporalModeFromIni(0) == 0 && TemporalModeFromIni(1) == 1 && TemporalModeFromIni(3) == 3, "the other ini modes pass through");
static_assert(TemporalModeFromIni(-1) == 0 && TemporalModeFromIni(9) == 1, "out-of-range ini modes are clamped");

// Everything the add-on persists: the validated layout plus the AddonState fields that are saved with
// it. The defaults mirror pw::DefaultConfigV2() and AddonState, so a missing key leaves the built-in
// default in place exactly as the add-on does on a first launch.
struct AddonPersisted {
    pw::ConfigV2 config = pw::DefaultConfigV2();
    bool showCenterOutline = false;
    bool showWorkOutline = false;
    bool workShiftEnabled = false;
    bool optiTakeover = true;
    float brightnessPercent = 0.0f;
    float gamma = 1.0f;
    pw_ngx::TemporalSettings temporal{};
};

// --- layout -------------------------------------------------------------------------------------

// Fills the keys that exist; returns true when at least one was present.
template <class Store>
bool LoadConfigFromStore(const Store &store, pw::ConfigV2 &config)
{
    bool any = false;
    int integer = 0;
    float number = 0.0f;
    if (store.GetInt("Mode", integer)) {
        config.mode = static_cast<pw::WarpMode>(integer); any = true;
    }
    if (store.GetInt("ColorFilter", integer)) {
        config.colorFilter = static_cast<pw::ColorFilter>(integer); any = true;
    }
    if (store.GetFloat("CenterX", number)) { config.xAxis.centerPercent = number; any = true; }
    if (store.GetFloat("WorkX", number)) { config.xAxis.workPercent = number; any = true; }
    if (store.GetFloat("CenterY", number)) { config.yAxis.centerPercent = number; any = true; }
    if (store.GetFloat("WorkY", number)) { config.yAxis.workPercent = number; any = true; }
    if (store.GetFloat("GlobalScale", number)) { config.globalScalePercent = number; any = true; }
    if (store.GetInt("Flags", integer)) {
        config.flags = static_cast<std::uint32_t>(integer); any = true;
    }
    if (store.GetFloat("OffsetX", number)) { config.centerOffsetXPercent = number; any = true; }
    if (store.GetFloat("OffsetY", number)) { config.centerOffsetYPercent = number; any = true; }
    if (store.GetFloat("WorkShiftX", number)) { config.workShiftXPercent = number; any = true; }
    if (store.GetFloat("WorkShiftY", number)) { config.workShiftYPercent = number; any = true; }
    return any;
}

template <class Store>
void SaveConfigToStore(Store &store, const pw::ConfigV2 &config)
{
    store.SetInt("Mode", static_cast<int>(config.mode));
    store.SetInt("ColorFilter", static_cast<int>(config.colorFilter));
    store.SetFloat("CenterX", config.xAxis.centerPercent);
    store.SetFloat("WorkX", config.xAxis.workPercent);
    store.SetFloat("CenterY", config.yAxis.centerPercent);
    store.SetFloat("WorkY", config.yAxis.workPercent);
    store.SetFloat("GlobalScale", config.globalScalePercent);
    store.SetInt("Flags", static_cast<int>(config.flags));
    store.SetFloat("OffsetX", config.centerOffsetXPercent);
    store.SetFloat("OffsetY", config.centerOffsetYPercent);
    store.SetFloat("WorkShiftX", config.workShiftXPercent);
    store.SetFloat("WorkShiftY", config.workShiftYPercent);
}

// --- the settings beside the layout -------------------------------------------------------------

template <class Store>
void LoadTemporalFromStore(const Store &store, pw_ngx::TemporalSettings &temporal)
{
    int integer = 0;
    pw_ngx::TemporalSettings t{};
    if (store.GetInt("TemporalMode", integer)) t.mode = TemporalModeFromIni(integer);
    if (store.GetInt("TemporalEvery", integer)) t.every = std::clamp(integer, 1, 8);
    if (store.GetInt("TemporalMaxQueue", integer)) t.maxQueue = std::clamp(integer, 0, 8);
    temporal = t;
}

// Only these three temporal keys are ever written. The read-only Debug* diagnostics keys
// (see LoadDiagnosticsFromReShadeIni) are deliberately absent from every Save function.
template <class Store>
void SaveTemporalToStore(Store &store, const pw_ngx::TemporalSettings &temporal)
{
    store.SetInt("TemporalMode", temporal.mode);
    store.SetInt("TemporalEvery", temporal.every);
    store.SetInt("TemporalMaxQueue", temporal.maxQueue);
}

template <class Store>
void SaveOutlinesToStore(Store &store, bool showCenterOutline, bool showWorkOutline)
{
    store.SetInt("ShowCenterOutline", showCenterOutline ? 1 : 0);
    store.SetInt("ShowWorkOutline", showWorkOutline ? 1 : 0);
}

template <class Store>
void SaveWorkShiftEnabledToStore(Store &store, bool enabled)
{
    store.SetInt("WorkShiftEnabled", enabled ? 1 : 0);
}

template <class Store>
void SaveColorAdjustToStore(Store &store, float brightnessPercent, float gamma)
{
    store.SetFloat("Brightness", brightnessPercent);
    store.SetFloat("Gamma", gamma);
}

template <class Store>
void SaveOptiScalerTakeoverToStore(Store &store, bool takeover)
{
    store.SetInt("OptiScalerTakeover", takeover ? 1 : 0);
}

// --- the whole persisted set --------------------------------------------------------------------

// Reads everything that is persisted. The layout is left at its default (and false returned) when the
// ini carries none of the layout keys; the settings beside it are applied key by key regardless.
// MotionScale / MotionInvert are diagnostics and are not read back (26.6.J): a stale MotionInvert=1 once
// inverted the warped model's vectors for a whole day of testing.
template <class Store>
bool LoadFromStore(const Store &store, AddonPersisted &out)
{
    int integer = 0;
    if (store.GetInt("ShowCenterOutline", integer)) out.showCenterOutline = integer != 0;
    if (store.GetInt("ShowWorkOutline", integer)) out.showWorkOutline = integer != 0;
    if (store.GetInt("WorkShiftEnabled", integer)) out.workShiftEnabled = integer != 0;
    if (store.GetInt("OptiScalerTakeover", integer)) out.optiTakeover = integer != 0;
    float number = 0.0f;
    if (store.GetFloat("Brightness", number) && std::isfinite(number)) out.brightnessPercent = std::clamp(number, -20.0f, 20.0f);
    if (store.GetFloat("Gamma", number) && std::isfinite(number) && number > 0.0f) out.gamma = std::clamp(number, 0.7f, 1.4f);
    LoadTemporalFromStore(store, out.temporal);
    return LoadConfigFromStore(store, out.config);
}

// Writes exactly the kIniKeys[] set - no more, no less. Foreign and read-only keys already in the ini
// are left untouched, because nothing here deletes.
template <class Store>
void SaveToStore(Store &store, const AddonPersisted &persisted)
{
    SaveConfigToStore(store, persisted.config);
    SaveOutlinesToStore(store, persisted.showCenterOutline, persisted.showWorkOutline);
    SaveWorkShiftEnabledToStore(store, persisted.workShiftEnabled);
    SaveColorAdjustToStore(store, persisted.brightnessPercent, persisted.gamma);
    SaveTemporalToStore(store, persisted.temporal);
    SaveOptiScalerTakeoverToStore(store, persisted.optiTakeover);
}

} // namespace pw_addon
