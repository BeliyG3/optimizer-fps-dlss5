#pragma once

#include "test_core_api_gpu.h"

namespace coretest {
bool PackedMotionWithinWorkPixelThreshold(const ReadbackCapture &pixel, const ReadbackCapture &compute);
} // namespace coretest
