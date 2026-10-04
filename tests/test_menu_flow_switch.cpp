// The core's motion source during menu runs (menu_flow_switch.h, Task 13 fix round 1): a refused switch-back stays owed,
// the host evaluates are withheld until a retry succeeds, and a core without the switch never blocks the game.
#include "hosts/reshade/menu_flow_switch.h"
#include <cstdio>
#include <vector>

using namespace ofps::reshade;
namespace {
int failures = 0;
void Check(bool condition, const char *what) {
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", what); ++failures; }
}

// A core switch that records the sources asked for and refuses while `refuse` is set.
struct FakeCore {
    bool refuse = false;
    std::vector<unsigned> calls;
    MenuFlowSwitch::Setter Setter() {
        return [this](unsigned source) { calls.push_back(source); return refuse ? -3 : 0; };
    }
};

void TestRunSwitch() {
    MenuFlowSwitch s;
    FakeCore core;
    Check(s.BeforeHostEvaluate(core.Setter()) == MenuFlowSwitch::Gate::Call, "nothing owed before any run");
    Check(core.calls.empty(), "no switch call without a run");
    Check(s.Set(true, core.Setter()) == MenuFlowSwitch::Outcome::Switched && s.On(), "a run switches the flow on");
    Check(s.Set(true, core.Setter()) == MenuFlowSwitch::Outcome::Unchanged, "the run's next frames do not switch again");
    Check(s.Set(false, core.Setter()) == MenuFlowSwitch::Outcome::Switched && !s.On() && !s.RestoreOwed(), "the run's end switches back");
    Check(core.calls == std::vector<unsigned>({1u, 0u}), "one switch on, one switch back");
    Check(s.Set(false, core.Setter()) == MenuFlowSwitch::Outcome::Unchanged, "a second run end is a no-op");
}

void TestRefusedRestore() {
    MenuFlowSwitch s;
    FakeCore core;
    s.Set(true, core.Setter());
    core.refuse = true;
    Check(s.Set(false, core.Setter()) == MenuFlowSwitch::Outcome::Refused, "the core refuses the switch back");
    Check(s.On() && s.RestoreOwed(), "the refused restore is owed");
    Check(s.BeforeHostEvaluate(core.Setter()) == MenuFlowSwitch::Gate::Withhold, "the game's evaluate is withheld while it fails");
    Check(s.BeforeHostEvaluate(core.Setter()) == MenuFlowSwitch::Gate::Withhold, "and retried at the next evaluate");
    Check(core.calls.size() == 4, "every evaluate retries the restore");
    core.refuse = false;
    Check(s.BeforeHostEvaluate(core.Setter()) == MenuFlowSwitch::Gate::Restored, "a retry that succeeds lets the evaluate run");
    Check(!s.On() && !s.RestoreOwed() && core.calls.back() == 0u, "the game's vectors are back");
    Check(s.BeforeHostEvaluate(core.Setter()) == MenuFlowSwitch::Gate::Call, "nothing owed afterwards");
}

void TestRefusedSwitchOn() {
    MenuFlowSwitch s;
    FakeCore core;
    core.refuse = true;
    Check(s.Set(true, core.Setter()) == MenuFlowSwitch::Outcome::Refused && !s.On(), "a refused switch-on leaves the game's vectors");
    Check(!s.RestoreOwed(), "a refused switch-on owes nothing");
    Check(s.BeforeHostEvaluate(core.Setter()) == MenuFlowSwitch::Gate::Call, "the game's evaluates run");
}

void TestNoSwitch() {
    MenuFlowSwitch s;
    const MenuFlowSwitch::Setter none;
    Check(s.Set(true, none) == MenuFlowSwitch::Outcome::NoSwitch && !s.On(), "without the switch the flow never goes on");
    FakeCore core;
    s.Set(true, core.Setter());
    core.refuse = true;
    s.Set(false, core.Setter());
    Check(s.RestoreOwed(), "owed before the core went away");
    Check(s.BeforeHostEvaluate(none) == MenuFlowSwitch::Gate::Restored && !s.On() && !s.RestoreOwed(),
          "no core any more: nothing runs the flow, the evaluate is not withheld");
}
} // namespace

int main() {
    TestRunSwitch();
    TestRefusedRestore();
    TestRefusedSwitchOn();
    TestNoSwitch();
    if (failures) { std::fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    std::puts("menu flow switch: all checks passed");
    return 0;
}
