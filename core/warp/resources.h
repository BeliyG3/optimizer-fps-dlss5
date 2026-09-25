#pragma once

#include "core/gpu/constant_ring.h"
#include "core/warp/path_policy.h"

#include <d3d12.h>
#include <cstdint>
#include <vector>

namespace ofps::core::warp {

struct TextureState {
    ID3D12Resource* resource = nullptr;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
    DXGI_FORMAT view = DXGI_FORMAT_UNKNOWN;
};

struct PackedSlot {
    TextureState color;
    TextureState depth;
    TextureState motion;
    TextureState intermediate;
};

struct WarpResources {
    ID3D12Device* device = nullptr;
    ID3D12DescriptorHeap* heap = nullptr;
    std::vector<PackedSlot> slots;
    gpu::SetRing descriptors;
    gpu::UploadBlocks constants;
    std::uint32_t descriptorStride = 0;
    std::uint32_t nextFrameSlot = 0;

    WarpResources() = default;
    WarpResources(const WarpResources&) = delete;
    WarpResources& operator=(const WarpResources&) = delete;
    ~WarpResources();

    bool Create(ID3D12Device* device, std::uint32_t workWidth,
                std::uint32_t workHeight, std::uint32_t nativeWidth,
                std::uint32_t nativeHeight, DXGI_FORMAT colorView,
                DXGI_FORMAT outputView, std::uint32_t frameSlots,
                std::uint32_t descriptorSets);
    std::uint32_t AcquireSet(const OfpsFencePoint& gate,
                             std::uint64_t evalNow);
};

} // namespace ofps::core::warp
