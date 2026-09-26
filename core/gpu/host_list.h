#pragma once

#include <d3d12.h>

namespace ofps::core::gpu {

// The host lists the core records on: DIRECT (games, RenoDX, OptiScaler) and COMPUTE
// (DLSS5-Reshade-AIO evaluates Neural Rendering on async compute). On a COMPUTE list only the
// compute warp path and the compute/copy work of the temporal machine can record; the pixel warp
// path is refused by the path policy.
inline bool HostListRecordable(ID3D12GraphicsCommandList *cmd)
{
    const D3D12_COMMAND_LIST_TYPE type = cmd->GetType();
    return type == D3D12_COMMAND_LIST_TYPE_DIRECT || type == D3D12_COMMAND_LIST_TYPE_COMPUTE;
}

inline bool HostListIsCompute(ID3D12GraphicsCommandList *cmd)
{
    return cmd->GetType() == D3D12_COMMAND_LIST_TYPE_COMPUTE;
}

} // namespace ofps::core::gpu
