#pragma once

#include <d3d12.h>
#include <string>

namespace ofps::core::warp::detail {

std::string FormatName(DXGI_FORMAT format);
bool TypedStore(ID3D12Device* device, DXGI_FORMAT format);
bool ShaderReadable(ID3D12Device* device, DXGI_FORMAT format);
bool ViewCompatible(DXGI_FORMAT resource, DXGI_FORMAT view);

} // namespace ofps::core::warp::detail
