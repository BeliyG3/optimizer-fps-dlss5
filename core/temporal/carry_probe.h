#pragma once

#include "core/temporal/machine.h"

namespace ofps::core::temporal {
// DebugTemporalCarryProbe (bench attribution of the carry under camera motion, phase 3 step 0).
constexpr int kProbeOracle = 1, kProbeAcceptAll = 2, kProbeBilinear = 4;
// The mode a caller passes where the probe must not apply: the fallback's colour copy and the spread stages, whose
// paths never decide the frame's effective mode.
constexpr int kModeNoProbe = -1;
// The oracle replaces the chain with the bench's offset to the last full pass; only the synchronous mode's full
// passes are frames the bench can know, so mode 3 (and 0, which has no chain) ignores it.
constexpr bool OracleChainActive(int effectiveMode, int probe) { return effectiveMode == 1 && (probe & kProbeOracle) != 0; }

// The probe's bits and the dump window on one frame's inputs (controller.cpp's TemporalInputs); nothing for
// kModeNoProbe. An oracle bit outside mode 1 is logged once and ignored.
void ApplyCarryProbe(FrameInputs &t, int effectiveMode, int probe, int dumpAccFrom);
} // namespace ofps::core::temporal
