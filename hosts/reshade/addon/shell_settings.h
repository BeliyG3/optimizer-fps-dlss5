#pragma once

namespace ofps::reshade {

struct ShellSettings {
    bool passive = false;
    bool floatingWindow = false;
    bool traceExit = false;
    bool debugLayer = false;
    bool debugLayerPresent = false;
    bool crashGuard = true;
};

void LoadShellSettings();
const ShellSettings &CurrentShellSettings();

} // namespace ofps::reshade
