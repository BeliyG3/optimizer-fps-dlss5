#include "core/temporal/profile.h"
#include "core/temporal/diagnostics.h"
#include "core/api/ofps_core.h"

#include <cmath>
#include <cstdio>

namespace {
bool Near(float a, float b) { return std::fabs(a - b) < 1e-6f; }
bool Check(bool ok, const char* what) {
    if (!ok) std::fprintf(stderr, "%s\n", what);
    return ok;
}
}

int main() {
    using namespace ofps::temporal;
    TemporalOverrides none{};
    const auto display = ResolveProfile(OFPS_COLOR_DISPLAY_REFERRED, false, none);
    const auto hdr = ResolveProfile(OFPS_COLOR_LINEAR_HDR, false, none);
    if (!Check(Near(display.colourTolerance, 0.08f) && Near(display.residualBlend, 0.6f) &&
               Near(display.ratioScale, 0.0f) && !display.lumaBand && !display.fillFloor,
               "display profile")) return 1;
    if (!Check(Near(hdr.colourTolerance, 0.16f) && Near(hdr.residualBlend, 0.8f) &&
               Near(hdr.ratioScale, 0.01f) && hdr.lumaBand && !hdr.fillFloor,
               "HDR profile")) return 1;
    if (!Check(!SameProfile(display, hdr), "domain switch must reset history")) return 1;
    const auto hdrFloor = ResolveProfile(OFPS_COLOR_LINEAR_HDR, true, none);
    if (!Check(hdrFloor.fillFloor && !hdr.fillFloor && !SameProfile(hdr, hdrFloor),
               "fill floor is a bool switch and must reset history")) return 1;
    none.fillFloor = false;
    none.colourTolerance = 0.1f;
    none.residualBlend = 0.7f;
    none.lumaBand = false;
    const auto overridden = ResolveProfile(OFPS_COLOR_LINEAR_HDR, true, none);
    if (!Check(!overridden.fillFloor && !overridden.lumaBand &&
               Near(overridden.colourTolerance, 0.1f) && Near(overridden.residualBlend, 0.7f),
               "explicit overrides")) return 1;

    DiagnosticValuesV1 diagnostics{};
    diagnostics.size = sizeof(diagnostics);
    diagnostics.version = 1;
    diagnostics.count = static_cast<std::uint32_t>(kDiagnosticKeys.size());
    const auto floorId = static_cast<unsigned>(DiagnosticId::FillFloor);
    diagnostics.explicitMask = std::uint64_t{1} << floorId;
    diagnostics.values[floorId] = 1.0f;
    if (!Check(ValidateDiagnostics(diagnostics), "fill floor true")) return 1;
    diagnostics.values[floorId] = 0.0f;
    if (!Check(ValidateDiagnostics(diagnostics), "fill floor false")) return 1;
    diagnostics.values[floorId] = 0.2f;
    if (!Check(!ValidateDiagnostics(diagnostics), "fill floor rejects fractional value")) return 1;
    diagnostics.values[floorId] = 1.0f;
    diagnostics.explicitMask |= std::uint64_t{1} << static_cast<unsigned>(DiagnosticId::NoCells);
    if (!Check(!ValidateDiagnostics(diagnostics), "ABI key must use ABI settings")) return 1;
    return 0;
}
