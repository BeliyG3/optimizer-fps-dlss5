#include "core/temporal/carry_probe.h"

#include "core/log.h"

#include <algorithm>
#include <atomic>
#include <cstdint>

namespace ofps::core::temporal {

void ApplyCarryProbe(FrameInputs &t, int effectiveMode, int probe, int dumpAccFrom)
{
    if (effectiveMode == kModeNoProbe) return;
    t.oracleChain = OracleChainActive(effectiveMode, probe);
    if (t.oracleChain) { t.refine = false; t.expectedDepth = false; }
    t.acceptAll = (probe & kProbeAcceptAll) != 0;
    if (probe & kProbeBilinear) t.residualCatmullRom = false;
    t.dumpAccFrom = static_cast<std::uint32_t>(std::max(dumpAccFrom, 0));
    static std::atomic<bool> s_oracleIgnored{false};
    if ((probe & kProbeOracle) != 0 && !t.oracleChain && !s_oracleIgnored.exchange(true))
        Log(true, "Optimizer FPS NGX hook: DebugTemporalCarryProbe: oracle chain needs TemporalMode 1; ignored");
}

} // namespace ofps::core::temporal
