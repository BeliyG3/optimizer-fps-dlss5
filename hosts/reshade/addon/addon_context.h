#pragma once

// The add-on's shared state (stage 27.D2, moved out of producer.cpp).
//
// Everything here is touched only from ReShade's runtime thread (the present and overlay callbacks)
// and therefore carries no lock of its own; the warp configuration itself is the exception and lives
// behind a mutex in config_store.h, because the NGX interposer reads it from the render thread.
//
// The fields below are the settings that are NOT part of ofps::sdk::ConfigV2: the diagnostic outlines, the
// output colour compensation, the motion-vector diagnostics and the temporal cadence. config_store.cpp persists the ones that are persisted; the modules that draw or
// apply them reach them through State().

#include "ini_schema.h"

#include <windows.h>

namespace ofps::reshade {

struct AddonState {
    // The add-on module locates the core DLL and dlss5-feed-host64.cfg.
    HMODULE module = nullptr;

    // Diagnostic outlines (Peripheral mode only): kept here rather than per effect runtime so they
    // survive ReShade re-creating the runtime (menus, fullscreen switches) and are saved with the layout.
    bool showCenterOutline = false;
    bool showWorkOutline = false;
    bool workShiftEnabled = false; // shows the Work-shift sliders; off = shift forced to 0

    // Motion-vector adjustment for the NGX interposer (Advanced): multiplier and sign flip on top of
    // the scale the host hands the model. Defaults leave the host's values untouched.
    float motionScaleAdjust = 1.0f;
    bool motionInvert = false;

    // Output colour compensation for the warped frame (the model's tone processing shifts slightly
    // with the frame scale): brightness percent and gamma, applied by Unpack.
    float brightnessPercent = 0.0f;
    float gamma = 1.0f;

    // Temporal NR modes (stage 24): the model every frame, or every N-th frame with reprojected
    // residual in between (synchronously or with the pass on a background queue).
    TemporalConfig temporal;
    OfpsSettingsValues values = SchemaDefaults();

    // Effective direct-host settings are separate from the local ini snapshot.
    OfpsSettingsValues directValues = SchemaDefaults();
    bool directValuesReady = false;


};

// The one instance. A function-local static in an inline function: one object across all the
// add-on's translation units, constructed before any ReShade callback can run.
using AddonConfig = AddonState;
inline AddonState &State()
{
    static AddonState state;
    return state;
}

} // namespace ofps::reshade
