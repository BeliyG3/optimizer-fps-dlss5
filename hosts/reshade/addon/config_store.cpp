#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <reshade.hpp>

#include "config_store.h"

#include "addon_context.h"
#include "ini_store.h"
#include "../shell_host.h"
#include "../direct_host.h"

#include <mutex>

namespace ofps::reshade {
namespace {

std::mutex g_mutex;
ofps::sdk::ConfigV2 g_config = ofps::sdk::DefaultConfigV2();
ofps::sdk::Status g_lastConfigStatus = ofps::sdk::Status::Ok;
} // namespace

ofps::sdk::Status StoreConfig(const ofps::sdk::ConfigV2 &config)
{
    const ofps::sdk::Status status = ofps::sdk::ValidateConfig(config);
    if (status != ofps::sdk::Status::Ok) return status;
    std::scoped_lock lock(g_mutex);
    g_config = config;
    g_lastConfigStatus = ofps::sdk::Status::Ok;
    return ofps::sdk::Status::Ok;
}

ofps::sdk::Status ApplyConfig(const ofps::sdk::ConfigV2 &config)
{
    if (DirectHostActive()) return ofps::sdk::Status::NotReady;
    const auto status = StoreConfig(config);
    if (status == ofps::sdk::Status::Ok) SaveConfigToReShadeIni(config);
    SetLastConfigStatus(status);
    return status;
}

ofps::sdk::ConfigV2 ConfigForNgxHook()
{
    std::scoped_lock lock(g_mutex);
    return g_config;
}

ofps::sdk::ConfigV2 CurrentConfig(ofps::sdk::Status &statusOut)
{
    std::scoped_lock lock(g_mutex);
    statusOut = g_lastConfigStatus;
    return g_config;
}

void SetLastConfigStatus(ofps::sdk::Status status)
{
    std::scoped_lock lock(g_mutex);
    g_lastConfigStatus = status;
}

void LogForNgxHook(bool warning, const char *message)
{
    ::reshade::log::message(warning ? ::reshade::log::level::warning : ::reshade::log::level::info, message);
}

void PushSettingsToCore() {
    if (DirectHostActive()) {
        auto &state = State();
        state.directValuesReady = Core() != nullptr;
        if (state.directValuesReady) Core()->GetSettings(&state.directValues);
        return;
    }
    if (!Core()) return;
    auto &state = State(); auto &v = state.values;
    const auto c = ConfigForNgxHook();
    v.v[OFPS_SET_MODE].i = static_cast<int>(c.mode);
    v.v[OFPS_SET_COLOR_FILTER].i = static_cast<int>(c.colorFilter);
    v.v[OFPS_SET_CENTER_X].f = c.xAxis.centerPercent; v.v[OFPS_SET_CENTER_Y].f = c.yAxis.centerPercent;
    v.v[OFPS_SET_WORK_X].f = c.xAxis.workPercent; v.v[OFPS_SET_WORK_Y].f = c.yAxis.workPercent;
    v.v[OFPS_SET_GLOBAL_SCALE].f = c.globalScalePercent; v.v[OFPS_SET_FLAGS].i = static_cast<int>(c.flags);
    v.v[OFPS_SET_OFFSET_X].f = c.centerOffsetXPercent; v.v[OFPS_SET_OFFSET_Y].f = c.centerOffsetYPercent;
    v.v[OFPS_SET_WORK_SHIFT_X].f = c.workShiftXPercent; v.v[OFPS_SET_WORK_SHIFT_Y].f = c.workShiftYPercent;
    v.v[OFPS_SET_WORK_SHIFT_ENABLED].i = state.workShiftEnabled;
    v.v[OFPS_SET_SHOW_CENTER_OUTLINE].i = state.showCenterOutline; v.v[OFPS_SET_SHOW_WORK_OUTLINE].i = state.showWorkOutline;
    v.v[OFPS_SET_BRIGHTNESS].f = state.brightnessPercent; v.v[OFPS_SET_GAMMA].f = state.gamma;
    v.v[OFPS_SET_TEMPORAL_MODE].i = state.temporal.mode; v.v[OFPS_SET_TEMPORAL_EVERY].i = state.temporal.every;
    v.v[OFPS_SET_TEMPORAL_MAX_QUEUE].i = state.temporal.maxQueue; v.v[OFPS_SET_MODEL_PASSES].i = state.temporal.modelPasses;
    v.v[OFPS_SET_SPREAD_PASSES].i = state.temporal.spreadPasses;
    const bool initialPush = InitialSettingsPushPending();
    const ScopedIniSaveSuppression suppressInitialSave(initialPush);
    if (Core()->SetSettings(&v) != OFPS_OK) return;
    Core()->GetSettings(&v);
    if (initialPush) RememberInitialEffectiveValues(v);
    state.temporal.mode = v.v[OFPS_SET_TEMPORAL_MODE].i; state.temporal.every = v.v[OFPS_SET_TEMPORAL_EVERY].i;
    state.temporal.maxQueue = v.v[OFPS_SET_TEMPORAL_MAX_QUEUE].i; state.temporal.modelPasses = v.v[OFPS_SET_MODEL_PASSES].i;
    state.temporal.spreadPasses = v.v[OFPS_SET_SPREAD_PASSES].i != 0;
    auto effective = c;
    effective.mode = static_cast<ofps::sdk::WarpMode>(v.v[OFPS_SET_MODE].i);
    effective.colorFilter = static_cast<ofps::sdk::ColorFilter>(v.v[OFPS_SET_COLOR_FILTER].i);
    effective.xAxis.centerPercent = v.v[OFPS_SET_CENTER_X].f; effective.yAxis.centerPercent = v.v[OFPS_SET_CENTER_Y].f;
    effective.xAxis.workPercent = v.v[OFPS_SET_WORK_X].f; effective.yAxis.workPercent = v.v[OFPS_SET_WORK_Y].f;
    effective.globalScalePercent = v.v[OFPS_SET_GLOBAL_SCALE].f;
    StoreConfig(effective);
}

void AdoptCoreValuesForOverlay() {
    if (DirectHostActive() || !Core()) return;
    OfpsSettingsValues values{};
    values.size = sizeof(values);
    Core()->GetSettings(&values);
    if (values.count != OFPS_SET_COUNT || DirectHostActive()) return;
    auto &state = State();
    state.values = values;
    state.workShiftEnabled = values.v[OFPS_SET_WORK_SHIFT_ENABLED].i != 0;
    state.showCenterOutline = values.v[OFPS_SET_SHOW_CENTER_OUTLINE].i != 0;
    state.showWorkOutline = values.v[OFPS_SET_SHOW_WORK_OUTLINE].i != 0;
    state.brightnessPercent = values.v[OFPS_SET_BRIGHTNESS].f;
    state.gamma = values.v[OFPS_SET_GAMMA].f;
    state.temporal.mode = values.v[OFPS_SET_TEMPORAL_MODE].i;
    state.temporal.every = values.v[OFPS_SET_TEMPORAL_EVERY].i;
    state.temporal.maxQueue = values.v[OFPS_SET_TEMPORAL_MAX_QUEUE].i;
    state.temporal.modelPasses = values.v[OFPS_SET_MODEL_PASSES].i;
    state.temporal.spreadPasses = values.v[OFPS_SET_SPREAD_PASSES].i != 0;

    ofps::sdk::Status ignored{};
    auto config = CurrentConfig(ignored);
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
    StoreConfig(config);
}
} // namespace ofps::reshade
