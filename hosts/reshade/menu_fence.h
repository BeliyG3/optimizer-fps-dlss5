#pragma once
// Fence completion as menu mode relies on it (final review, Codex C1). After a device removal a fence answers
// UINT64_MAX and the device's GetDeviceRemovedReason fails. Neither proves that GPU work finished, so nothing counts as
// passed: objects the GPU may still use are kept (quarantined), as the D3D11 bridge keeps its own.
#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>

namespace ofps::reshade {

// The fence answers UINT64_MAX or its device reports a removal. A fence whose device cannot be asked counts as live.
inline bool MenuFenceRemoved(ID3D12Fence *fence) {
    if (!fence) return false;
    if (fence->GetCompletedValue() == UINT64_MAX) return true;
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    return SUCCEEDED(fence->GetDevice(IID_PPV_ARGS(&device))) && device && device->GetDeviceRemovedReason() != S_OK;
}

// The fence reached `value` on a live device: the only completion that releases, reuses or proves anything.
inline bool MenuFencePassed(ID3D12Fence *fence, UINT64 value) {
    const UINT64 done = fence->GetCompletedValue();
    return done != UINT64_MAX && done >= value && !MenuFenceRemoved(fence);
}

// The completed value for progress reads (dump read-backs, GPU timestamps); 0 on a removed device.
inline UINT64 MenuFenceProgress(ID3D12Fence *fence) {
    if (!fence || MenuFenceRemoved(fence)) return 0;
    return fence->GetCompletedValue();
}

} // namespace ofps::reshade
