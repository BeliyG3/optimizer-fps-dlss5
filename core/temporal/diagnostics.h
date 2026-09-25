#pragma once

#include "core/temporal/diagnostic_keys.h"

namespace ofps::temporal {

bool ValidateDiagnostics(const DiagnosticValuesV1& values) noexcept;
bool Explicit(const DiagnosticValuesV1& values, DiagnosticId id) noexcept;
float Value(const DiagnosticValuesV1& values, DiagnosticId id, float fallback) noexcept;

} // namespace ofps::temporal
