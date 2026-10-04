#pragma once
// Menu mode, stage 3 (Task 13): the core's motion source while menu runs pass frames through it, and the C6 verdict on
// its optical flow. Menus have no game motion, so the sync temporal cadence carries menu frames along NVIDIA optical
// flow: switched on at a run's first core frame (MenuCorePass), back to the game's vectors at every end of a run (EndRun,
// correction C5). The switch state is menu_flow_switch.h. Under the menu pipeline's lock, except MenuFlowMayBeOn.

namespace ofps::reshade {

// On at the run's first core frame with the sync temporal mode, off at every end of a run (idempotent). True: the
// core's motion source is now as asked. A failed switch-on sets MenuFlowProblem(); a failed switch-back stays owed and
// is retried by MenuFlowBeforeHostEvaluate.
bool MenuCoreFlow(bool on);
// Fix round 1 (Codex I1), before a host evaluate reaches the core while no menu run is active: an owed switch-back is
// retried. False: the core still uses the menu's optical flow; the evaluate is withheld (NGX failure code, logged once).
bool MenuFlowBeforeHostEvaluate(void *hostHandle);
// Lock-free hint for the host evaluate's early return: the core's source may be the menu's optical flow.
bool MenuFlowMayBeOn();

// C6: null while the sync cadence can carry menu frames along optical flow; else "optical flow is not available (...)"
// (the core has no switch, refused it, or its flow failed in a menu: latched for the session).
const char *MenuFlowProblem();
// The temporal mode menu frames run with: the user's (menu_settings.h), the sync mode falling back to every frame (0)
// while MenuFlowProblem() is set.
int MenuCadenceMode();
// After a core frame with the flow on: true when the core carried this frame with the optical flow field. False
// otherwise; a flow failure is latched (MenuFlowProblem): the core's own reason, or any carried frame without the field
// (fix round 1: it was carried along the menu's zero motion). DebugMenuNoFlow=2/3 fake that failure once per session, on
// the first carried frame from the run's `kMenuForcedFlowFailureFrame`-th core frame on.
bool MenuFlowCheckFrame(bool carried, unsigned runCoreFrames);
inline constexpr unsigned kMenuForcedFlowFailureFrame = 10; // the in-menu dump's present when no present reused a pass

} // namespace ofps::reshade
