#include "core/gpu/constant_ring.h"

#include <cstring>
#include <limits>

namespace ofps::core::gpu
{
void SetRing::Reset(std::uint32_t firstSet, std::uint32_t count)
{
    first_ = firstSet;
    pool_.Reset(count);
}
std::uint32_t SetRing::Acquire(const OfpsFencePoint &use, std::uint64_t evalNow, DWORD waitMs)
{
    const std::uint32_t slot = pool_.Acquire(use, evalNow, waitMs);
    return slot == DescriptorPool::kNone ? DescriptorPool::kNone : first_ + slot;
}

UploadBlocks::~UploadBlocks()
{
    if (resource_) {
        if (mapped_) resource_->Unmap(0, nullptr);
        resource_->Release();
    }
}

bool UploadBlocks::Create(ID3D12Device* device, std::uint32_t count, std::string& reason)
{
    if (!device || !count || resource_ || count > std::numeric_limits<std::uint32_t>::max() / kStride) {
        reason = "constant upload: invalid device or block count";
        return false;
    }
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = static_cast<std::uint64_t>(count) * kStride;
    desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    const HRESULT created = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
                                                              &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                              nullptr, IID_PPV_ARGS(&resource_));
    if (FAILED(created)) {
        reason = "constant upload: buffer allocation failed";
        return false;
    }
    void* data = nullptr;
    const D3D12_RANGE noRead{0, 0};
    if (FAILED(resource_->Map(0, &noRead, &data)) || !data) {
        reason = "constant upload: mapping failed";
        resource_->Release();
        resource_ = nullptr;
        return false;
    }
    mapped_ = static_cast<std::uint8_t*>(data);
    count_ = count;
    return true;
}

bool UploadBlocks::Write(std::uint32_t index, const void* data, std::size_t bytes)
{
    if (!mapped_ || index >= count_ || !data || !bytes || bytes > kStride) return false;
    auto* destination = mapped_ + static_cast<std::size_t>(index) * kStride;
    std::memset(destination, 0, kStride);
    std::memcpy(destination, data, bytes);
    return true;
}

D3D12_GPU_VIRTUAL_ADDRESS UploadBlocks::GpuAddress(std::uint32_t index) const
{
    return resource_ && index < count_ ? resource_->GetGPUVirtualAddress() + static_cast<std::uint64_t>(index) * kStride : 0;
}

bool UploadBlocks::CreateView(ID3D12Device* device, std::uint32_t index,
                              D3D12_CPU_DESCRIPTOR_HANDLE handle) const
{
    const auto address = GpuAddress(index);
    if (!device || !address) return false;
    D3D12_CONSTANT_BUFFER_VIEW_DESC view{};
    view.BufferLocation = address;
    view.SizeInBytes = kStride;
    device->CreateConstantBufferView(&view, handle);
    return true;
}
} // namespace ofps::core::gpu
