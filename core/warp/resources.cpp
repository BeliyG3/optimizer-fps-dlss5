#include "core/warp/resources.h"

#include "core/gpu/barriers.h"

#include <string>

namespace ofps::core::warp {

namespace {
void ReleaseTexture(TextureState& texture) {
    if (texture.resource) texture.resource->Release();
    texture = {};
}

bool MakeTexture(ID3D12Device* device, std::uint32_t width,
                 std::uint32_t height, DXGI_FORMAT view,
                 TextureState* target) {
    if (!gpu::CreateTexture(device, width, height, view,
                            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                            &target->resource))
        return false;
    target->state = D3D12_RESOURCE_STATE_COMMON;
    target->view = view;
    return true;
}
} // namespace

WarpResources::~WarpResources() {
    descriptors.Reset(0, 0);
    for (auto& slot : slots) {
        ReleaseTexture(slot.color);
        ReleaseTexture(slot.depth);
        ReleaseTexture(slot.motion);
        ReleaseTexture(slot.intermediate);
    }
    if (heap) heap->Release();
    if (device) device->Release();
}

bool WarpResources::Create(ID3D12Device* sourceDevice,
                           std::uint32_t workWidth, std::uint32_t workHeight,
                           std::uint32_t nativeWidth, std::uint32_t nativeHeight,
                           DXGI_FORMAT colorView, DXGI_FORMAT outputView,
                           std::uint32_t frameSlots,
                           std::uint32_t descriptorSets) {
    if (device || !sourceDevice || !workWidth || !workHeight ||
        !nativeWidth || !nativeHeight || !frameSlots || descriptorSets < 64)
        return false;
    device = sourceDevice;
    device->AddRef();
    slots.resize(frameSlots);
    for (auto& slot : slots) {
        if (!MakeTexture(device, workWidth, workHeight, colorView,
                         &slot.color) ||
            !MakeTexture(device, workWidth, workHeight, DXGI_FORMAT_R32_FLOAT,
                         &slot.depth) ||
            !MakeTexture(device, workWidth, workHeight, DXGI_FORMAT_R16G16_FLOAT,
                         &slot.motion) ||
            !MakeTexture(device, nativeWidth, nativeHeight, outputView,
                         &slot.intermediate))
            return false;
    }
    descriptorStride = device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_DESCRIPTOR_HEAP_DESC desc{};
    desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    desc.NumDescriptors = descriptorSets * 10u;
    desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&heap))))
        return false;
    std::string reason;
    if (!constants.Create(device, descriptorSets * 3u, reason))
        return false;
    descriptors.Reset(0, descriptorSets);
    return true;
}

std::uint32_t WarpResources::AcquireSet(const OfpsFencePoint& gate,
                                        std::uint64_t evalNow) {
    return descriptors.Acquire(gate, evalNow, gpu::kPoolWaitMilliseconds);
}

} // namespace ofps::core::warp
