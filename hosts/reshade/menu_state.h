#pragma once
// Menu mode's entry and exit rule and the tab's one-line status (pure: no D3D, no ReShade; tests/test_menu_state.cpp).
// Entry (spec section 9, ruling 2026-09-28): the checkbox on, a snapshot ready, nothing blocking, and at least
// entryMs since the last host evaluate of the watched feature and kEntryPresents presents after the present that follows
// that evaluate (the evaluate's own frame present does not count: entry is at the 6th present at the earliest). A run ends at the next host
// evaluate, or at once when the checkbox goes off, the snapshot goes away, a blocker appears, the run is suspended (a
// model pass too slow: until the next host evaluate) or menu mode is stopped (a GPU fence error: for the session).
//
// Blockers are passed on every present: a null pointer means nothing blocks, a reason ends a run at once and shows in the
// tab; retryable = true adds "(retried automatically)". The reasons the pipeline raises are transient ones such as an HDR10
// swap chain or "waiting for the host's NR queue" (retryable) and permanent ones such as an unsupported model shape.
//
// Owed reset (ruling 2026-09-29): the NR history must be reset on the first host evaluate after ANY run that submitted a
// pass, however the run ended (host evaluate, checkbox off, blocker, suspension, release, stop). The pipeline calls
// NotePassSubmitted() when a pass is submitted and TakeResetOwed() in the host evaluate hook, so no end-of-run path
// has to remember it. If the evaluate that took the reset fails or is skipped, RestoreResetOwed() owes it again.
//
// Not thread-safe (std::string members): the pipeline's lock covers every call, from the host, present and tab threads.
// Active() stays true after SetEnabled(false) until the next Present ends the run (step.left); use Status() for the tab.
#include <string>

namespace ofps::reshade {

enum class MenuStatus { Off, Waiting, Active, Unavailable };

struct MenuStep {
    bool enter = false; // this present starts a run
    bool run = false;   // this present gets a menu pass
    bool left = false;  // a run ended on this present without a host evaluate
};

class MenuStateMachine {
public:
    static constexpr unsigned kEntryPresents = 5;
    explicit MenuStateMachine(double entryMs = 150.0) : entryMs_(entryMs) {}
    void SetEntryMs(double ms) { entryMs_ = ms; }
    void SetEnabled(bool on) { enabled_ = on; }
    bool Enabled() const { return enabled_; }
    bool HostEvaluated(double nowMs); // a host evaluate of the watched feature; true: it ended a run
    // One present. ready: a snapshot of the watched feature exists. blocker: why a pass cannot run now (null: nothing);
    // retryable: the blocker may go away by itself (the tab says it is retried).
    MenuStep Present(double nowMs, bool ready, const char *blocker, bool retryable);
    void SuspendRun(const char *reason);
    // A run ends for a reason that does not outlast this present (a resize, a swap chain's teardown): the next allowed
    // present may enter again at once, without a host evaluate.
    void Interrupt() { active_ = false; }
    // A blocker found after this present's Present (a run's entry meeting "waiting for the host's NR queue"): the run
    // ends and the tab shows the blocker at once; the next Present sets the blocker again from what it is given.
    void Block(const char *reason, bool retryable) {
        active_ = false;
        blocker_ = reason ? reason : "blocked";
        retryable_ = retryable;
    }
    void Stop(const char *reason);
    void Forget(); // the watched feature was released
    void NotePassSubmitted() { resetOwed_ = true; } // a menu pass went to the GPU: the next host evaluate resets NR history
    bool ResetOwed() const { return resetOwed_; }
    bool TakeResetOwed();                            // true once per owed reset: the host evaluate hook adds Reset=1
    void RestoreResetOwed() { resetOwed_ = true; }   // the evaluate that took the reset failed or was skipped: owe it again
    bool Active() const { return active_; }
    bool Stopped() const { return stopped_; }
    unsigned QuietPresents() const { return quiet_; }
    double MsSinceEvaluate(double nowMs) const { return nowMs - lastEvaluateMs_; }
    MenuStatus Status() const;
    std::string StatusLine() const;

private:
    double entryMs_;
    bool enabled_ = false, active_ = false, evaluated_ = false, evaluatedSincePresent_ = false, stopped_ = false;
    bool ready_ = false, retryable_ = false, resetOwed_ = false;
    unsigned quiet_ = 0;
    double lastEvaluateMs_ = 0.0;
    std::string blocker_, suspended_, stopReason_;
};

} // namespace ofps::reshade
