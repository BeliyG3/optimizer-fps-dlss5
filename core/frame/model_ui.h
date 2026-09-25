#pragma once
#include "core/api/ofps_core.h"

namespace ofps::core {
// A direct host may supply UI only when every supplied texture is already on the model grid.
bool WithholdModelUi(const OfpsFrameInputs &frame, uint32_t width, uint32_t height);
} // namespace ofps::core
