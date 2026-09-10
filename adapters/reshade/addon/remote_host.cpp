#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <reshade.hpp>

#include "remote_host.h"

#include "addon_context.h"
#include "config_store.h"
#include "layout_bridge.h"
#include "../ngx_hook.h"
#include "../pw_remote_ipc.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace pw_addon {
namespace {

HANDLE g_remoteMapping = nullptr;
PwRemoteBlockV1 *g_remoteBlock = nullptr;
bool g_remoteTried = false;
std::uint32_t g_remoteSeenSettings = 0;

pw_ofa::Settings g_ofa{};
bool g_ofaLoaded = false;
int g_ofaRefreshIn = 0;

// Picks up an edit made outside the tab (the file by hand, or the other tab). Once every 120
// presents, and then only a file stat.
void OfaRefresh()
{
    if (!g_ofaLoaded) { OfaEnsureLoaded(); return; }
    if (--g_ofaRefreshIn > 0) return;
    g_ofaRefreshIn = 120;
    pw_ofa::Refresh(&g_ofa);
}

void RemoteEnsureBlock()
{
    if (g_remoteBlock != nullptr || g_remoteTried) return;
    g_remoteTried = true;
    g_remoteMapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                         static_cast<DWORD>(sizeof(PwRemoteBlockV1)),
                                         PW_REMOTE_MAPPING_NAME_W);
    if (g_remoteMapping == nullptr) {
        reshade::log::message(reshade::log::level::warning,
            "Optimizer FPS: could not create the remote overlay block; the 32-bit tab will not connect");
        return;
    }
    const bool existed = GetLastError() == ERROR_ALREADY_EXISTS;
    g_remoteBlock = static_cast<PwRemoteBlockV1 *>(
        MapViewOfFile(g_remoteMapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(PwRemoteBlockV1)));
    if (g_remoteBlock == nullptr) {
        CloseHandle(g_remoteMapping);
        g_remoteMapping = nullptr;
        reshade::log::message(reshade::log::level::warning,
            "Optimizer FPS: could not map the remote overlay block");
        return;
    }
    if (!existed || g_remoteBlock->magic != PW_REMOTE_MAGIC || g_remoteBlock->version != PW_REMOTE_VERSION ||
        g_remoteBlock->size != sizeof(PwRemoteBlockV1))
        std::memset(g_remoteBlock, 0, sizeof(PwRemoteBlockV1));
    g_remoteBlock->magic = PW_REMOTE_MAGIC;
    g_remoteBlock->version = PW_REMOTE_VERSION;
    g_remoteBlock->size = sizeof(PwRemoteBlockV1);
    // Whatever the remote side left in the block belongs to an earlier session; this process's own
    // ini is the truth until the player moves a control in the game's tab.
    g_remoteSeenSettings = g_remoteBlock->settingsGeneration;
    reshade::log::message(reshade::log::level::info,
        "Optimizer FPS: remote overlay block ready (Local\\PeripheralWarpRemoteV1)");
}

PwRemoteSettingsV1 RemoteSettingsFrom(const pw::ConfigV2 &config)
{
    PwRemoteSettingsV1 settings{};
    settings.mode = static_cast<std::int32_t>(config.mode);
    settings.colorFilter = static_cast<std::int32_t>(config.colorFilter);
    settings.centerX = config.xAxis.centerPercent;
    settings.centerY = config.yAxis.centerPercent;
    settings.workX = config.xAxis.workPercent;
    settings.workY = config.yAxis.workPercent;
    settings.globalScale = config.globalScalePercent;
    settings.offsetX = config.centerOffsetXPercent;
    settings.offsetY = config.centerOffsetYPercent;
    settings.workShiftX = config.workShiftXPercent;
    settings.workShiftY = config.workShiftYPercent;
    settings.brightness = State().brightnessPercent;
    settings.gamma = State().gamma;
    settings.workShiftEnabled = State().workShiftEnabled ? 1 : 0;
    settings.showCenterOutline = State().showCenterOutline ? 1 : 0;
    settings.showWorkOutline = State().showWorkOutline ? 1 : 0;
    settings.temporalMode = State().temporal.mode;
    settings.temporalEvery = State().temporal.every;
    settings.temporalMaxQueue = State().temporal.maxQueue;
    settings.ofaSource = g_ofaLoaded ? (g_ofa.source == pw_ofa::SourceShader ? PwRemoteOfaShader : PwRemoteOfaOfa)
                                     : PwRemoteOfaUnset;
    settings.ofaGrid = g_ofaLoaded ? g_ofa.grid : 0;
    settings.ofaPerf = g_ofaLoaded ? g_ofa.perf : 0;
    return settings;
}

// The optical-flow half of an edit made in the 32-bit tab. Zero means "no opinion" (a version-1
// remote, or a remote whose host reported no config file), and an unchanged value writes nothing:
// the host re-reads the file on its write time, so a needless write would restart its session.
void RemoteApplyOfa(const PwRemoteSettingsV1 &settings)
{
    if (!g_ofaLoaded) return;
    if (settings.ofaSource == PwRemoteOfaUnset && settings.ofaGrid == 0 && settings.ofaPerf == 0) return;
    pw_ofa::Settings wanted = g_ofa;
    if (settings.ofaSource == PwRemoteOfaOfa) wanted.source = pw_ofa::SourceOfa;
    else if (settings.ofaSource == PwRemoteOfaShader) wanted.source = pw_ofa::SourceShader;
    if (settings.ofaGrid == 1 || settings.ofaGrid == 2 || settings.ofaGrid == 4) wanted.grid = settings.ofaGrid;
    if (settings.ofaPerf == 5 || settings.ofaPerf == 10 || settings.ofaPerf == 20) wanted.perf = settings.ofaPerf;
    if (wanted.source == g_ofa.source && wanted.grid == g_ofa.grid && wanted.perf == g_ofa.perf) return;
    g_ofa = wanted;
    OfaSave();
}

float RemoteFloat(float value, float lo, float hi, float fallback)
{
    return std::isfinite(value) ? std::clamp(value, lo, hi) : fallback;
}

// Applies an edit made in the game's tab exactly as the local overlay would: the layout through
// ApplyOverlayConfig (which persists it), the rest through the same setters and ini writers.
void RemoteApplySettings(const PwRemoteSettingsV1 &settings)
{
    pw::ConfigV2 config = ConfigForNgxHook();
    config.mode = static_cast<pw::WarpMode>(std::clamp(settings.mode, 0, 2));
    config.colorFilter = static_cast<pw::ColorFilter>(std::clamp(settings.colorFilter, 0, 1));
    config.xAxis.centerPercent = RemoteFloat(settings.centerX, 1.0f, 99.0f, config.xAxis.centerPercent);
    config.yAxis.centerPercent = RemoteFloat(settings.centerY, 1.0f, 99.0f, config.yAxis.centerPercent);
    config.xAxis.workPercent = RemoteFloat(settings.workX, pw::kMinimumWorkPercentV2, 100.0f, config.xAxis.workPercent);
    config.yAxis.workPercent = RemoteFloat(settings.workY, pw::kMinimumWorkPercentV2, 100.0f, config.yAxis.workPercent);
    config.globalScalePercent = RemoteFloat(settings.globalScale, pw::kMinimumEffectiveScalePercentV2, 100.0f,
                                            config.globalScalePercent);
    const float limitX = pw::MaximumCenterOffsetPercentV2(config.xAxis.centerPercent);
    const float limitY = pw::MaximumCenterOffsetPercentV2(config.yAxis.centerPercent);
    config.centerOffsetXPercent = RemoteFloat(settings.offsetX, -limitX, limitX, 0.0f);
    config.centerOffsetYPercent = RemoteFloat(settings.offsetY, -limitY, limitY, 0.0f);
    State().workShiftEnabled = settings.workShiftEnabled != 0;
    SaveWorkShiftEnabledToReShadeIni();
    if (State().workShiftEnabled) {
        float shiftMinX = 0.0f, shiftMaxX = 0.0f, shiftMinY = 0.0f, shiftMaxY = 0.0f;
        pw::WorkShiftLimitsPercentV2(config, 0, &shiftMinX, &shiftMaxX);
        pw::WorkShiftLimitsPercentV2(config, 1, &shiftMinY, &shiftMaxY);
        config.workShiftXPercent = RemoteFloat(settings.workShiftX, shiftMinX, shiftMaxX, 0.0f);
        config.workShiftYPercent = RemoteFloat(settings.workShiftY, shiftMinY, shiftMaxY, 0.0f);
    } else {
        config.workShiftXPercent = 0.0f;
        config.workShiftYPercent = 0.0f;
    }
    if (pw::ValidateConfig(config) == pw::Status::Ok) ApplyConfig(config);

    State().showCenterOutline = settings.showCenterOutline != 0;
    State().showWorkOutline = settings.showWorkOutline != 0;
    SaveOutlinesToReShadeIni();
    State().brightnessPercent = RemoteFloat(settings.brightness, -20.0f, 20.0f, 0.0f);
    State().gamma = RemoteFloat(settings.gamma, 0.7f, 1.4f, 1.0f);
    SaveColorAdjustToReShadeIni();
    const int mode = settings.temporalMode == 3 ? 3 : std::clamp(settings.temporalMode, 0, 1);
    State().temporal.mode = mode;
    State().temporal.every = std::clamp(settings.temporalEvery, mode == 3 ? 1 : 2, 8);
    State().temporal.maxQueue = std::clamp(settings.temporalMaxQueue, 0, 8);
    SaveTemporalToReShadeIni();
    pw_ngx::SetTemporal(State().temporal);
}

} // namespace

void OfaEnsureLoaded()
{
    if (g_ofaLoaded) return;
    pw_ofa::Init(State().module);
    if (!pw_ofa::Available()) return;
    if (!pw_ofa::Load(&g_ofa)) return;
    g_ofaLoaded = true;
    reshade::log::message(reshade::log::level::info,
        "Optimizer FPS: running inside the DLSS 5 feed host; the optical flow settings are editable in the tab");
}

bool OfaLoaded()
{
    return g_ofaLoaded;
}

pw_ofa::Settings &OfaSettings()
{
    return g_ofa;
}

void OfaSave()
{
    if (!g_ofaLoaded) return;
    if (pw_ofa::Save(g_ofa)) {
        char line[192];
        std::snprintf(line, sizeof(line),
                      "Optimizer FPS: optical flow -- mv_source=%s ofa_grid=%d ofa_perf=%d written to %s",
                      g_ofa.source == pw_ofa::SourceShader ? "shader" : "ofa", g_ofa.grid, g_ofa.perf,
                      pw_ofa::CfgPath().c_str());
        reshade::log::message(reshade::log::level::info, line);
    } else {
        reshade::log::message(reshade::log::level::warning,
            "Optimizer FPS: the optical flow settings could not be written to dlss5-feed-host64.cfg");
    }
}

void RemotePublish()
{
    RemoteEnsureBlock();
    if (g_remoteBlock == nullptr) return;
    PwRemoteBlockV1 &block = *g_remoteBlock;
    if (block.settingsGeneration != g_remoteSeenSettings) {
        PwRemoteSettingsV1 incoming{};
        std::memcpy(&incoming, &block.settings, sizeof(incoming));
        g_remoteSeenSettings = block.settingsGeneration;
        // A version-1 remote writes only the first 76 bytes of the struct; the optical-flow fields
        // it never heard of stay zero and are ignored below, which is exactly what is wanted.
        RemoteApplySettings(incoming);
        RemoteApplyOfa(incoming);
    }
    pw::ConfigV2 config = ConfigForNgxHook();
    const pw_ngx::Status hook = pw_ngx::GetStatus();
    PwRemoteStatusV1 status{};
    status.active = hook.active ? 1 : 0;
    const char *why = "";
    if (hook.active) {
        status.hookState = PwRemoteHook_Active;
    } else if (!hook.moduleFound) {
        status.hookState = PwRemoteHook_ModuleNotLoaded;
        why = "nvngx_dlssnr.dll is not loaded in the host process (no Neural Rendering host)";
    } else if (!hook.hooked) {
        status.hookState = PwRemoteHook_NotHooked;
        why = hook.reason[0] != 0 ? hook.reason : "hook not installed yet";
    } else if (!hook.featureCreated) {
        status.hookState = PwRemoteHook_WaitingForFeature;
        why = "waiting for the host to create feature 18";
    } else if (config.mode == pw::WarpMode::Off) {
        status.hookState = PwRemoteHook_ModeOff;
        why = "mode is Off";
    } else {
        status.hookState = PwRemoteHook_PassThrough;
        why = hook.reason[0] != 0 ? hook.reason : "pass-through";
    }
    std::snprintf(status.reason, sizeof(status.reason), "%s", why);
    status.nativeW = hook.nativeWidth;
    status.nativeH = hook.nativeHeight;
    status.modelW = hook.workWidth;
    status.modelH = hook.workHeight;
    status.temporalMode = static_cast<std::uint32_t>(hook.temporalMode);
    status.modelMs = hook.modelMsFull;
    // Without a temporal mode every evaluate is a model frame; with one the interposer counts both.
    status.fullFrames = hook.temporalMode != 0 ? hook.fullFrames : hook.evaluations;
    status.interpFrames = hook.interpFrames;
    // The optical flow: what the config file says, which is all this side can know -- the engine's
    // live state belongs to the host exe, not to the add-on inside it.
    OfaRefresh();
    status.ofaAvailable = g_ofaLoaded ? 1 : 0;
    status.ofaActive = g_ofaLoaded ? (g_ofa.source == pw_ofa::SourceShader ? 0 : 1) : -1;
    status.ofaGrid = g_ofaLoaded ? g_ofa.grid : 0;
    status.ofaPerf = g_ofaLoaded ? g_ofa.perf : 0;
    const PwRemoteSettingsV1 applied = RemoteSettingsFrom(config);
    std::memcpy(&block.status, &status, sizeof(status));
    std::memcpy(&block.applied, &applied, sizeof(applied));
    InterlockedExchange64(reinterpret_cast<volatile LONG64 *>(&block.hostHeartbeatTick),
                          static_cast<LONG64>(GetTickCount64()));
    InterlockedExchange(reinterpret_cast<volatile LONG *>(&block.statusGeneration),
                        static_cast<LONG>(block.statusGeneration + 1u));
}

} // namespace pw_addon
