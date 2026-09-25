#include "diagnostics.h"

#include <cmath>

namespace ofps::temporal {
namespace {

bool InRange(float value, float lo, float hi) noexcept {
    return std::isfinite(value) && value >= lo && value <= hi;
}

bool ValidValue(DiagnosticId id, float value) noexcept {
    switch (id) {
    case DiagnosticId::View: return InRange(value, 0.0f, 6.0f) && std::trunc(value) == value;
    case DiagnosticId::HistoryDiv: return InRange(value, 1.0f, 16.0f) && std::trunc(value) == value;
    case DiagnosticId::HistoryPasses: return InRange(value, 0.0f, 2.0f) && std::trunc(value) == value;
    case DiagnosticId::DepthTolerance:
    case DiagnosticId::ColourTolerance: return InRange(value, 0.0f, 1.0f);
    case DiagnosticId::Blend: return InRange(value, -1.0f, 0.9f);
    case DiagnosticId::PhaseIn: return InRange(value, -1.0f, 8.0f) && std::trunc(value) == value;
    default: return value == 0.0f || value == 1.0f;
    }
}

} // namespace

bool Explicit(const DiagnosticValuesV1& values, DiagnosticId id) noexcept {
    return ((values.explicitMask >> static_cast<unsigned>(id)) & 1ull) != 0;
}

float Value(const DiagnosticValuesV1& values, DiagnosticId id, float fallback) noexcept {
    return Explicit(values, id) ? values.values[static_cast<unsigned>(id)] : fallback;
}

bool ValidateDiagnostics(const DiagnosticValuesV1& values) noexcept {
    if (values.size != sizeof(values) || values.version != 1 ||
        values.count != kDiagnosticKeys.size() ||
        (values.explicitMask >> kDiagnosticKeys.size()) != 0)
        return false;
    for (unsigned i = 0; i < kDiagnosticKeys.size(); ++i) {
        if (((values.explicitMask >> i) & 1ull) == 0) continue;
        if (kDiagnosticKeys[i].abiSettingId >= 0 ||
            !ValidValue(static_cast<DiagnosticId>(i), values.values[i])) return false;
    }
    return true;
}

} // namespace ofps::temporal
