#pragma once

// The OptiScaler layout bridge (stage 27.D2, moved from producer.cpp).
//
// When a consumer that applies the warp itself (OptiScaler) exports PeripheralWarpLayoutBridgeV1, its
// values are the truth: overlay edits are forwarded to it and its own edits are pulled back, so both
// overlays agree. With the takeover (the default, see AddonState::optiTakeover) the add-on keeps
// hooking feature 18 instead and forces OptiScaler's own spatial warp Off through the same bridge, so
// the frame is warped once and all the add-on's features work in OptiScaler games.
//
// Everything here runs on ReShade's runtime thread (present and overlay callbacks), which is why the
// bridge state needs no lock of its own.

#include "peripheral_warp/layout_bridge_v1.h"
#include "peripheral_warp/types_v2.h"

namespace pw_addon {

// Applies an edited layout: forwarded to the consumer's bridge when one is linked (its rules
// decide), otherwise stored here; an accepted layout is persisted. Runtime-thread only.
pw::Status ApplyConfig(const pw::ConfigV2 &config);

// Looks for the bridge export in the loaded modules; gives up after a few hundred presents.
void ProbeLayoutBridge();
// Adopts the consumer's layout when its generation moved (or right after linking); in takeover mode
// it instead keeps the consumer's own warp switched Off.
void PullLayoutFromBridge();

PeripheralWarpLayoutStateV1 ToBridgeState(const pw::ConfigV2 &config);
void FromBridgeState(const PeripheralWarpLayoutStateV1 &state, pw::ConfigV2 &config);

// A consumer's bridge is linked in this process.
bool BridgeLinked();
// The last layout the overlay (or an export) applied was refused by the consumer.
bool BridgeLayoutRejected();
// The consumer's own spatial warp has been switched Off at least once (takeover).
bool BridgeForcedWarpOff();

// The takeover checkbox in the tab: remembers the choice, persists it and hands the layout over in
// whichever direction the new mode needs.
void SetOptiScalerTakeover(bool takeover);

} // namespace pw_addon
