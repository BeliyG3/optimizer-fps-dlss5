#pragma once
// Menu mode tests (ofps_menu_colour): images in the back-buffer and host formats on the
// WARP device: CPU encode/decode of RGBA8 / BGRA8 / RGB10A2 / RGBA16F texels, upload into a texture, read back.
#include "test_core_api_gpu.h"

#include <DirectXPackedVector.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace menutest {
using coretest::ComPtr;
using Pixel = std::array<float, 4>;
using Image = std::vector<Pixel>; // row-major

inline std::uint32_t Bytes(DXGI_FORMAT f) { return f == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8u : 4u; }

inline void Encode(DXGI_FORMAT f, const Pixel &c, std::byte *out) {
    if (f == DXGI_FORMAT_R16G16B16A16_FLOAT) {
        std::uint16_t h[4];
        for (int i = 0; i < 4; ++i) h[i] = DirectX::PackedVector::XMConvertFloatToHalf(c[i]);
        std::memcpy(out, h, 8);
    } else if (f == DXGI_FORMAT_R10G10B10A2_UNORM) {
        const auto q = [&](int i, float m) { return std::uint32_t(std::lround(std::clamp(c[i], 0.0f, 1.0f) * m)); };
        const std::uint32_t v = q(0, 1023) | (q(1, 1023) << 10) | (q(2, 1023) << 20) | (q(3, 3) << 30);
        std::memcpy(out, &v, 4);
    } else {
        const bool bgra = f == DXGI_FORMAT_B8G8R8A8_UNORM;
        for (int i = 0; i < 4; ++i) out[bgra && i < 3 ? 2 - i : i] = std::byte(std::lround(std::clamp(c[i], 0.0f, 1.0f) * 255.0f));
    }
}

inline Pixel Decode(DXGI_FORMAT f, const std::byte *in) {
    Pixel c{};
    if (f == DXGI_FORMAT_R16G16B16A16_FLOAT) {
        std::uint16_t h[4]; std::memcpy(h, in, 8);
        for (int i = 0; i < 4; ++i) c[i] = coretest::HalfToFloat(h[i]);
    } else if (f == DXGI_FORMAT_R10G10B10A2_UNORM) {
        std::uint32_t v; std::memcpy(&v, in, 4);
        c = {(v & 1023u) / 1023.0f, ((v >> 10) & 1023u) / 1023.0f, ((v >> 20) & 1023u) / 1023.0f, (v >> 30) / 3.0f};
    } else {
        const bool bgra = f == DXGI_FORMAT_B8G8R8A8_UNORM;
        for (int i = 0; i < 4; ++i) c[i] = float(std::to_integer<std::uint8_t>(in[bgra && i < 3 ? 2 - i : i])) / 255.0f;
    }
    return c;
}

// The value as the format stores it.
inline Pixel Stored(DXGI_FORMAT f, const Pixel &c) { std::byte raw[8]{}; Encode(f, c, raw); return Decode(f, raw); }

// Records a copy of `image` (width x height) into `target` (COPY_DEST) on w.list; `upload` must outlive the list.
inline bool Upload(coretest::WarpDevice &w, ID3D12Resource *target, DXGI_FORMAT f, const Image &image, std::uint32_t width,
                   std::uint32_t height, ComPtr<ID3D12Resource> &upload) {
    const auto layout = coretest::CreateReadback(w.device.Get(), target);
    if (!layout.buffer) return false;
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    const auto desc = layout.buffer->GetDesc();
    if (FAILED(w.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                 IID_PPV_ARGS(&upload)))) return false;
    void *mapped = nullptr;
    if (FAILED(upload->Map(0, nullptr, &mapped))) return false;
    for (std::uint32_t y = 0; y < height; ++y)
        for (std::uint32_t x = 0; x < width; ++x)
            Encode(f, image[size_t(y) * width + x], static_cast<std::byte *>(mapped) + layout.footprint.Offset +
                                                        size_t(y) * layout.footprint.Footprint.RowPitch + size_t(x) * Bytes(f));
    upload->Unmap(0, nullptr);
    D3D12_TEXTURE_COPY_LOCATION from{}, to{};
    from.pResource = upload.Get(); from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; from.PlacedFootprint = layout.footprint;
    to.pResource = target; to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    w.list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    return true;
}

inline Image Read(const coretest::ReadbackCapture &capture, DXGI_FORMAT f, std::uint32_t width, std::uint32_t height) {
    Image image(size_t(width) * height);
    void *mapped = nullptr;
    if (FAILED(capture.buffer->Map(0, nullptr, &mapped))) return {};
    for (std::uint32_t y = 0; y < height; ++y)
        for (std::uint32_t x = 0; x < width; ++x)
            image[size_t(y) * width + x] = Decode(f, static_cast<const std::byte *>(mapped) + capture.footprint.Offset +
                                                         size_t(y) * capture.footprint.Footprint.RowPitch + size_t(x) * Bytes(f));
    const D3D12_RANGE none{0, 0};
    capture.buffer->Unmap(0, &none);
    return image;
}

// A buffer in a DEFAULT (UAV), UPLOAD or READBACK heap.
inline ComPtr<ID3D12Resource> CreateBuffer(ID3D12Device *device, UINT64 bytes, D3D12_HEAP_TYPE type) {
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = type;
    D3D12_RESOURCE_DESC d{}; d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; d.Width = bytes; d.Height = 1; d.DepthOrArraySize = 1;
    d.MipLevels = 1; d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (type == D3D12_HEAP_TYPE_DEFAULT) d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    const D3D12_RESOURCE_STATES state = type == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ
                                        : type == D3D12_HEAP_TYPE_READBACK ? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_COMMON;
    ComPtr<ID3D12Resource> buffer;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&buffer)))) buffer.Reset();
    return buffer;
}

// SRV/UAV views of a texture (the format as the spike's views use it) at slot i of a CBV/SRV/UAV heap.
inline D3D12_CPU_DESCRIPTOR_HANDLE Slot(ID3D12Device *device, ID3D12DescriptorHeap *heap, UINT i) {
    auto c = heap->GetCPUDescriptorHandleForHeapStart();
    c.ptr += SIZE_T(i) * device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    return c;
}
inline D3D12_GPU_DESCRIPTOR_HANDLE GpuSlot(ID3D12Device *device, ID3D12DescriptorHeap *heap, UINT i) {
    auto g = heap->GetGPUDescriptorHandleForHeapStart();
    g.ptr += UINT64(i) * device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    return g;
}
inline void Srv(ID3D12Device *device, ID3D12DescriptorHeap *heap, UINT i, ID3D12Resource *texture) {
    D3D12_SHADER_RESOURCE_VIEW_DESC v{}; v.Format = texture->GetDesc().Format;
    v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    v.Texture2D.MipLevels = 1; device->CreateShaderResourceView(texture, &v, Slot(device, heap, i));
}
inline void Uav(ID3D12Device *device, ID3D12DescriptorHeap *heap, UINT i, ID3D12Resource *texture) {
    D3D12_UNORDERED_ACCESS_VIEW_DESC v{}; v.Format = texture->GetDesc().Format;
    v.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D; device->CreateUnorderedAccessView(texture, nullptr, &v, Slot(device, heap, i));
}

} // namespace menutest
