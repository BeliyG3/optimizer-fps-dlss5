#pragma once

// Tells the frame-generation presenter when ReShade's overlay is open.
//
// The presenter's window is TOPMOST and not click-through, so while it covers the game every click
// lands on it instead of on the overlay underneath. It therefore hides itself for as long as an
// overlay is open, and learns about that from the named event Local\DLSS5_ReShadeOverlay_<pid of
// the game>. In a 64-bit game the add-on itself publishes it; in a 32-bit game the add-on runs in
// host64 and its process id is the wrong one, so the tab inside the game publishes it instead.
//
// Older presenters watched the Home key themselves. That is what this replaces: the key belongs to
// ReShade, and a presenter that reads it guesses at a state it can simply be told.

namespace pw_remote {

// Registers the ReShade overlay callback. Call once, at add-on registration.
void OverlayEventRegister();

// Resets and closes the event. A signalled event left behind would tell the presenter the overlay
// is still open after the add-on is gone.
void OverlayEventUnregister();

} // namespace pw_remote
