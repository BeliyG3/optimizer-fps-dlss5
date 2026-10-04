#pragma once

// What a temporal machine is built for. The role decides which optional textures Resources::Create
// allocates and is part of Machine::Matches, so a machine never changes role without being rebuilt.

#include <cstdint>

namespace ofps::core::temporal {

enum class MachineRole : std::uint8_t {
    Synchronous = 0, // the host's machine on the host queue (temporal modes 1/2, spread's shown stage)
    Background = 1,  // the host's machine with the background job (temporal mode 3)
    Hidden = 2,      // a spread stage that is never shown: no blend, no phase-in, no history
};

struct RoleTextures {
    bool pendingChains = false;  // accP[2], expectP[2]: the chains to the frame of the pass in flight
    bool kickExpectation = false; // expectKick: the kick's lookup into the displayed residual
    bool residualMix = false;    // the frozen mix of an interrupted background phase-in
    bool residualOld = false;    // the previous pass's residual for the phase-in (PW_T_RAMP)
    bool history = false;        // the two preceding passes (history.h)
    bool previousResidual = false; // the second residual: the blend source of the next pass
};

constexpr RoleTextures TexturesFor(MachineRole role) noexcept {
    const bool background = role == MachineRole::Background;
    const bool shown = role != MachineRole::Hidden;
    RoleTextures t;
    t.pendingChains = background;
    t.kickExpectation = background;
    t.residualMix = background;
    t.residualOld = shown;
    t.history = shown;
    t.previousResidual = shown;
    return t;
}

} // namespace ofps::core::temporal
