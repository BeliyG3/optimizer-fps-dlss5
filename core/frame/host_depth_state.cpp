#include "core/frame/host_depth_state.h"

#include "core/context.h"

namespace ofps::core {

// The host supplies the resting state; explicit debug overrides remain available.
D3D12_RESOURCE_STATES HostDepthState(const OfpsResource &resource)
{
    ID3D12Resource *depth = resource.res;
    int mode = Ctx().diag.depthState; // DebugDepthState; 0/99 follow the supplied state
    if ((mode == 2 || mode == 3) && Ctx().evalOnCompute) {
        static bool s_ignored = false;
        if (!s_ignored) { s_ignored = true; Log(true, "Optimizer FPS NGX hook: DebugDepthState=%d names a state a compute list cannot use; ignored on the host's compute list", mode); }
        mode = 0;
    }
    switch (mode) {
    case 1: return D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    case 2: return D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    case 3: return D3D12_RESOURCE_STATE_GENERIC_READ;
    case 4: return D3D12_RESOURCE_STATE_COMMON;
    default: break;
    }
    // 26.7.3: the NGX programming guide requires every input, depth included, in NON_PIXEL_SHADER_RESOURCE at
    // evaluate and restores it afterwards, so NPSR is the resting state for depth-stencil resources as well. What
    // differs for them is planarity (see DepthBarrierSubresource): the depth plane alone is barriered and copied.
    if (depth != nullptr) {
        const D3D12_RESOURCE_DESC d = depth->GetDesc();
        if (d.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) {
            static bool s_logged = false;
            if (!s_logged) {
                s_logged = true;
                Log(false,
                    "Optimizer FPS NGX hook: the host depth is a depth-stencil resource (fmt %d, %s): "
                    "barriers address the depth plane only; resting state NON_PIXEL_SHADER_RESOURCE "
                    "(DebugDepthState overrides)",
                    (int)d.Format, ofps::core::gpu::PlanarDepthFormat(d.Format) ? "planar" : "single plane");
            }
        }
    }
    return resource.restState;
}

} // namespace ofps::core
