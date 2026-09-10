#pragma once

// The 64-bit half of the remote overlay, and the optical-flow config it edits (stage 27.D2, moved
// from producer.cpp).
//
// Remote overlay (stage 26.11). In the 32-bit kits the game runs ReShade x86 with dlss5-feed and
// the model runs in a 64-bit host process that owns this add-on; our tab therefore lives in a
// window the player never sees. `optimizer-fps-dlss5-remote.addon32`, loaded by the game's ReShade,
// draws the same tab and exchanges settings and status with us through a shared block. One writer
// per direction, publication by a generation counter written last. Runtime thread only.
//
// Optical flow in the 64-bit host (stage 26.16). When this add-on runs inside dlss5-feed-host64.exe
// the motion vectors handed to the model are computed by the host itself, and which source it uses
// is a file next to the exe that the host re-reads while it runs. So the tab writes that file: no
// restart, and the same three controls reach the 32-bit tab over the remote block.

#include "../pw_ofa_cfg.h"

namespace pw_addon {

// Called from present: applies whatever the game's tab changed, then republishes status and the
// applied settings into the shared block.
void RemotePublish();

// Detection and the first read of dlss5-feed-host64.cfg, retried while it fails (the host writes the
// file at start-up and this add-on can be loaded before that). Runtime thread only.
void OfaEnsureLoaded();
bool OfaLoaded();
// The settings as last read from (or written to) the file; only meaningful once OfaLoaded().
pw_ofa::Settings &OfaSettings();
// Writes them back; the host re-reads the file while it runs.
void OfaSave();

} // namespace pw_addon
