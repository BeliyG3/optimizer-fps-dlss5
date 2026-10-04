#include "hosts/reshade/menu_state.h"

namespace ofps::reshade {

bool MenuStateMachine::HostEvaluated(double nowMs) {
    lastEvaluateMs_ = nowMs;
    evaluated_ = true;
    evaluatedSincePresent_ = true;
    suspended_.clear(); // a suspension lasts until the game runs NR again
    const bool ended = active_;
    active_ = false;
    return ended;
}

MenuStep MenuStateMachine::Present(double nowMs, bool ready, const char *blocker, bool retryable) {
    MenuStep step;
    if (evaluatedSincePresent_) { quiet_ = 0; evaluatedSincePresent_ = false; }
    else ++quiet_;
    ready_ = ready;
    retryable_ = retryable;
    blocker_ = blocker ? blocker : "";
    const bool allowed = enabled_ && !stopped_ && ready && blocker_.empty() && suspended_.empty();
    if (active_ && !allowed) {
        active_ = false;
        step.left = true;
        return step;
    }
    if (!active_ && allowed && evaluated_ && quiet_ >= kEntryPresents && nowMs - lastEvaluateMs_ >= entryMs_) {
        active_ = true;
        step.enter = true;
    }
    step.run = active_;
    return step;
}

void MenuStateMachine::SuspendRun(const char *reason) {
    suspended_ = reason ? reason : "suspended";
    active_ = false;
}

void MenuStateMachine::Stop(const char *reason) {
    stopped_ = true;
    stopReason_ = reason ? reason : "stopped";
    active_ = false;
}

void MenuStateMachine::Forget() {
    active_ = false;
    evaluated_ = false;
    evaluatedSincePresent_ = false;
    quiet_ = 0;
    suspended_.clear();
    ready_ = false; // no snapshot and no stale blocker in the status line right after a release; resetOwed_ is kept
    blocker_.clear();
    retryable_ = false;
}

bool MenuStateMachine::TakeResetOwed() {
    const bool owed = resetOwed_;
    resetOwed_ = false;
    return owed;
}

MenuStatus MenuStateMachine::Status() const {
    if (!enabled_) return MenuStatus::Off;
    if (stopped_ || !blocker_.empty() || !suspended_.empty()) return MenuStatus::Unavailable;
    return active_ ? MenuStatus::Active : MenuStatus::Waiting;
}

std::string MenuStateMachine::StatusLine() const {
    switch (Status()) {
    case MenuStatus::Off: return "Menu mode: off";
    case MenuStatus::Active: return "Menu mode: active (NR runs on the menu frame)";
    case MenuStatus::Waiting:
        return ready_ ? "Menu mode: waiting for a menu" : "Menu mode: waiting for the game's Neural Rendering";
    case MenuStatus::Unavailable: break;
    }
    if (stopped_) return "Menu mode: unavailable: " + stopReason_;
    if (!blocker_.empty()) return "Menu mode: unavailable: " + blocker_ + (retryable_ ? " (retried automatically)" : "");
    return "Menu mode: unavailable: " + suspended_ + " (retried at the next menu)";
}

} // namespace ofps::reshade
