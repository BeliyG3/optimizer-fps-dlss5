// Menu mode's entry/exit rule and status line (menu_state.h).
#include "hosts/reshade/menu_state.h"
#include <cstdio>
#include <string>

using namespace ofps::reshade;
namespace {
int failures = 0;
void Check(bool condition, const char *what) {
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", what); ++failures; }
}
bool Contains(const std::string &text, const char *part) { return text.find(part) != std::string::npos; }

// A running game: host evaluates every 16 ms, each followed by its present; returns the time of the last present.
double Play(MenuStateMachine &s, double t, int frames) {
    for (int i = 0; i < frames; ++i) {
        s.HostEvaluated(t);
        const MenuStep step = s.Present(t + 1.0, true, nullptr, false);
        Check(!step.run && !step.enter, "no menu pass while the host evaluates");
        t += 16.0;
    }
    return t;
}

// Presents without host evaluates, 16 ms apart; returns the index (1-based) of the present that entered, 0 if none.
int Menu(MenuStateMachine &s, double &t, int presents) {
    int entered = 0;
    for (int i = 1; i <= presents; ++i) {
        const MenuStep step = s.Present(t, true, nullptr, false);
        if (step.enter) entered = i;
        Check(step.run == s.Active(), "a present runs exactly while a run is active");
        t += 16.0;
    }
    return entered;
}

void TestOffByDefault() {
    MenuStateMachine s;
    Check(s.Status() == MenuStatus::Off && s.StatusLine() == "Menu mode: off", "off until the checkbox is on");
    double t = 0;
    Check(Menu(s, t, 30) == 0, "never enters while off");
}

void TestEntryRule() {
    MenuStateMachine s;
    s.SetEnabled(true);
    double t = Play(s, 0.0, 10);
    Check(s.Status() == MenuStatus::Waiting && Contains(s.StatusLine(), "waiting for a menu"), "waiting while the game evaluates");
    const int entered = Menu(s, t, 20); // 16 ms apart: 150 ms need 10 presents, 5 presents come earlier
    Check(entered == 10, "enters at the first present that is >= 150 ms and >= 5 presents after the last host evaluate");
    Check(s.Status() == MenuStatus::Active && Contains(s.StatusLine(), "active"), "active in the menu");
    Check(s.HostEvaluated(t), "the next host evaluate ends the run");
    Check(!s.Active() && s.Status() == MenuStatus::Waiting, "back to waiting");
}

void TestEntryPresentsOnly() {
    MenuStateMachine s(0.0); // DebugMenuEntryMs=0: repeatable entries on the bench
    s.SetEnabled(true);
    double t = Play(s, 0.0, 3);
    Check(Menu(s, t, 10) == MenuStateMachine::kEntryPresents, "with 0 ms the 5th quiet present enters");
}

void TestDisableInsideRun() {
    MenuStateMachine s(0.0);
    s.SetEnabled(true);
    double t = Play(s, 0.0, 3);
    Menu(s, t, 6);
    Check(s.Active(), "in a run");
    s.SetEnabled(false);
    const MenuStep step = s.Present(t, true, nullptr, false);
    Check(step.left && !step.run && !s.Active() && s.Status() == MenuStatus::Off, "turning the checkbox off ends the run at once");
}

void TestReadyLost() {
    MenuStateMachine s(0.0);
    s.SetEnabled(true);
    double t = Play(s, 0.0, 3);
    Menu(s, t, 6);
    const MenuStep step = s.Present(t, false, nullptr, false); // the feature was released: no snapshot any more
    Check(step.left && !s.Active(), "a run ends when its snapshot is gone");
    Check(Contains(s.StatusLine(), "waiting for the game's Neural Rendering"), "and waits for the game's NR");
    s.Forget();
    Check(Menu(s, t, 20) == 0, "no entry without a host evaluate after a release");
}

void TestRetryableBlocker() {
    MenuStateMachine s(0.0);
    s.SetEnabled(true);
    double t = Play(s, 0.0, 3);
    for (int i = 0; i < 10; ++i) s.Present(t += 16.0, true, "HDR10 swap chain (not supported)", true);
    Check(!s.Active() && s.Status() == MenuStatus::Unavailable, "a blocker keeps menu mode out");
    Check(Contains(s.StatusLine(), "unavailable: HDR10 swap chain (not supported)") && Contains(s.StatusLine(), "retried"),
          "the tab names the reason and says it is retried");
    const MenuStep step = s.Present(t += 16.0, true, nullptr, false);
    Check(step.enter && s.Active(), "the blocker gone, the menu is entered on the next present");
}

void TestSuspendAndStop() {
    MenuStateMachine s(0.0);
    s.SetEnabled(true);
    double t = Play(s, 0.0, 3);
    Menu(s, t, 6);
    s.SuspendRun("the model needs more than 100 ms per menu frame on this GPU");
    Check(!s.Active() && s.Status() == MenuStatus::Unavailable && Contains(s.StatusLine(), "100 ms"), "a slow run is suspended");
    Check(Menu(s, t, 20) == 0, "the suspended run is not re-entered in the same menu");
    t = Play(s, t, 3);
    Check(Menu(s, t, 10) == MenuStateMachine::kEntryPresents, "the next menu tries again");
    s.Stop("stopped after a GPU fence error (see ReShade.log)");
    Check(s.Stopped() && !s.Active() && Contains(s.StatusLine(), "GPU fence error"), "a stop ends the run");
    t = Play(s, t, 3);
    Check(Menu(s, t, 20) == 0 && s.Status() == MenuStatus::Unavailable, "and holds for the session");
}

// Correction C2: the pipeline raises "waiting for the host's NR queue" as a retryable blocker; it ends a run at once,
// shows in the tab as retried automatically, and clears when the pipeline stops raising it.
void TestHostQueueBlocker() {
    const char *reason = "waiting for the host's NR queue";
    MenuStateMachine s(0.0);
    s.SetEnabled(true);
    double t = Play(s, 0.0, 3);
    Check(Menu(s, t, 6) != 0 && s.Active(), "in a run");
    const MenuStep left = s.Present(t += 16.0, true, reason, true);
    Check(left.left && !left.run && !s.Active(), "the blocker ends a run that is under way");
    Check(s.Status() == MenuStatus::Unavailable &&
              s.StatusLine() == "Menu mode: unavailable: waiting for the host's NR queue (retried automatically)",
          "the tab shows the queue blocker as retried automatically");
    for (int i = 0; i < 10; ++i) Check(!s.Present(t += 16.0, true, reason, true).enter, "no entry while the queue is unobserved");
    const MenuStep step = s.Present(t += 16.0, true, nullptr, false);
    Check(step.enter && s.Active() && s.Status() == MenuStatus::Active, "the queue observed, menu mode resumes in the same menu");
    // A non-retryable blocker is named without the retry note.
    s.Present(t += 16.0, true, "the model's shape is not supported", false);
    Check(Contains(s.StatusLine(), "not supported") && !Contains(s.StatusLine(), "retried"), "a permanent blocker has no retry note");
}

// Task 8's owed reset: every run that submitted a pass leaves a reset owed for the next host evaluate, however it ended.
void TestResetOwed() {
    MenuStateMachine s(0.0);
    s.SetEnabled(true);
    double t = Play(s, 0.0, 3);
    Check(!s.ResetOwed(), "nothing owed before any pass");
    Menu(s, t, 6);
    Check(!s.ResetOwed(), "a run without a submitted pass owes nothing");
    s.HostEvaluated(t);
    Check(!s.TakeResetOwed(), "and the next host evaluate takes no reset");

    // The run ends by a host evaluate.
    t = Play(s, t, 3);
    Menu(s, t, 6);
    s.NotePassSubmitted();
    Check(s.ResetOwed(), "a submitted pass owes a reset");
    Check(s.HostEvaluated(t) && s.ResetOwed(), "the ending host evaluate leaves the reset for its own model call");
    Check(s.TakeResetOwed() && !s.ResetOwed() && !s.TakeResetOwed(), "it is taken exactly once");

    // The run ends by the checkbox, a blocker, a stop, a suspension: the reset stays owed.
    t = Play(s, t, 3);
    Menu(s, t, 6);
    s.NotePassSubmitted();
    s.SetEnabled(false);
    s.Present(t, true, nullptr, false);
    Check(!s.Active() && s.ResetOwed(), "checkbox off keeps the reset owed");
    s.SetEnabled(true);
    Check(s.TakeResetOwed(), "taken at the next host evaluate");

    t = Play(s, t, 3);
    Menu(s, t, 6);
    s.NotePassSubmitted();
    s.Present(t, true, "HDR10 swap chain (not supported)", true);
    Check(!s.Active() && s.ResetOwed(), "a blocker keeps the reset owed");
    Check(s.TakeResetOwed(), "taken");

    t = Play(s, t, 3);
    Menu(s, t, 6);
    s.NotePassSubmitted();
    s.SuspendRun("slow");
    Check(s.ResetOwed(), "a suspension keeps the reset owed");
    Check(s.TakeResetOwed(), "taken");

    t = Play(s, t, 3);
    Menu(s, t, 6);
    s.NotePassSubmitted();
    s.Forget();
    Check(s.ResetOwed(), "a released feature keeps the reset owed (a re-created model resets harmlessly)");
    Check(s.TakeResetOwed(), "taken");

    t = Play(s, t, 3);
    Menu(s, t, 6);
    s.NotePassSubmitted();
    s.Stop("stopped");
    Check(s.ResetOwed(), "a stop keeps the reset owed");
    t = Play(s, t, 1);
    Check(s.TakeResetOwed() && !s.TakeResetOwed(), "and it is taken at the next host evaluate after a stop");
}

// A failed evaluate gives the reset back.
void TestRestoreResetOwed() {
    MenuStateMachine s(0.0);
    s.SetEnabled(true);
    s.NotePassSubmitted();
    Check(s.TakeResetOwed() && !s.ResetOwed(), "taken for an evaluate");
    s.RestoreResetOwed(); // that evaluate failed
    Check(s.ResetOwed(), "the reset is owed again");
    Check(s.TakeResetOwed() && !s.TakeResetOwed(), "and taken once by the next evaluate");
}

// The status line straight after the watched feature is released: no stale readiness or blocker.
void TestForgetStatus() {
    MenuStateMachine s(0.0);
    s.SetEnabled(true);
    double t = Play(s, 0.0, 3);
    Menu(s, t, 4);
    Check(s.StatusLine() == "Menu mode: waiting for a menu", "waiting for a menu with a snapshot");
    s.NotePassSubmitted();
    s.Forget();
    Check(s.Status() == MenuStatus::Waiting && s.StatusLine() == "Menu mode: waiting for the game's Neural Rendering",
          "right after Forget the line waits for the game's NR");
    Check(s.ResetOwed(), "Forget keeps the reset owed");

    s.Present(t += 16.0, true, "HDR10 swap chain (not supported)", true);
    Check(s.Status() == MenuStatus::Unavailable, "a blocker shows");
    s.Forget();
    Check(s.Status() == MenuStatus::Waiting && s.StatusLine() == "Menu mode: waiting for the game's Neural Rendering",
          "Forget drops the old blocker text");
}

// Entry needs >= entryMs: exactly at the threshold enters, just before does not.
void TestEntryBoundary() {
    MenuStateMachine s(150.0);
    s.SetEnabled(true);
    s.HostEvaluated(1000.0);
    Check(!s.Present(1000.0, true, nullptr, false).enter, "the present after the evaluate");
    for (double at = 1010.0; at <= 1050.0; at += 10.0) Check(!s.Present(at, true, nullptr, false).enter, "5 presents, too early");
    Check(s.QuietPresents() >= MenuStateMachine::kEntryPresents, "the presents condition holds");
    Check(!s.Present(1149.9, true, nullptr, false).enter, "149.9 ms does not enter");
    Check(s.Present(1150.0, true, nullptr, false).enter, "exactly 150 ms enters");
}

// The exact texts of the tab's line, and the precedence stop > blocker > suspension.
void TestStatusTexts() {
    MenuStateMachine s(0.0);
    s.SetEnabled(true);
    double t = Play(s, 0.0, 3);
    Menu(s, t, 6);
    Check(s.StatusLine() == "Menu mode: active (NR runs on the menu frame)", "the active line");
    s.SuspendRun("the model needs more than 100 ms per menu frame on this GPU");
    Check(s.StatusLine() == "Menu mode: unavailable: the model needs more than 100 ms per menu frame on this GPU (retried at the next menu)",
          "the suspension line");
    s.Present(t += 16.0, true, "HDR10 swap chain (not supported)", false);
    Check(s.StatusLine() == "Menu mode: unavailable: HDR10 swap chain (not supported)", "a blocker wins over a suspension");
    s.Stop("stopped after a GPU fence error (see ReShade.log)");
    Check(s.StatusLine() == "Menu mode: unavailable: stopped after a GPU fence error (see ReShade.log)", "a stop wins over a blocker");
    s.Present(t += 16.0, true, nullptr, false);
    Check(s.StatusLine() == "Menu mode: unavailable: stopped after a GPU fence error (see ReShade.log)", "and stays");
}

// Task 8 (C5): a resize or a swap chain's teardown ends a run for that present only; the next allowed present enters
// again without a host evaluate, the tab never shows "unavailable" for it, and an owed reset stays owed.
void TestInterrupt() {
    MenuStateMachine s(0.0);
    s.SetEnabled(true);
    double t = Play(s, 0.0, 3);
    Check(Menu(s, t, 6) != 0 && s.Active(), "in a run");
    s.NotePassSubmitted();
    s.Interrupt();
    Check(!s.Active() && s.Status() == MenuStatus::Waiting && Contains(s.StatusLine(), "waiting for a menu"),
          "an interrupted run is over and the tab waits");
    const MenuStep step = s.Present(t += 16.0, true, nullptr, false);
    Check(step.enter && step.run && !step.left && s.Active(), "the next allowed present enters again in the same menu");
    Check(s.ResetOwed(), "the owed reset survives the interruption");
    MenuStateMachine idle(0.0);
    idle.SetEnabled(true);
    idle.Interrupt();
    Check(!idle.Active() && idle.Status() == MenuStatus::Waiting, "interrupting without a run changes nothing");
}

// Task 8 fix round 1 (Codex minor): an entry that meets "waiting for the host's NR queue" ends the run and shows the
// blocker on the same present; the tab never reads "active" for a present without a pass.
void TestBlockAfterPresent() {
    const char *reason = "waiting for the host's NR queue";
    MenuStateMachine s(0.0);
    s.SetEnabled(true);
    double t = Play(s, 0.0, 3);
    Check(Menu(s, t, 6) != 0 && s.Active(), "in a run");
    s.Block(reason, true);
    Check(!s.Active() && s.Status() == MenuStatus::Unavailable &&
              s.StatusLine() == "Menu mode: unavailable: waiting for the host's NR queue (retried automatically)",
          "Block ends the run and the tab shows the blocker at once");
    Check(!s.Present(t += 16.0, true, reason, true).enter && s.Status() == MenuStatus::Unavailable, "held while the pipeline raises it");
    const MenuStep step = s.Present(t += 16.0, true, nullptr, false);
    Check(step.enter && s.Active(), "cleared by the next Present without it: entered again");
}
} // namespace

int main() {
    TestOffByDefault();
    TestEntryRule();
    TestEntryPresentsOnly();
    TestDisableInsideRun();
    TestReadyLost();
    TestRetryableBlocker();
    TestSuspendAndStop();
    TestHostQueueBlocker();
    TestResetOwed();
    TestRestoreResetOwed();
    TestForgetStatus();
    TestEntryBoundary();
    TestStatusTexts();
    TestInterrupt();
    TestBlockAfterPresent();
    if (failures == 0) std::printf("menu state: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
