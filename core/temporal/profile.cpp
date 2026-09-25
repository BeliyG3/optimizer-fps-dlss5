#include "profile.h"

#include "core/api/ofps_core.h"

#include <cmath>
#include <stdexcept>

namespace ofps::temporal {
namespace {

float Choose(std::optional<float> value, float fallback, float lo, float hi) {
    const float result = value.value_or(fallback);
    if (!std::isfinite(result) || result < lo || result > hi)
        throw std::invalid_argument("temporal profile value");
    return result;
}

} // namespace

TemporalProfile ResolveProfile(std::uint32_t colorDomain, bool pwTuneW,
                               const TemporalOverrides& overrides) {
    if (colorDomain != OFPS_COLOR_DISPLAY_REFERRED &&
        colorDomain != OFPS_COLOR_LINEAR_HDR)
        throw std::invalid_argument("color domain");
    const bool hdr = colorDomain == OFPS_COLOR_LINEAR_HDR;
    return {
        Choose(overrides.colourTolerance, hdr ? 0.16f : 0.08f, 0.0f, 1.0f),
        Choose(overrides.depthTolerance, 0.05f, 0.0f, 1.0f),
        Choose(overrides.residualBlend, hdr ? 0.8f : 0.6f, 0.0f, 1.0f),
        Choose(overrides.ratioScale, hdr ? 0.01f : 0.0f, 0.0f, 1.0f),
        overrides.lumaBand.value_or(hdr),
        overrides.fillFloor.value_or(hdr && pwTuneW)
    };
}

bool SameProfile(const TemporalProfile& a, const TemporalProfile& b) noexcept {
    return a.colourTolerance == b.colourTolerance &&
           a.depthTolerance == b.depthTolerance &&
           a.residualBlend == b.residualBlend &&
           a.ratioScale == b.ratioScale &&
           a.lumaBand == b.lumaBand &&
           a.fillFloor == b.fillFloor;
}

} // namespace ofps::temporal
