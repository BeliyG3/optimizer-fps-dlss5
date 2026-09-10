#pragma once

// 26.16 crash guard (stage 27.D2, moved from producer.cpp).
//
// A marker file is written next to the add-on the moment the first warped frame went through (that
// is the moment a game that cannot take the warp dies) and removed on a clean unload. Finding the
// marker at start-up means the previous session of this game ended without unloading us: the add-on
// then forwards every NR call untouched (pw_ngx::SetSafeMode), says so in red in the tab and offers a
// retry, instead of killing the game at every launch. [PeripheralWarp] CrashGuard=0 disables it.

namespace pw_addon {

// Called once from DllMain with the directory the add-on was loaded from: reads the CrashGuard key,
// builds the marker path and decides whether the previous session counts as a crash (safe mode).
void CrashGuardInit(const wchar_t *directory);

// Registered with the hook: fired once, on the render thread, just before the first warped evaluate
// is recorded - so the marker exists even when that very evaluate kills the process.
void CrashMarkerOnFirstWarped();

// Called from present: refreshes the marker while the add-on is warping.
void CrashGuardOnPresent();

// The Retry button in the tab: warp again in this session.
void CrashGuardRetry();

// A clean unload (or a Retry): the next session may warp.
void CrashMarkerClear();

// Whether the marker found at start-up put this session into safe mode (drawn in the tab).
bool CrashGuardTripped();

} // namespace pw_addon
