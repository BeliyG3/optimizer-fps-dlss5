#pragma once

// Validated NGX configuration and the glue that pushes its values into the core.
// ReShade.ini ownership, section selection and migration live in ini_store.h.

#include "ini_store.h"

#include "optimizer_fps/types_v2.h"

namespace ofps::reshade {
void PushSettingsToCore();
// Keep the shell's next-present input in step with edits made through the core UI.
void AdoptCoreValuesForOverlay();

// Validates a configuration and stores it as the one the NGX interposer reads (ConfigForNgxHook).
// ApplyConfig additionally persists accepted local edits.
ofps::sdk::Status StoreConfig(const ofps::sdk::ConfigV2 &config);
ofps::sdk::Status ApplyConfig(const ofps::sdk::ConfigV2 &config);

// The stored configuration. ConfigForNgxHook is handed to pw_ngx::Configure as the getter it polls
// on every create/evaluate, so it is called from the render thread as well.
ofps::sdk::ConfigV2 ConfigForNgxHook();
// The same snapshot together with the status of the last configuration that was applied.
ofps::sdk::ConfigV2 CurrentConfig(ofps::sdk::Status &statusOut);
void SetLastConfigStatus(ofps::sdk::Status status);

// Log sink handed to pw_ngx::Configure.
void LogForNgxHook(bool warning, const char *message);


} // namespace ofps::reshade
