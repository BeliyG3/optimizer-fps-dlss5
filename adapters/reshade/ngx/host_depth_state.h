#pragma once

// The resting state of the host's depth guide. 26.7.2: the guide may be a real depth-stencil
// resource, whose planar layout and state differ from a plain shader resource; the rule is in one
// place because every path that barriers the depth asks for it.

#include "hook_common.h"

namespace pwhook {

D3D12_RESOURCE_STATES HostDepthState(ID3D12Resource *depth);

} // namespace pwhook
