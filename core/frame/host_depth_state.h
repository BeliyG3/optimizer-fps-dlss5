#pragma once

// The resting state of the host's depth guide. 26.7.2: the guide may be a real depth-stencil
// resource, whose planar layout and state differ from a plain shader resource; the rule is in one
// place because every path that barriers the depth asks for it.

#include "core/frame/common.h"

namespace ofps::core {

D3D12_RESOURCE_STATES HostDepthState(const OfpsResource &resource);

} // namespace ofps::core
