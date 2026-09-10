#include "host_depth_state.h"

#include "hook_context.h"

namespace pwhook {

// 26.7.2: the host's depth guide may be a real depth-stencil resource (UE: R32G8X24_TYPELESS with
// ALLOW_DEPTH_STENCIL). Such a resource rests in DEPTH_READ | NON_PIXEL_SHADER_RESOURCE, not in NPSR; a
// barrier chain that assumed NPSR left it in NPSR, and the game's own next transition (from DEPTH_READ|NPSR)
// mismatched - Gotham Knights froze after the first warped frame. Auto rule below, overridable.
D3D12_RESOURCE_STATES HostDepthState(ID3D12Resource *depth)
{
    const int mode = Ctx().diag.depthState; // DebugDepthState; 99 = follow the automatic rule below
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
            if (!s_logged) { s_logged = true; Log(false, "Optimizer FPS NGX hook: the host depth is a depth-stencil resource (fmt %d, %s): barriers address the depth plane only; resting state NON_PIXEL_SHADER_RESOURCE (DebugDepthState overrides)", (int) d.Format, pwngx::PlanarDepthFormat(d.Format) ? "planar" : "single plane"); }
        }
    }
    return kHostInputState;
}

} // namespace pwhook
