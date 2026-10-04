#pragma once
#include <windows.h>
#include "core/flow/OpticalFlow.h"
#include "third_party/nvofa/nvOpticalFlowD3D12.h"
#include <memory>
#include <vector>
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
    // The input fence is created at kInputReady, so a registration's input point is already reached (a Wait on
    // value 0 is flagged by the debug layer). Each registration and each Execute takes the next ticket as its
    // output value; an Execute also signals its ticket on the input fence (always above kInputReady).
    static constexpr std::uint64_t kInputReady = 1;
    std::uint64_t ticket = 0;
    std::uint64_t registered = 0; // output value of the last registration: all of them done on the engine's queue

    // Registration.cpp
    bool Register(ID3D12Resource* resource, NvOFGPUBufferHandle* buffer);
    // Every output value handed to the engine is reached (never after a device removal).
    bool Settled() const;
    // A session whose creation failed: destroyed now if the engine finished its work, otherwise kept (quarantined)
    // until it has; kept for the process if it never does. Sweep() destroys the quarantined ones that settled.
    static void Retire(std::unique_ptr<Impl> impl);
    static void Sweep();
    static std::vector<std::unique_ptr<Impl>>& Quarantine();
    ~Impl();
};
} // namespace ofps::core::flow
