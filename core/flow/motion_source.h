#pragma once
#include "core/api/ofps_core.h"
#include <cstdint>

// Private export contract. ABI 1 interfaces and setting IDs stay unchanged.
namespace ofps::core::flow {
enum class MotionSource : std::uint32_t { GameVectors = 0, OpticalFlow = 1 };
using SetTemporalMotionSourceV1 = int (*)(IOfpsCore*, std::uint32_t);
}
