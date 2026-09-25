#pragma once

#include <cstdint>
#include <optional>

namespace ofps::temporal {

struct TemporalOverrides {
    std::optional<float> colourTolerance;
    std::optional<float> depthTolerance;
    std::optional<float> residualBlend;
    std::optional<float> ratioScale;
    std::optional<bool> lumaBand;
    std::optional<bool> fillFloor;
};

struct TemporalProfile {
    float colourTolerance;
    float depthTolerance;
    float residualBlend;
    float ratioScale;
    bool lumaBand;
    bool fillFloor;
};

TemporalProfile ResolveProfile(std::uint32_t colorDomain,
                               bool pwTuneW,
                               const TemporalOverrides& overrides);
bool SameProfile(const TemporalProfile& a, const TemporalProfile& b) noexcept;

} // namespace ofps::temporal
