#pragma once

namespace ofps::reshade {

struct ShellSettings {
    bool passive = false;
    bool floatingWindow = false;
    bool traceExit = false;
    bool debugLayer = false;
    bool debugLayerPresent = false;
    bool crashGuard = true;
    bool crashGuardPresent = false; // CrashGuard is set in the ini (overrides a host-specific default)
};

void LoadShellSettings();
const ShellSettings &CurrentShellSettings();

} // namespace ofps::reshade
