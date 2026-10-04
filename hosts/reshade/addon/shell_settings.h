#pragma once

namespace ofps::reshade {

// Menu mode's pass (DebugMenuPass): the model is the product; the others check the pipeline's ordering and its
// failure paths on the bench.
enum class MenuPassKind { Model = 0, Marker = 1, ForcedRefusal = 2, ForcedSignalFailure = 3 };

struct ShellSettings {
    bool passive = false;
    bool floatingWindow = false;
    bool traceExit = false;
    bool debugLayer = false;
    bool debugLayerPresent = false;
    // Menu mode diagnostics ([OptimizerFPS] Debug* keys, read once, never written). The checkbox is the schema's MenuMode.
    MenuPassKind debugMenuPass = MenuPassKind::Model; // 1 a red 64x64 marker square, 2 every pass refused, 3 the f2
                                                      // signal of the first run's 10th menu present skipped (D3D12)
    int debugMenuDump = 0;      // 1: menu_events.log and menu_dump_*.bmp next to the exe, a log line per menu run
    int debugMenuEntryMs = 150; // entry: ms without a host evaluate (with 5 presents); 0: presents only (repeatable)
    int debugMenuOwnBlock = 0;  // 1: host model evaluates run with menu mode's own block (hosts that never pause)
    // 1: menus act as if the core had no optical flow (C6's fallback on a GPU that has it). 2: the flow fails once, on a
    // carried menu frame (menu_core_flow.h), which runs again as a full model frame; 3: the same, the last output is
    // shown again instead (the full frame's failure).
    int debugMenuNoFlow = 0;
    // 1: on a D3D11 bridge, the D3D11/D3D12 debug layers' messages in ReShade.log at the bridge's open, run entry, first
    // present after an exit and release; with a D3D12 info queue it also stores every message (no count limit) and
    // creates one invalid zero-width buffer as the channel's positive control (bench only; final review I1).
    int debugMenuBridgeCanary = 0;
    bool crashGuard = true;
    bool crashGuardPresent = false; // CrashGuard is set in the ini (overrides a host-specific default)
};

void LoadShellSettings();
const ShellSettings &CurrentShellSettings();

} // namespace ofps::reshade
