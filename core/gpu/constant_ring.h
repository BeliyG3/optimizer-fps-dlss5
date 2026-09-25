#pragma once
// Not thread-safe: access only under Ctx().mutex.
#include "core/gpu/descriptor_pool.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace ofps::core::gpu {

class SetRing {
public:
    void Reset(std::uint32_t firstSet, std::uint32_t count);
    std::uint32_t Count() const { return pool_.Count(); }
    std::uint32_t Acquire(const OfpsFencePoint &use, std::uint64_t evalNow, DWORD waitMs);

private:
    DescriptorPool pool_;
    std::uint32_t first_ = 0;
};

// One 256-byte upload block per CBV. Its index must come from a fence-safe SetRing.
class UploadBlocks {
public:
    static constexpr std::uint32_t kStride = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;
    ~UploadBlocks();
    UploadBlocks() = default;
    UploadBlocks(const UploadBlocks&) = delete;
    UploadBlocks& operator=(const UploadBlocks&) = delete;

    bool Create(ID3D12Device* device, std::uint32_t count, std::string& reason);
    bool Write(std::uint32_t index, const void* data, std::size_t bytes);
    D3D12_GPU_VIRTUAL_ADDRESS GpuAddress(std::uint32_t index) const;
    bool CreateView(ID3D12Device* device, std::uint32_t index,
                    D3D12_CPU_DESCRIPTOR_HANDLE handle) const;

private:
    ID3D12Resource* resource_ = nullptr;
    std::uint8_t* mapped_ = nullptr;
    std::uint32_t count_ = 0;
};

} // namespace ofps::core::gpu
