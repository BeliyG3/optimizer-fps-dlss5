#pragma once

// Living next to a public OptiScaler build (OptiScaler loads ReShade with [Plugins] LoadReshade=true).
//
// OptiScaler's menu cannot host another add-on's settings, so while that menu is open (its own
// shortcut, [Menu] ShortcutKey, Insert by default) this add-on draws its settings in a window of
// its own beside it. The window is drawn through ReShade's reshade_overlay event, which runs every
// frame with the ReShade overlay closed too; ReShade reads the mouse from the window messages before
// the game's (and OptiScaler's) window procedure sees them, so the window takes clicks while
// OptiScaler blocks the game's input.
//
// The same probe reads OptiScaler's own NR settings that would duplicate this add-on's work.

namespace reshade::api {
struct effect_runtime;
}

namespace ofps::reshade {

struct OptiScalerLink {
    bool present = false;
    bool ownCompression = false; // [DlssNr] SpatialCompression=true (wilsjo2 v0.8.9+)
    int ownPasses = 1;           // [DlssNr] Passes
    unsigned shortcutKey = 0x2D; // [Menu] ShortcutKey as a virtual-key code (VK_INSERT by default)
};

// The module is looked up once, on the first present; its OptiScaler.ini is re-read every 2 s
// (OptiScaler writes it when its settings are saved).
const OptiScalerLink &OptiScalerState();

// First present: when OptiScaler is in the process, registers the window drawn beside its menu.
void OptiScalerLinkInit();
void OptiScalerLinkShutdown();

} // namespace ofps::reshade
