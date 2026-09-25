#pragma once
#include <windows.h>
#include "core/flow/OpticalFlow.h"
#include "third_party/nvofa/nvOpticalFlowD3D12.h"
#include <wrl/client.h>

namespace ofps::core::flow {
struct Session::Impl {
    HMODULE library = nullptr;
    NV_OF_D3D12_API_FUNCTION_LIST api = {};
    NvOFHandle handle = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    std::uint32_t width = 0, height = 0, grid = 4;
    Microsoft::WRL::ComPtr<ID3D12Resource> now, then, field;
    NvOFGPUBufferHandle nowBuffer = nullptr, thenBuffer = nullptr, fieldBuffer = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Fence> fenceIn, fenceOut;
    std::uint64_t ticket = 0;

    bool Register(ID3D12Resource* resource, NvOFGPUBufferHandle* buffer);
    ~Impl();
};
} // namespace ofps::core::flow
