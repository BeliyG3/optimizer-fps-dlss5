#pragma once

#include "core/api/ofps_ui_source.h"

namespace ofps::reshade {
// showAdvanced: draw the Diagnostics group, the core's status lines and the live counters.
void DrawSchemaSettings(ofps::ui::CoreUiSource &source,
                        const ofps::ui::UiSnapshot &snapshot, bool showAdvanced);
}
