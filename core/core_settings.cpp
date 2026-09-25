#include "core/core_impl.h"
#include "core/frame/feature_state.h"
#include "core/frame/spread_passes.h"
#include "core/temporal/diagnostics.h"
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <mutex>
namespace ofps::core {
int Core::SetTemporalDiagnostics(const ofps::temporal::DiagnosticValuesV1 *values) {
    if (!values || InCallback() || !ofps::temporal::ValidateDiagnostics(*values))
        return OFPS_E_ARG;
    std::lock_guard<std::mutex> lock(Ctx().mutex);
    if (hosts_.empty()) return OFPS_E_STATE;
    const auto &old = Ctx().temporalDiagnostics;
    bool changed = old.explicitMask != values->explicitMask;
    if (!changed) {
        for (unsigned i = 0; i < values->count; ++i)
            if (((values->explicitMask >> i) & 1ull) && old.values[i] != values->values[i]) {
                changed = true;
                break;
            }
    }
    Ctx().temporalDiagnostics = *values;
    if (changed) {
        for (auto &entry : Ctx().features) {
            auto &feature = *entry.second;
            RetireSpread(feature, RetirementGate(feature));
            if (feature.temporal) feature.temporal->Invalidate();
            if (feature.async) {
                gpu::GateSet gate;
                BuryAsync(feature, &gate);
            }
            feature.sinceFull = 0;
        }
    }
    return OFPS_OK;
}

void Core::ApplyTemporal(const TemporalSettings &s) {
    const bool multiChanged = (s.modelPasses > 1 || Ctx().temporal.modelPasses > 1) &&
                              (s.spreadPasses != Ctx().temporal.spreadPasses ||
                               s.modelPasses != Ctx().temporal.modelPasses || s.every != Ctx().temporal.every);
    const bool modeChanged = s.mode != Ctx().temporal.mode || multiChanged;
    Ctx().temporal = s;
    if (modeChanged) {
        for (auto &entry : Ctx().features) {
            RetireSpread(*entry.second, RetirementGate(*entry.second));
            entry.second->temporalDisabled = false;
            entry.second->temporalLogged = false;
            if (entry.second->temporal)
                entry.second->temporal->Invalidate();
            if (entry.second->async && s.mode != 3) {
                gpu::GateSet gate;
                BuryAsync(*entry.second, &gate);
            }
        }
        if (s.mode == 0)
            Ctx().status.temporalMode = 0;
    }
}
int Core::SetSettings(const OfpsSettingsValues *values) try {
    if (values == nullptr || values->size < offsetof(OfpsSettingsValues, v) || values->count > OFPS_SET_COUNT)
        return OFPS_E_ARG;
    if (InCallback())
        return OFPS_E_STATE;
    std::lock_guard<std::mutex> lock(Ctx().mutex);
    CoreSettings next = settings_;
    const int r = ValuesToSettings(*values, &next);
    if (r != OFPS_OK)
        return r;
    if (ModelResolutionHost())
        next.config.globalScalePercent = 100.0f;
    OfpsSettingsValues effective = Ctx().values;
    SettingsToValues(next, &effective);
    const uint32_t supplied =
        std::min<uint32_t>(values->count, (values->size - offsetof(OfpsSettingsValues, v)) / sizeof(OfpsSettingValue));
    for (uint32_t i = 0; i < supplied; ++i) {
        const auto word = i / 64;
        const uint64_t bit = 1ull << (i % 64);
        if (values->size >= offsetof(OfpsSettingsValues, explicitMask) + sizeof(uint64_t) * (word + 1))
            effective.explicitMask[word] = (effective.explicitMask[word] & ~bit) | (values->explicitMask[word] & bit);
    }
    const bool changed = std::memcmp(effective.v, Ctx().values.v, sizeof(effective.v)) != 0;
    settings_ = next;
    Ctx().values = effective;
    Ctx().config = next.config;
    ApplyTemporal(next.temporal);
    if (std::memcmp(&Ctx().diag, &next.diag, sizeof(DiagnosticsConfig)) != 0)
        Ctx().diag = next.diag;
    Ctx().showCenterOutline = next.showCenterOutline;
    Ctx().showWorkOutline = next.showWorkOutline;
    Ctx().outputGain = 1.0f + next.brightnessPercent * 0.01f;
    Ctx().outputGamma = next.gamma;
    if (changed) {
        OfpsEventData data{sizeof(OfpsEventData), &Ctx().values, nullptr, nullptr};
        Emit(OFPS_EVENT_SETTINGS_CHANGED, data);
    }
    return OFPS_OK;
} catch (...) {
    return OFPS_E_STATE;
}
void Core::GetSettings(OfpsSettingsValues *values) {
    if (InCallback() || values == nullptr || values->size < offsetof(OfpsSettingsValues, v))
        return;
    std::lock_guard<std::mutex> lock(Ctx().mutex);
    const auto capacity = values->size;
    const auto count =
        std::min<uint32_t>(OFPS_SET_COUNT, (capacity - offsetof(OfpsSettingsValues, v)) / sizeof(OfpsSettingValue));
    values->count = count;
    std::copy_n(Ctx().values.v, count, values->v);
    for (uint32_t word = 0; word < 2; ++word)
        if (capacity >= offsetof(OfpsSettingsValues, explicitMask) + sizeof(uint64_t) * (word + 1))
            values->explicitMask[word] = Ctx().values.explicitMask[word];
}
void Core::GetSettingRange(uint32_t settingId, float *lo, float *hi) {
    if (InCallback() || lo == nullptr || hi == nullptr)
        return;
    std::lock_guard<std::mutex> lock(Ctx().mutex);
    if (!SettingRange(settings_, settingId, lo, hi)) {
        *lo = 0.0f;
        *hi = 0.0f;
    }
}
void Core::SetHostMotionGrid(uint32_t blockSize) {
    if (InCallback())
        return;
    std::lock_guard lock(Ctx().mutex);
    Ctx().blockMotionGrid = (blockSize == 2 || blockSize == 4) ? static_cast<int>(blockSize) : 0;
}
int Core::SetTemporalMotionSource(std::uint32_t source) {
    if (source > 1) return OFPS_E_ARG;
    if (InCallback()) return OFPS_E_STATE;
    std::lock_guard lock(Ctx().mutex);
    if (hosts_.empty()) return OFPS_E_STATE;
    if (Ctx().temporalMotionSource.exchange(source) != source) {
        for (auto &entry : Ctx().features) {
            if (entry.second->temporal) entry.second->temporal->Invalidate();
            entry.second->sinceFull = 0;
        }
    }
    return OFPS_OK;
}
} // namespace ofps::core

extern "C" __declspec(dllexport) int OfpsSetTemporalDiagnosticsV1(
    IOfpsCore *core, const ofps::temporal::DiagnosticValuesV1 *values) {
    if (core != &ofps::core::CoreInstance()) return OFPS_E_ARG;
    return ofps::core::CoreInstance().SetTemporalDiagnostics(values);
}
extern "C" __declspec(dllexport) int OfpsSetTemporalMotionSourceV1(IOfpsCore *core, std::uint32_t source) {
    if (core != &ofps::core::CoreInstance()) return OFPS_E_ARG;
    return ofps::core::CoreInstance().SetTemporalMotionSource(source);
}
