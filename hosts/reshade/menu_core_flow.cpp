#include "hosts/reshade/menu_core_flow.h"
#include "hosts/reshade/menu_flow_switch.h"
#include "hosts/reshade/menu_settings.h"
#include "hosts/reshade/shell_host.h"
#include "hosts/reshade/addon/shell_settings.h"
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <string>

namespace ofps::reshade {
namespace {
// The core's status row for the temporal optical flow (core/settings/status.cpp): severity 0 while the field is active,
// 1 with the reason when it is not (the second text: a full frame, whose field comes with the next frame).
constexpr const char *kFlowRow = "Temporal optical flow";
constexpr const char *kFlowWaiting = "waiting for the previous submitted frame";

struct Flow {
    MenuFlowSwitch source;
    std::string problem; // C6: why the cadence cannot use optical flow (for the session)
    bool loggedOn = false, loggedOff = false, loggedStuck = false, loggedWithheld = false;
    bool forcedFailure = false; // DebugMenuNoFlow=2/3 faked its failure (once per session)
};
Flow &F() { static Flow *flow = new Flow(); return *flow; } // never destroyed from DllMain
std::atomic<bool> g_mayBeOn{false};

void Log(const char *fmt, ...) {
    char text[384]; va_list args; va_start(args, fmt); std::vsnprintf(text, sizeof(text), fmt, args); va_end(args);
    Host().Log(OFPS_LOG_INFO, text);
}

bool Dumping() { return CurrentShellSettings().debugMenuDump != 0; }

void Latch(Flow &f, const char *why) {
    if (!f.problem.empty()) return;
    f.problem = std::string("optical flow is not available (") + why + ")";
    Log("menu core pass: %s; the sync temporal cadence is off in menus for the session: Mode Off runs the model on every "
        "menu frame, Uniform/Peripheral menus stay untouched", f.problem.c_str());
}

// The core's switch for the current core; empty without a core or without the export.
MenuFlowSwitch::Setter CoreSetter() {
    const auto set = CoreMotionSourceSetter();
    IOfpsCore *core = Core();
    if (!set || !core) return {};
    return [set, core](unsigned source) { return set(core, source); };
}

void Mirror(const Flow &f) { g_mayBeOn.store(f.source.On()); }
} // namespace

bool MenuCoreFlow(bool on) {
    Flow &f = F();
    const MenuFlowSwitch::Setter setter = CoreSetter();
    const MenuFlowSwitch::Outcome outcome = f.source.Set(on, setter);
    Mirror(f);
    switch (outcome) {
    case MenuFlowSwitch::Outcome::Unchanged: return true;
    case MenuFlowSwitch::Outcome::NoSwitch: // switching off: no core any more, nothing runs the flow
        if (on && Core()) Latch(f, "the core DLL has no optical flow switch");
        return !on;
    case MenuFlowSwitch::Outcome::Refused:
        if (on) Latch(f, "the core refused the optical flow switch");
        else if (!f.loggedStuck) {
            f.loggedStuck = true;
            Log("menu core pass: the core refused to switch back to the game's motion vectors; the watched feature's host "
                "evaluates retry it before the core runs them and are withheld while it fails (logged once)");
        }
        return false;
    case MenuFlowSwitch::Outcome::Switched: break;
    }
    bool &logged = on ? f.loggedOn : f.loggedOff;
    if (!logged || Dumping()) {
        logged = true;
        Log(on ? "menu core pass: optical flow on for the sync temporal cadence (menus carry no game motion)"
               : "menu core pass: optical flow off at the run's end; the game's motion vectors again");
    }
    return true;
}

bool MenuFlowBeforeHostEvaluate(void *hostHandle) {
    Flow &f = F();
    if (!f.source.On()) return true; // every game frame: nothing owed, no core call
    const MenuFlowSwitch::Gate gate = f.source.BeforeHostEvaluate(CoreSetter());
    Mirror(f);
    switch (gate) {
    case MenuFlowSwitch::Gate::Call: return true;
    case MenuFlowSwitch::Gate::Restored:
        f.loggedWithheld = f.loggedStuck = false;
        Log("menu core pass: the game's motion vectors are back (switched at the host evaluate of feature %p)", hostHandle);
        return true;
    case MenuFlowSwitch::Gate::Withhold: break;
    }
    if (!f.loggedWithheld) {
        f.loggedWithheld = true;
        Log("menu mode: the game's NR evaluate of feature %p is withheld (NGX failure code): the core still uses the menu's "
            "optical flow; retried at every evaluate (logged once)", hostHandle);
    }
    return false;
}

bool MenuFlowMayBeOn() { return g_mayBeOn.load(); }

const char *MenuFlowProblem() {
    Flow &f = F();
    // Reviewer minor 7: a core not attached yet (or any more) says nothing about its export.
    if (f.problem.empty() && Core() && !CoreMotionSourceSetter()) Latch(f, "the core DLL has no optical flow switch");
    if (f.problem.empty() && CurrentShellSettings().debugMenuNoFlow == 1) Latch(f, "forced by DebugMenuNoFlow=1");
    return f.problem.empty() ? nullptr : f.problem.c_str();
}

int MenuCadenceMode() {
    const int mode = MenuTemporalMode();
    return mode == 1 && MenuFlowProblem() ? 0 : mode;
}

bool MenuFlowCheckFrame(bool carried, unsigned runCoreFrames) {
    Flow &f = F();
    bool active = false;
    std::string reason; // copied at once: the core's status text lives in a buffer it rewrites on the next call
    if (IOfpsCore *core = Core()) {
        OfpsStatusRow rows[48]{};
        const std::uint32_t count = core->StatusLines(rows, 48);
        for (std::uint32_t i = 0; i < count; ++i) {
            if (!rows[i].label || std::string(rows[i].label) != kFlowRow) continue;
            active = rows[i].severity == 0;
            if (rows[i].value) reason = rows[i].value;
            break;
        }
    }
    const int forced = CurrentShellSettings().debugMenuNoFlow;
    if (active && carried && forced >= 2 && !f.forcedFailure && runCoreFrames >= kMenuForcedFlowFailureFrame) {
        f.forcedFailure = true;
        active = false;
        reason = forced == 2 ? "forced by DebugMenuNoFlow=2" : "forced by DebugMenuNoFlow=3";
    }
    if (active) return true;
    const bool waiting = reason.empty() || reason == kFlowWaiting;
    if (carried) Latch(f, waiting ? "the core carried a menu frame without the optical flow field" : reason.c_str());
    else if (!waiting) Latch(f, reason.c_str());
    return false;
}

} // namespace ofps::reshade
