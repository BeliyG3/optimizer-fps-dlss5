#pragma once

// The add-on's settings: the one validated pw::ConfigV2 the NGX interposer reads, and everything
// that is written to (or read from) ReShade.ini [PeripheralWarp] (stage 27.D2, moved from producer.cpp).
//
// Persistence: the layout lives in ReShade.ini [PeripheralWarp] and is restored on the next launch.
// The section name, the key list and the sanitising of the values are the ReShade-free schema in
// ini_schema.h (stage 27.T); this unit is its ReShade binding. Anything not in the kIniKeys[] list is
// read-only (CrashGuard, FloatingWindow, Passive, TraceExit, DebugLayer and the Debug* diagnostics of
// LoadDiagnosticsFromReShadeIni, deliberately never written back) or belongs to somebody else's
// add-on in the same ReShade.ini and is never touched.
//
// Applying an edited layout (which may have to go through OptiScaler's layout bridge first) is
// ApplyConfig in layout_bridge.h, not here.

#include "ini_schema.h" // kIniSection, kIniKeys, the Load*/Save* schema

#include "peripheral_warp/types_v2.h"

namespace pw_addon {

// Validates a configuration and stores it as the one the NGX interposer reads (ConfigForNgxHook).
// Persisting and the layout bridge are the callers' business - see ApplyConfig in layout_bridge.h.
pw::Status StoreConfig(const pw::ConfigV2 &config);

// The stored configuration. ConfigForNgxHook is handed to pw_ngx::Configure as the getter it polls
// on every create/evaluate, so it is called from the render thread as well.
pw::ConfigV2 ConfigForNgxHook();
// The same snapshot together with the status of the last configuration that was applied.
pw::ConfigV2 CurrentConfig(pw::Status &statusOut);
void SetLastConfigStatus(pw::Status status);

// Log sink handed to pw_ngx::Configure.
void LogForNgxHook(bool warning, const char *message);

void SaveConfigToReShadeIni(const pw::ConfigV2 &config);
// Fills the keys that exist; returns true when at least one was present.
bool LoadConfigFromReShadeIni(pw::ConfigV2 &config);
// The whole persisted set (layout, outlines, colour, temporal, takeover), read once on first present.
void LoadPersistedConfigOnce();

void SaveWorkShiftEnabledToReShadeIni();
void SaveOutlinesToReShadeIni();
void SaveColorAdjustToReShadeIni();
void SaveTemporalToReShadeIni();
void SaveOptiScalerTakeoverToReShadeIni(bool takeover);

// The NGX hook's diagnostic switches, read once at load from the read-only [PeripheralWarp] Debug*
// keys and handed to pw_ngx::SetDiagnostics. Nothing ever writes these keys back.
void LoadDiagnosticsFromReShadeIni(bool debugLayer);

} // namespace pw_addon
