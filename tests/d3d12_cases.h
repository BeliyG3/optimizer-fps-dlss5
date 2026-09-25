#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "d3d12_adapter.h"
#include "optimizer_fps/math.h"

#include <d3d12sdklayers.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace d3d12_cases {
extern int failures;
void Check(bool condition, const char *message);
std::vector<char> ReadBinary(const char *path);
ComPtr<ID3D12Resource> CreateTexture(ID3D12Device *device, std::uint32_t width,
                                      std::uint32_t height, DXGI_FORMAT format,
                                      D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE,
                                      D3D12_RESOURCE_STATES initialState = D3D12_RESOURCE_STATE_COMMON);
bool WaitForQueue(ID3D12Device *device, ID3D12CommandQueue *queue);
bool HasDebugErrors(ID3D12Device *device);
bool ExecuteRoundTrip(ID3D12Device *device, ID3D12CommandQueue *queue,
                      ofps::sdk::D3D12Adapter *adapter, const ofps::sdk::LayoutV1 &layout,
                      const ofps::sdk::D3D12TargetFormats &formats, float gain = 1.0f,
                      float gamma = 1.0f, DXGI_FORMAT motionFormat = DXGI_FORMAT_UNKNOWN);
bool ExecuteRoundTrip(ID3D12Device *device, ID3D12CommandQueue *queue,
                      ofps::sdk::D3D12Adapter *adapter, const ofps::sdk::LayoutV2 &layout,
                      const ofps::sdk::D3D12TargetFormats &formats, float gain = 1.0f,
                      float gamma = 1.0f, DXGI_FORMAT motionFormat = DXGI_FORMAT_UNKNOWN);
void RunAdapterCases(const std::vector<char> &vertex, const std::vector<char> &pack,
                     const std::vector<char> &unpack);
} // namespace d3d12_cases
