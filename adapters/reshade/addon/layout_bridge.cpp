#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <reshade.hpp>

#include "layout_bridge.h"

#include "addon_context.h"
#include "config_store.h"

#define PSAPI_VERSION 2
#include <psapi.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <mutex>

namespace pw_addon {
namespace {

struct LayoutBridgeLink {
    const PeripheralWarpLayoutBridgeV1 *table = nullptr;
    std::uint32_t lastSeenGeneration = 0;
    int probesLeft = 300;
    bool pullOnLink = true;
};
LayoutBridgeLink g_bridge;

bool g_bridgeRejected = false;
bool g_optiForcedOff = false; // bridge told to switch its warp off at least once (log once)
bool g_optiForceOffFailedLogged = false; // a failed Set is logged once, not every present

void ForceBridgeWarpOff()
{
    if (g_bridge.table == nullptr) return;
    PeripheralWarpLayoutStateV1 state{};
    state.structSize = sizeof(state);
    if (g_bridge.table->Get(&state) != PeripheralWarpLayoutBridge_Ok) {
        g_bridge.pullOnLink = true; // retried on the next present: the state was never read
        return;
    }
    if (state.mode == 0) { // already Off: nothing to write, this generation is dealt with
        g_bridge.lastSeenGeneration = state.generation;
        g_optiForcedOff = true;
        return;
    }
    state.mode = 0; // Off: OptiScaler runs the model on the native frame, the hook warps inside the call
    std::uint32_t generation = 0;
    if (g_bridge.table->Set(&state, &generation) != PeripheralWarpLayoutBridge_Ok) {
        // 26.21: the generation is NOT remembered on a failure - it used to be stored before the Set,
        // so the next present saw no change and never retried while the takeover kept warping on top
        // of OptiScaler's own warp.
        g_bridge.pullOnLink = true;
        if (!g_optiForceOffFailedLogged) {
            g_optiForceOffFailedLogged = true;
            reshade::log::message(reshade::log::level::warning,
                "Optimizer FPS: the layout bridge refused to switch OptiScaler's own spatial warp Off; retrying every present (the frame would be warped twice until it succeeds)");
        }
        return;
    }
    g_bridge.lastSeenGeneration = generation;
    g_optiForceOffFailedLogged = false;
    if (!g_optiForcedOff)
        reshade::log::message(reshade::log::level::info,
            "Optimizer FPS: OptiScaler's own spatial warp switched Off through the layout bridge; the add-on warps inside OptiScaler's NR call (OptiScalerTakeover=1)");
    g_optiForcedOff = true;
}

} // namespace

PeripheralWarpLayoutStateV1 ToBridgeState(const pw::ConfigV2 &config)
{
    PeripheralWarpLayoutStateV1 state{};
    state.structSize = sizeof(state);
    state.mode = static_cast<std::uint32_t>(config.mode);
    state.filter = static_cast<std::uint32_t>(config.colorFilter);
    state.centerX = config.xAxis.centerPercent;
    state.workX = config.xAxis.workPercent;
    state.centerY = config.yAxis.centerPercent;
    state.workY = config.yAxis.workPercent;
    state.globalScalePercent = config.globalScalePercent;
    return state;
}

void FromBridgeState(const PeripheralWarpLayoutStateV1 &state, pw::ConfigV2 &config)
{
    config.mode = static_cast<pw::WarpMode>(state.mode);
    config.colorFilter = static_cast<pw::ColorFilter>(state.filter);
    config.xAxis.centerPercent = state.centerX;
    config.xAxis.workPercent = state.workX;
    config.yAxis.centerPercent = state.centerY;
    config.yAxis.workPercent = state.workY;
    config.globalScalePercent = state.globalScalePercent;
}

pw::Status ApplyConfig(const pw::ConfigV2 &config)
{
    pw::Status status = pw::Status::Ok;
    bool rejectedByBridge = false;
    if (g_bridge.table != nullptr && !State().optiTakeover && pw::ValidateConfig(config) == pw::Status::Ok) {
        // The consumer validates with its own rules; a refusal leaves its layout untouched and
        // the next present pulls the consumer's values back into this overlay.
        PeripheralWarpLayoutStateV1 state = ToBridgeState(config);
        std::uint32_t generation = 0;
        const std::uint32_t bridgeStatus = g_bridge.table->Set(&state, &generation);
        if (bridgeStatus == PeripheralWarpLayoutBridge_Ok) {
            g_bridge.lastSeenGeneration = generation;
        } else {
            rejectedByBridge = true;
            g_bridge.pullOnLink = true;
        }
    }
    if (!rejectedByBridge) {
        status = StoreConfig(config);
        if (status == pw::Status::Ok) SaveConfigToReShadeIni(config);
    }
    g_bridgeRejected = rejectedByBridge;
    SetLastConfigStatus(status);
    return status;
}

void ProbeLayoutBridge()
{
    if (g_bridge.table != nullptr || g_bridge.probesLeft <= 0) return;
    --g_bridge.probesLeft;
    HMODULE modules[512]{};
    DWORD needed = 0;
    if (!EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &needed)) return;
    const DWORD count = std::min<DWORD>(needed / sizeof(HMODULE), static_cast<DWORD>(std::size(modules)));
    for (DWORD i = 0; i < count; ++i) {
        if (modules[i] == nullptr || modules[i] == State().module) continue;
        const auto fn = reinterpret_cast<PeripheralWarpLayoutBridgeV1Fn>(
            GetProcAddress(modules[i], PW_LAYOUT_BRIDGE_V1_EXPORT_NAME));
        if (fn == nullptr) continue;
        const PeripheralWarpLayoutBridgeV1 *table = fn();
        if (table == nullptr || table->structSize < sizeof(*table) || table->version != 1 ||
            table->Get == nullptr || table->Set == nullptr)
            continue;
        g_bridge.table = table;
        g_bridge.pullOnLink = true;
        if (State().optiTakeover) ForceBridgeWarpOff();
        char name[MAX_PATH]{};
        GetModuleFileNameA(modules[i], name, MAX_PATH);
        char message[MAX_PATH + 64]{};
        std::snprintf(message, sizeof(message), "Optimizer FPS: linked to the layout bridge exported by %s", name);
        reshade::log::message(reshade::log::level::info, message);
        return;
    }
    if (g_bridge.probesLeft == 0)
        reshade::log::message(reshade::log::level::info,
            "Optimizer FPS: no layout bridge in this process; the add-on keeps its own layout");
}

void PullLayoutFromBridge()
{
    if (g_bridge.table == nullptr) return;
    PeripheralWarpLayoutStateV1 state{};
    state.structSize = sizeof(state);
    if (g_bridge.table->Get(&state) != PeripheralWarpLayoutBridge_Ok) return;
    if (State().optiTakeover) {
        // OptiScaler's menu may switch its warp back on; keep it Off so the frame is warped once.
        if (state.generation != g_bridge.lastSeenGeneration || g_bridge.pullOnLink) {
            g_bridge.pullOnLink = false;
            ForceBridgeWarpOff(); // sets pullOnLink again when it could not write: retried next present
        }
        return;
    }
    if (!g_bridge.pullOnLink && state.generation == g_bridge.lastSeenGeneration) return;

    pw::ConfigV2 config = ConfigForNgxHook();
    pw::ConfigV2 updated = config;
    FromBridgeState(state, updated);
    if (std::memcmp(&updated, &config, sizeof(config)) == 0) {
        g_bridge.lastSeenGeneration = state.generation;
        g_bridge.pullOnLink = false;
        return;
    }
    const pw::Status status = StoreConfig(updated);
    g_bridge.lastSeenGeneration = state.generation;
    g_bridge.pullOnLink = false;
    if (status == pw::Status::Ok) {
        SaveConfigToReShadeIni(updated);
    } else {
        SetLastConfigStatus(status);
    }
}

bool BridgeLinked()
{
    return g_bridge.table != nullptr;
}

bool BridgeLayoutRejected()
{
    return g_bridgeRejected;
}

bool BridgeForcedWarpOff()
{
    return g_optiForcedOff;
}

void SetOptiScalerTakeover(bool takeover)
{
    State().optiTakeover = takeover;
    SaveOptiScalerTakeoverToReShadeIni(takeover);
    g_optiForcedOff = false;
    g_bridge.pullOnLink = true;
    if (takeover) ForceBridgeWarpOff();
    else {
        PeripheralWarpLayoutStateV1 state = ToBridgeState(ConfigForNgxHook());
        std::uint32_t generation = 0;
        if (g_bridge.table->Set(&state, &generation) == PeripheralWarpLayoutBridge_Ok) g_bridge.lastSeenGeneration = generation;
    }
}

} // namespace pw_addon
