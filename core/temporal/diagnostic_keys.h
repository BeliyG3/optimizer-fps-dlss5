#pragma once

#include "core/api/ofps_settings_schema.h"

#include <array>
#include <cstdint>

namespace ofps::temporal {

enum class DiagnosticId : std::uint32_t {
    NoHistory, NoHistorySearch, Blend, PhaseIn,
    NoFill, NoCompose, NoRefine, NoCells,
    NoExpect, NoModelMotion, FillFloor, LumaLimit,
    Log, StatsEvery, View, GuideProbes,
    HistoryDiv, HistoryPasses, RawFull, Reset,
    NoLimit, DepthTolerance, ColourTolerance, Timing,
    Count
};

struct DiagnosticKey {
    const char* iniKey;
    int abiSettingId;
};

inline constexpr std::array<DiagnosticKey,
    static_cast<std::size_t>(DiagnosticId::Count)> kDiagnosticKeys{{
    {"DebugTemporalNoHistory", -1},
    {"DebugTemporalNoHistorySearch", -1},
    {"DebugTemporalBlend", OFPS_SET_DEBUG_TEMPORAL_BLEND},
    {"DebugTemporalPhaseIn", OFPS_SET_DEBUG_TEMPORAL_PHASE_IN},
    {"DebugTemporalNoFill", -1},
    {"DebugTemporalNoCompose", -1},
    {"DebugTemporalNoRefine", -1},
    {"DebugTemporalNoCells", OFPS_SET_DEBUG_TEMPORAL_NO_CELLS},
    {"DebugTemporalNoExpect", OFPS_SET_DEBUG_TEMPORAL_NO_EXPECT},
    {"DebugTemporalNoModelMotion", OFPS_SET_DEBUG_TEMPORAL_NO_MODEL_MOTION},
    {"TemporalFillFloor", -1},
    {"TemporalLumaLimit", -1},
    {"DebugTemporalLog", -1},
    {"DebugTemporalStatsEvery", -1},
    {"DebugTemporalView", -1},
    {"DebugTemporalGuideProbes", -1},
    {"DebugTemporalHistoryDiv", -1},
    {"DebugTemporalHistoryPasses", -1},
    {"DebugTemporalRawFull", -1},
    {"DebugTemporalReset", -1},
    {"DebugTemporalNoLimit", -1},
    {"TemporalDepthTolerance", -1},
    {"TemporalColourTolerance", -1},
    {"DebugTiming", OFPS_SET_DEBUG_TIMING},
}};

struct DiagnosticValuesV1 {
    std::uint32_t size;
    std::uint32_t version;
    std::uint32_t count;
    float values[kDiagnosticKeys.size()];
    std::uint64_t explicitMask;
};

static_assert(kDiagnosticKeys.size() <= 64);
using SetTemporalDiagnosticsV1 = int (*)(IOfpsCore*, const DiagnosticValuesV1*);

} // namespace ofps::temporal
