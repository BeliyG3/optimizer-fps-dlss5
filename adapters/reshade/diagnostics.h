#pragma once

// Diagnostic switches of the NGX interposer (26.26, stage 27.B5).
//
// Until 26.25 every one of these was an environment variable (`PW_NGX_*`) read lazily into a static
// local the first time its code path ran. They are now read once, at add-on load, from read-only
// `[PeripheralWarp] Debug*` keys in ReShade.ini (producer.cpp) and handed to the hook through
// `pw_ngx::SetDiagnostics`. Nothing writes these keys back: they exist only if a human (or the bench
// harness) put them there, so a saved layout can never resurrect a diagnostic switch.
//
// The configuration is set once, before the first evaluate, and read without a lock afterwards -
// changing a key therefore takes effect on the next game start, exactly as the environment
// variables did.

namespace pw_ngx {

struct DiagnosticsConfig {
    bool timing = false;              // DebugTiming: timestamp queries around the model's evaluate
    bool temporalReadback = false;    // DebugTemporalReadback: centre texels printed every 60th interpolated frame
    bool temporalKeepOutput = false;  // DebugTemporalKeepOutput: interpolated frames do not write the host Output
    float temporalBlend = -1.0f;      // DebugTemporalBlend: cross-pass residual weight (<0: the built-in 0.6)
    float temporalDepth = -1.0f;      // DebugTemporalDepth: depth tolerance override (<0: the setting's value)
    float temporalSmooth = -1.0f;     // DebugTemporalSmooth: compose radius override, 0 = off (<0: the setting's value)
    bool debugLayerLog = false;       // DebugLayer=1 (which also turns the D3D12 debug layer on): dump its messages
    bool asyncCompute = false;        // DebugAsyncCompute: background pass on a compute queue instead of a direct one
    bool asyncNormalPriority = false; // DebugAsyncNormalPriority: normal instead of high queue priority
    bool asyncNoRealtime = false;     // DebugAsyncNoRealtime: skip the GLOBAL_REALTIME request
    bool asyncLog = false;            // DebugAsyncLog: verbose background-pass log
    bool asyncShowPass = false;       // DebugAsyncShowPass: show the last pass's output as is (no reprojection)
    unsigned hookDelayMs = 0;         // DebugHookDelayMs: delay the hook so a host creates its model first
    bool keepBackbuffer = false;      // DebugKeepBackbuffer: hand the native UI/back buffer to the warped model
    int depthState = 99;              // DebugDepthState: host depth resting state (99 = follow the automatic rule)
};

// Call once, at add-on load, before any evaluate can run.
void SetDiagnostics(const DiagnosticsConfig &config);

} // namespace pw_ngx
