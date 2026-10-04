#pragma once
#include "core/api/ofps_core.h"
#include <cstdint>

// Private export contract (menu mode, final review: Codex I1). ABI 1 interfaces and setting IDs stay unchanged.
// OfpsNonBlockingDescriptorsV1(core, 1): until the matching (core, 0) call, the calling thread's descriptor-pool acquires
// never wait (no free slot answers at once, the core records its fallback); returns OFPS_OK.
// OfpsNonBlockingDescriptorsV1(core, 0): ends that scope and returns how many acquires it refused (>= 0).
namespace ofps::core::gpu {
using NonBlockingDescriptorsV1 = int (*)(IOfpsCore *, std::uint32_t on);
}
