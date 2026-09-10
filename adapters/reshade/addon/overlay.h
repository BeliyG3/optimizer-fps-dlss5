#pragma once

// The Optimizer FPS tab (stage 27.D2, moved from producer.cpp).
//
// 26.13: the settings are drawn inside ReShade's Add-ons tab by default (register_overlay with a null
// title), like the other DLSS 5 add-ons, so the tab is found where users look for it and cannot get
// lost in a hidden dock node. [PeripheralWarp] FloatingWindow=1 adds the old separate window as well,
// and that one is the only difference between the two callbacks below: the floating window is resized
// to its content, the embedded one never touches the host window.
//
// The body is drawn in the order the sections appear in the tab; the section functions inside
// overlay.cpp are a pure extraction, so the sequence of ImGui calls and every widget label - which is
// what ImGui hashes its IDs and its saved window state from - is exactly what it was.

namespace reshade::api {
struct effect_runtime;
}

namespace pw_addon {

// [PeripheralWarp] FloatingWindow=1: the old separate ReShade window.
void DrawOverlay(reshade::api::effect_runtime *runtime);
// The default: inside ReShade's Add-ons tab.
void DrawOverlayEmbedded(reshade::api::effect_runtime *runtime);

} // namespace pw_addon
