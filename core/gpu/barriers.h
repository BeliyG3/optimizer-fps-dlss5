#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>
#include "optimizer_fps/input_v2.h"
namespace ofps::core::gpu {
DXGI_FORMAT TypedView(DXGI_FORMAT format, bool depth);
ofps::sdk::ColorEncoding EncodingFor(DXGI_FORMAT format);
void Barrier(ID3D12GraphicsCommandList *cmd, ID3D12Resource *res, D3D12_RESOURCE_STATES &tracked,
             D3D12_RESOURCE_STATES to);
bool PlanarDepthFormat(DXGI_FORMAT format);
UINT DepthBarrierSubresource(ID3D12Resource *depth);
void BarrierExternal(ID3D12GraphicsCommandList *cmd, ID3D12Resource *res, D3D12_RESOURCE_STATES from,
                     D3D12_RESOURCE_STATES to, UINT subresource);
void BarrierExternal(ID3D12GraphicsCommandList *cmd, ID3D12Resource *res, D3D12_RESOURCE_STATES from,
                     D3D12_RESOURCE_STATES to);
bool CreateTexture(ID3D12Device *device, UINT w, UINT h, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags,
                   ID3D12Resource **out);
} // namespace ofps::core::gpu
