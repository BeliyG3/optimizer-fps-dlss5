#include "core/settings/values.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
namespace ofps::core {
namespace {
float Finite(float value, float fallback) { return std::isfinite(value) ? value : fallback; }
} // namespace
int TemporalModeFromValue(int value) { return value == 3 ? 3 : (value < 0 ? 0 : (value > 1 ? 1 : value)); }
void ClampTemporal(TemporalSettings *t) {
    TemporalSettings &s = *t;
    s.modelPasses = std::clamp(s.modelPasses, 1, 3);
    s.mode = std::clamp(s.mode, 0, 3);
    s.every = std::clamp(s.every, s.mode == 3 ? 1 : 2, 8);
    s.maxAge = std::clamp(s.maxAge, 3, 16);
    s.maxQueue = std::clamp(s.maxQueue, 0, 8);
    if (!std::isfinite(s.depthTolerance))
        s.depthTolerance = 0.05f;
    s.depthTolerance = std::clamp(s.depthTolerance, 0.0f, 1.0f);
    if (!std::isfinite(s.colorTolerance))
        s.colorTolerance = 0.08f;
    s.colorTolerance = std::clamp(s.colorTolerance, 0.0f, 1.0f);
    if (!std::isfinite(s.mvSearchRadiusPx))
        s.mvSearchRadiusPx = 16.0f;
    s.mvSearchRadiusPx = std::clamp(s.mvSearchRadiusPx, 0.0f, 128.0f);
    if (!std::isfinite(s.smoothRadiusPx))
        s.smoothRadiusPx = 24.0f;
    s.smoothRadiusPx = std::clamp(s.smoothRadiusPx, 0.0f, 64.0f);
    s.debugView = std::clamp(s.debugView, 0, 3);
    if (s.mode != 0 && s.modelPasses > 1 && s.spreadPasses)
        s.every = std::max(s.every, s.modelPasses);
}
void DefaultSettingsValues(OfpsSettingsValues *out) {
    std::memset(out, 0, sizeof(*out));
    out->size = sizeof(OfpsSettingsValues);
    out->count = OFPS_SET_COUNT;
    for (std::uint32_t i = 0; i < OFPS_SET_COUNT; ++i)
        out->v[i] = kOfpsSettings[i].defaultValue;
}
void SettingsToValues(const CoreSettings &in, OfpsSettingsValues *out) {
    out->size = sizeof(OfpsSettingsValues);
    out->count = OFPS_SET_COUNT;
    OfpsSettingValue *v = out->v;
    const auto I = [&](std::uint32_t id, int value) { v[id].i = value; };
    const auto F = [&](std::uint32_t id, float value) { v[id].f = value; };
    const ofps::sdk::ConfigV2 &c = in.config;
    I(OFPS_SET_MODE, static_cast<int>(c.mode));
    I(OFPS_SET_COLOR_FILTER, static_cast<int>(c.colorFilter));
    F(OFPS_SET_CENTER_X, c.xAxis.centerPercent);
    F(OFPS_SET_CENTER_Y, c.yAxis.centerPercent);
    F(OFPS_SET_WORK_X, c.xAxis.workPercent);
    F(OFPS_SET_WORK_Y, c.yAxis.workPercent);
    F(OFPS_SET_GLOBAL_SCALE, c.globalScalePercent);
    I(OFPS_SET_FLAGS, static_cast<int>(c.flags));
    F(OFPS_SET_OFFSET_X, c.centerOffsetXPercent);
    F(OFPS_SET_OFFSET_Y, c.centerOffsetYPercent);
    F(OFPS_SET_WORK_SHIFT_X, c.workShiftXPercent);
    F(OFPS_SET_WORK_SHIFT_Y, c.workShiftYPercent);
    I(OFPS_SET_WORK_SHIFT_ENABLED, in.workShiftEnabled ? 1 : 0);
    I(OFPS_SET_SHOW_CENTER_OUTLINE, in.showCenterOutline ? 1 : 0);
    I(OFPS_SET_SHOW_WORK_OUTLINE, in.showWorkOutline ? 1 : 0);
    F(OFPS_SET_BRIGHTNESS, in.brightnessPercent);
    F(OFPS_SET_GAMMA, in.gamma);
    const TemporalSettings &t = in.temporal;
    I(OFPS_SET_TEMPORAL_MODE, t.mode);
    I(OFPS_SET_TEMPORAL_EVERY, t.every);
    I(OFPS_SET_TEMPORAL_MAX_QUEUE, t.maxQueue);
    I(OFPS_SET_MODEL_PASSES, t.modelPasses);
    I(OFPS_SET_SPREAD_PASSES, t.spreadPasses ? 1 : 0);
    const DiagnosticsConfig &d = in.diag;
    I(OFPS_SET_DEBUG_TIMING, d.timing ? 1 : 0);
    I(OFPS_SET_DEBUG_TEMPORAL_READBACK, d.temporalReadback ? 1 : 0);
    I(OFPS_SET_DEBUG_ASYNC_LOG, d.asyncLog ? 1 : 0);
    I(OFPS_SET_DEBUG_ASYNC_SHOW_PASS, d.asyncShowPass ? 1 : 0);
    I(OFPS_SET_DEBUG_ASYNC_COMPUTE, d.asyncCompute ? 1 : 0);
    I(OFPS_SET_DEBUG_ASYNC_NORMAL_PRIORITY, d.asyncNormalPriority ? 1 : 0);
    I(OFPS_SET_DEBUG_ASYNC_NO_REALTIME, d.asyncNoRealtime ? 1 : 0);
    I(OFPS_SET_DEBUG_HOOK_DELAY_MS, static_cast<int>(d.hookDelayMs));
    I(OFPS_SET_DEBUG_KEEP_BACKBUFFER, d.keepBackbuffer ? 1 : 0);
    I(OFPS_SET_DEBUG_DEPTH_STATE, d.depthState);
    I(OFPS_SET_DEBUG_TEMPORAL_VIS, t.debugView);
    I(OFPS_SET_DEBUG_TEMPORAL_KEEP_OUTPUT, d.temporalKeepOutput ? 1 : 0);
    F(OFPS_SET_DEBUG_TEMPORAL_BLEND, d.temporalBlend);
    F(OFPS_SET_DEBUG_TEMPORAL_DEPTH, d.temporalDepth);
    F(OFPS_SET_DEBUG_TEMPORAL_SMOOTH, d.temporalSmooth);
    I(OFPS_SET_DEBUG_MOTION_SMOOTH, d.motionSmooth ? 1 : 0);
    I(OFPS_SET_DEBUG_PASS_NO_HISTORY, d.passNoHistory ? 1 : 0);
    I(OFPS_SET_DEBUG_TEMPORAL_NO_MODEL_MOTION, d.temporalNoModelMotion ? 1 : 0);
    I(OFPS_SET_DEBUG_TEMPORAL_NO_EXPECT, d.temporalNoExpect ? 1 : 0);
    I(OFPS_SET_DEBUG_TEMPORAL_NO_CELLS, d.temporalNoCells ? 1 : 0);
    I(OFPS_SET_DEBUG_TEMPORAL_PHASE_IN, d.temporalPhaseIn);
    I(OFPS_SET_DEBUG_LAYER, d.debugLayerLog ? 1 : 0);
    I(OFPS_SET_DEBUG_WARP_PATH, in.debugWarpPath);
}
int ValuesToSettings(const OfpsSettingsValues &in, CoreSettings *io) {
    if (!io || in.size < offsetof(OfpsSettingsValues, v) || in.count > OFPS_SET_COUNT)
        return OFPS_E_ARG;
    CoreSettings s = *io;
    const auto available =
        static_cast<std::uint32_t>((in.size - offsetof(OfpsSettingsValues, v)) / sizeof(OfpsSettingValue));
    const std::uint32_t count = std::min(in.count, available);
    const auto has = [&](std::uint32_t id) { return id < count; };
    const auto I = [&](std::uint32_t id, int &field) {
        if (has(id))
            field = in.v[id].i;
    };
    const auto B = [&](std::uint32_t id, bool &field) {
        if (has(id))
            field = in.v[id].i != 0;
    };
    const auto F = [&](std::uint32_t id, float &field) {
        if (has(id))
            field = in.v[id].f;
    };
    ofps::sdk::ConfigV2 &c = s.config;
    if (has(OFPS_SET_MODE))
        c.mode = static_cast<ofps::sdk::WarpMode>(in.v[OFPS_SET_MODE].i);
    if (has(OFPS_SET_COLOR_FILTER))
        c.colorFilter = static_cast<ofps::sdk::ColorFilter>(in.v[OFPS_SET_COLOR_FILTER].i);
    F(OFPS_SET_CENTER_X, c.xAxis.centerPercent);
    F(OFPS_SET_CENTER_Y, c.yAxis.centerPercent);
    F(OFPS_SET_WORK_X, c.xAxis.workPercent);
    F(OFPS_SET_WORK_Y, c.yAxis.workPercent);
    F(OFPS_SET_GLOBAL_SCALE, c.globalScalePercent);
    if (has(OFPS_SET_FLAGS))
        c.flags = static_cast<std::uint32_t>(in.v[OFPS_SET_FLAGS].i);
    F(OFPS_SET_OFFSET_X, c.centerOffsetXPercent);
    F(OFPS_SET_OFFSET_Y, c.centerOffsetYPercent);
    F(OFPS_SET_WORK_SHIFT_X, c.workShiftXPercent);
    F(OFPS_SET_WORK_SHIFT_Y, c.workShiftYPercent);
    if (ofps::sdk::ValidateConfig(c) != ofps::sdk::Status::Ok)
        return OFPS_E_ARG;
    B(OFPS_SET_WORK_SHIFT_ENABLED, s.workShiftEnabled);
    B(OFPS_SET_SHOW_CENTER_OUTLINE, s.showCenterOutline);
    B(OFPS_SET_SHOW_WORK_OUTLINE, s.showWorkOutline);
    if (has(OFPS_SET_BRIGHTNESS))
        s.brightnessPercent = std::clamp(Finite(in.v[OFPS_SET_BRIGHTNESS].f, 0.0f), -20.0f, 20.0f);
    if (has(OFPS_SET_GAMMA)) {
        const float g = in.v[OFPS_SET_GAMMA].f;
        s.gamma = (std::isfinite(g) && g > 0.0f) ? std::clamp(g, 0.7f, 1.4f) : 1.0f;
    }
    TemporalSettings &t = s.temporal;
    if (has(OFPS_SET_TEMPORAL_MODE))
        t.mode = TemporalModeFromValue(in.v[OFPS_SET_TEMPORAL_MODE].i);
    I(OFPS_SET_TEMPORAL_EVERY, t.every);
    I(OFPS_SET_TEMPORAL_MAX_QUEUE, t.maxQueue);
    I(OFPS_SET_MODEL_PASSES, t.modelPasses);
    B(OFPS_SET_SPREAD_PASSES, t.spreadPasses);
    I(OFPS_SET_DEBUG_TEMPORAL_VIS, t.debugView);
    ClampTemporal(&t);
    DiagnosticsConfig &d = s.diag;
    B(OFPS_SET_DEBUG_TIMING, d.timing);
    B(OFPS_SET_DEBUG_TEMPORAL_READBACK, d.temporalReadback);
    B(OFPS_SET_DEBUG_ASYNC_LOG, d.asyncLog);
    B(OFPS_SET_DEBUG_ASYNC_SHOW_PASS, d.asyncShowPass);
    B(OFPS_SET_DEBUG_ASYNC_COMPUTE, d.asyncCompute);
    B(OFPS_SET_DEBUG_ASYNC_NORMAL_PRIORITY, d.asyncNormalPriority);
    B(OFPS_SET_DEBUG_ASYNC_NO_REALTIME, d.asyncNoRealtime);
    if (has(OFPS_SET_DEBUG_HOOK_DELAY_MS)) {
        const int ms = in.v[OFPS_SET_DEBUG_HOOK_DELAY_MS].i;
        d.hookDelayMs = ms > 0 ? static_cast<unsigned>(std::min(ms, 600000)) : 0u;
    }
    B(OFPS_SET_DEBUG_KEEP_BACKBUFFER, d.keepBackbuffer);
    if (has(OFPS_SET_DEBUG_DEPTH_STATE)) {
        const int st = in.v[OFPS_SET_DEBUG_DEPTH_STATE].i;
        d.depthState = (st >= 0 && st <= 4) ? st : 99;
    }
    B(OFPS_SET_DEBUG_TEMPORAL_KEEP_OUTPUT, d.temporalKeepOutput);
    const auto override = [&](std::uint32_t id, float &field, float lo, float hi) {
        if (!has(id))
            return;
        const float x = in.v[id].f;
        field = (std::isfinite(x) && x >= 0.0f) ? std::clamp(x, lo, hi) : -1.0f;
    };
    override(OFPS_SET_DEBUG_TEMPORAL_BLEND, d.temporalBlend, 0.0f, 0.9f);
    override(OFPS_SET_DEBUG_TEMPORAL_DEPTH, d.temporalDepth, 0.0f, 1.0f);
    override(OFPS_SET_DEBUG_TEMPORAL_SMOOTH, d.temporalSmooth, 0.0f, 128.0f);
    B(OFPS_SET_DEBUG_MOTION_SMOOTH, d.motionSmooth);
    B(OFPS_SET_DEBUG_PASS_NO_HISTORY, d.passNoHistory);
    // One ini key intentionally controls both motion suppression fields.
    B(OFPS_SET_DEBUG_TEMPORAL_NO_MODEL_MOTION, d.temporalNoModelMotion);
    B(OFPS_SET_DEBUG_TEMPORAL_NO_MODEL_MOTION, d.temporalNoBackgroundModelMotion);
    B(OFPS_SET_DEBUG_TEMPORAL_NO_EXPECT, d.temporalNoExpect);
    B(OFPS_SET_DEBUG_TEMPORAL_NO_CELLS, d.temporalNoCells);
    if (has(OFPS_SET_DEBUG_TEMPORAL_PHASE_IN)) {
        const int p = in.v[OFPS_SET_DEBUG_TEMPORAL_PHASE_IN].i;
        d.temporalPhaseIn = p >= 0 ? std::min(p, 8) : -1;
        d.temporalBackgroundPhaseIn = std::clamp(p, -1, 8);
    }
    B(OFPS_SET_DEBUG_LAYER, d.debugLayerLog);
    I(OFPS_SET_DEBUG_WARP_PATH, s.debugWarpPath);
    s.debugWarpPath = std::clamp(s.debugWarpPath, 0, 2);
    *io = s;
    return OFPS_OK;
}
bool SettingRange(const CoreSettings &s, std::uint32_t id, float *lo, float *hi) {
    if (id >= OFPS_SET_COUNT)
        return false;
    switch (id) {
    case OFPS_SET_OFFSET_X:
        *hi = ofps::sdk::MaximumCenterOffsetPercentV2(s.config.xAxis.centerPercent);
        *lo = -*hi;
        return true;
    case OFPS_SET_OFFSET_Y:
        *hi = ofps::sdk::MaximumCenterOffsetPercentV2(s.config.yAxis.centerPercent);
        *lo = -*hi;
        return true;
    case OFPS_SET_WORK_SHIFT_X:
        ofps::sdk::WorkShiftLimitsPercentV2(s.config, 0, lo, hi);
        return true;
    case OFPS_SET_WORK_SHIFT_Y:
        ofps::sdk::WorkShiftLimitsPercentV2(s.config, 1, lo, hi);
        return true;
    default:
        *lo = kOfpsSettings[id].minValue;
        *hi = kOfpsSettings[id].maxValue;
        return true;
    }
}
} // namespace ofps::core
