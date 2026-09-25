#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

// These add-on exports are called on the presenting thread. Bench clients resolve
// the names through GetProcAddress; no Core ABI entry is added here.
#include "addon_context.h"
#include "config_store.h"
#include "ini_store.h"
#include "layout_bridge_v1.h"
#include "../shell_host.h"
#include "../direct_host.h"
#include "core/api/ofps_settings_schema.h"
#include "optimizer_fps/types_v2.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>

namespace ofps::reshade {
namespace {

bool ReadSettings(IOfpsCore *core, OfpsSettingsValues &values) {
    values.size = sizeof(values);
    core->GetSettings(&values);
    return values.count == OFPS_SET_COUNT;
}

void LogSetError(int result) {
    char message[128];
    std::snprintf(message, sizeof(message),
                  "Optimizer FPS: setting export SetSettings failed (%d)", result);
    LogForNgxHook(true, message);
}

// The legacy calls edit several fields together. Suppress the synchronous event
// and persist only their requested fields after Core has applied its clamps.
template <std::size_t N>
std::uint32_t ApplyLegacySettings(IOfpsCore *core, OfpsSettingsValues &values,
                                  const std::array<std::uint32_t, N> &ids) {
    for (const std::uint32_t id : ids)
        values.explicitMask[id / 64u] |= 1ull << (id % 64u);
    const ScopedIniSaveSuppression suppressEvent(true);
    const int result = core->SetSettings(&values);
    if (result != OFPS_OK) {
        LogSetError(result);
        return PeripheralWarpLayoutBridge_InvalidArgument;
    }
    if (!ReadSettings(core, values)) return PeripheralWarpLayoutBridge_NotReady;
    for (const std::uint32_t id : ids) SaveOneSettingToReShadeIni(id, values);
    AdoptCoreValuesForOverlay();
    return PeripheralWarpLayoutBridge_Ok;
}

ofps::sdk::ConfigV2 ConfigFromValues(const OfpsSettingsValues &values) {
    auto config = ConfigForNgxHook();
    config.mode = static_cast<ofps::sdk::WarpMode>(values.v[OFPS_SET_MODE].i);
    config.colorFilter = static_cast<ofps::sdk::ColorFilter>(values.v[OFPS_SET_COLOR_FILTER].i);
    config.xAxis.centerPercent = values.v[OFPS_SET_CENTER_X].f;
    config.yAxis.centerPercent = values.v[OFPS_SET_CENTER_Y].f;
    config.xAxis.workPercent = values.v[OFPS_SET_WORK_X].f;
    config.yAxis.workPercent = values.v[OFPS_SET_WORK_Y].f;
    config.globalScalePercent = values.v[OFPS_SET_GLOBAL_SCALE].f;
    config.flags = static_cast<std::uint32_t>(values.v[OFPS_SET_FLAGS].i);
    config.centerOffsetXPercent = values.v[OFPS_SET_OFFSET_X].f;
    config.centerOffsetYPercent = values.v[OFPS_SET_OFFSET_Y].f;
    config.workShiftXPercent = values.v[OFPS_SET_WORK_SHIFT_X].f;
    config.workShiftYPercent = values.v[OFPS_SET_WORK_SHIFT_Y].f;
    return config;
}

void FromBridgeState(const PeripheralWarpLayoutStateV1 &state,
                     ofps::sdk::ConfigV2 &config) {
    config.mode = static_cast<ofps::sdk::WarpMode>(state.mode);
    config.colorFilter = static_cast<ofps::sdk::ColorFilter>(state.filter);
    config.xAxis.centerPercent = state.centerX;
    config.xAxis.workPercent = state.workX;
    config.yAxis.centerPercent = state.centerY;
    config.yAxis.workPercent = state.workY;
    config.globalScalePercent = state.globalScalePercent;
}

} // namespace
} // namespace ofps::reshade

extern "C" __declspec(dllexport) std::uint32_t
OptimizerFpsSetSettingV1(std::uint32_t id, OfpsSettingValue value) {
    using namespace ofps::reshade;
    if (id >= OFPS_SET_COUNT) return PeripheralWarpLayoutBridge_InvalidArgument;
    IOfpsCore *core = Core();
    if (core == nullptr || DirectHostActive()) return PeripheralWarpLayoutBridge_NotReady;

    const OfpsSettingDesc &desc = kOfpsSettings[id];
    if ((desc.flags & OFPS_FLAG_PERSISTED) == 0u ||
        (desc.flags & OFPS_FLAG_DIAGNOSTIC) != 0u || desc.hostCap != 0u ||
        desc.type > OFPS_TYPE_ENUM)
        return PeripheralWarpLayoutBridge_InvalidArgument;

    float lo = desc.minValue, hi = desc.maxValue;
    if (desc.rangeFrom != 0u) core->GetSettingRange(id, &lo, &hi);
    const float number = desc.type == OFPS_TYPE_FLOAT ? value.f : static_cast<float>(value.i);
    if (!std::isfinite(number) || !std::isfinite(lo) || !std::isfinite(hi) ||
        lo > hi || number < lo || number > hi)
        return PeripheralWarpLayoutBridge_InvalidArgument;
    if (desc.type == OFPS_TYPE_BOOL && value.i != 0 && value.i != 1)
        return PeripheralWarpLayoutBridge_InvalidArgument;
    // Range is checked before indexing the null-terminated enum label table.
    if (desc.type == OFPS_TYPE_ENUM &&
        (desc.enumLabels == nullptr || desc.enumLabels[value.i] == nullptr))
        return PeripheralWarpLayoutBridge_InvalidArgument;

    OfpsSettingsValues values{};
    if (!ReadSettings(core, values)) return PeripheralWarpLayoutBridge_NotReady;
    values.v[id] = value;
    values.explicitMask[id / 64u] |= 1ull << (id % 64u);
    const ScopedSettingSaveFilter onlyThisKey(id);
    const int result = core->SetSettings(&values);
    if (result != OFPS_OK) {
        LogSetError(result);
        return PeripheralWarpLayoutBridge_InvalidArgument;
    }
    if (!ReadSettings(core, values)) return PeripheralWarpLayoutBridge_NotReady;
    SaveOneSettingToReShadeIni(id, values); // Also persists an explicit default without an event.
    AdoptCoreValuesForOverlay();
    return PeripheralWarpLayoutBridge_Ok;
}

extern "C" __declspec(dllexport) std::uint32_t
PeripheralWarpSetTemporalV1(std::uint32_t mode, std::uint32_t every) {
    using namespace ofps::reshade;
    if (DirectHostActive()) return PeripheralWarpLayoutBridge_NotReady;
    if (mode > 3 || mode == 2 || every < (mode == 3 ? 1u : 2u) || every > 8)
        return PeripheralWarpLayoutBridge_InvalidArgument;
    IOfpsCore *core = Core();
    if (core == nullptr) return PeripheralWarpLayoutBridge_NotReady;
    OfpsSettingsValues values{};
    if (!ReadSettings(core, values)) return PeripheralWarpLayoutBridge_NotReady;
    values.v[OFPS_SET_TEMPORAL_MODE].i = static_cast<int>(mode);
    values.v[OFPS_SET_TEMPORAL_EVERY].i = static_cast<int>(every);
    return ApplyLegacySettings(core, values,
        std::array<std::uint32_t, 2>{OFPS_SET_TEMPORAL_MODE, OFPS_SET_TEMPORAL_EVERY});
}

extern "C" __declspec(dllexport) std::uint32_t
PeripheralWarpSetLayoutV1(const PeripheralWarpLayoutStateV1 *state) {
    using namespace ofps::reshade;
    if (DirectHostActive()) return PeripheralWarpLayoutBridge_NotReady;
    if (state == nullptr || state->structSize < sizeof(PeripheralWarpLayoutStateV1))
        return PeripheralWarpLayoutBridge_InvalidArgument;
    IOfpsCore *core = Core();
    if (core == nullptr) return PeripheralWarpLayoutBridge_NotReady;
    OfpsSettingsValues values{};
    if (!ReadSettings(core, values)) return PeripheralWarpLayoutBridge_NotReady;
    auto config = ConfigFromValues(values);
    FromBridgeState(*state, config); // Preserve flags, offsets and shifts from the Core snapshot.
    if (ofps::sdk::ValidateConfig(config) != ofps::sdk::Status::Ok)
        return PeripheralWarpLayoutBridge_InvalidArgument;
    values.v[OFPS_SET_MODE].i = static_cast<int>(config.mode);
    values.v[OFPS_SET_COLOR_FILTER].i = static_cast<int>(config.colorFilter);
    values.v[OFPS_SET_CENTER_X].f = config.xAxis.centerPercent;
    values.v[OFPS_SET_WORK_X].f = config.xAxis.workPercent;
    values.v[OFPS_SET_CENTER_Y].f = config.yAxis.centerPercent;
    values.v[OFPS_SET_WORK_Y].f = config.yAxis.workPercent;
    values.v[OFPS_SET_GLOBAL_SCALE].f = config.globalScalePercent;
    return ApplyLegacySettings(core, values,
        std::array<std::uint32_t, 7>{OFPS_SET_MODE, OFPS_SET_COLOR_FILTER,
            OFPS_SET_CENTER_X, OFPS_SET_WORK_X, OFPS_SET_CENTER_Y,
            OFPS_SET_WORK_Y, OFPS_SET_GLOBAL_SCALE});
}
