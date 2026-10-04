#pragma once
// The core's motion source while menu runs pass frames through it (Task 13, corrections C5/C6): optical flow from a run's
// first core frame, the game's vectors again at the run's end (EndRun). Fix round 1 (Codex I1): a switch-back the core
// refused stays owed; the watched feature's host evaluates retry it before the core runs them and are withheld while it
// still fails, so no game frame runs with the menu's optical flow. Pure logic: the caller passes the core's switch and
// logs (menu_core_pass.cpp). Not thread-safe: the caller holds the menu pipeline's lock.
#include <functional>

namespace ofps::reshade {

class MenuFlowSwitch {
public:
    // The core's OfpsSetTemporalMotionSourceV1 for one core (0: the game's vectors, 1: optical flow); returns OFPS_OK (0)
    // or the core's error. Empty: no core, or a core DLL without the switch.
    using Setter = std::function<int(unsigned source)>;
    enum class Outcome {
        Unchanged, // already as asked
        Switched,  // the core now uses the asked source
        NoSwitch,  // no setter: switching on is impossible; switching off counts as done (no core runs the flow)
        Refused,   // the core refused; the state is unchanged (switching off: the restore is owed)
    };
    Outcome Set(bool on, const Setter &setter);

    enum class Gate {
        Call,     // the game's vectors are the source (no restore was owed)
        Restored, // an owed restore succeeded now: call the core
        Withhold, // the core still uses optical flow: withhold this evaluate, retry at the next
    };
    // Before a host evaluate reaches the core, when no menu run is active.
    Gate BeforeHostEvaluate(const Setter &setter);

    bool On() const { return on_; } // the core's source is optical flow (a run switched it on, no restore succeeded yet)
    bool RestoreOwed() const { return owed_; } // a switch-back was refused and has not succeeded since

private:
    bool on_ = false;
    bool owed_ = false;
};

} // namespace ofps::reshade
