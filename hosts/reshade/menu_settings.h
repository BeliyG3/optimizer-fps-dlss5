#pragma once
// Menu mode's view of the add-on's settings, cached from OFPS_EVENT_SETTINGS_CHANGED (shell_host.cpp). The event
// arrives under the core's lock, so this only stores (a leaf lock for the last values, atomics for the readers).
#include "core/api/ofps_settings_schema.h"
#include <cstdint>

namespace ofps::reshade {

void MenuSettingsChanged(const OfpsSettingsValues &values);
bool MenuModeOn();        // the MenuMode checkbox
int MenuTemporalMode();   // TemporalMode: 0 every frame, 1 sync, 3 background (2 reads as 1)
// Advances whenever a setting other than MenuMode changes value. A menu frame through the core must not see a setting
// the game's last evaluate did not apply: the core would re-lay out the feature (re-create its model, retire its GPU
// objects) inside the menu call (Task 12 fix round 1).
std::uint64_t MenuSettingsEpoch();

} // namespace ofps::reshade
