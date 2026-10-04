#include "hosts/reshade/menu_marker.h"
#include <cstdint>
#include <cstring>

namespace ofps::reshade {
namespace {
constexpr UINT kMarkerSize = 64, kMarkerAt = 16;
ID3D12Resource *g_marker = nullptr; // owned by the pipeline's resources; bound while they live

// One opaque red texel in the back buffer's format; false for a format the marker does not know.
bool RedTexel(DXGI_FORMAT format, std::uint64_t *texel, UINT *bytes) {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        *texel = 0xFF0000FFull; *bytes = 4; return true;              // bytes FF 00 00 FF
    case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        *texel = 0xFFFF0000ull; *bytes = 4; return true;              // bytes 00 00 FF FF
    case DXGI_FORMAT_R10G10B10A2_UNORM:
        *texel = 0xC00003FFull; *bytes = 4; return true;              // R 1023, A 3
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        *texel = 0x3C00000000003C00ull; *bytes = 8; return true;      // R 1.0h, A 1.0h
    default: return false;
    }
}
} // namespace

bool MenuMarkerCreate(ID3D12Device *device, DXGI_FORMAT format, ID3D12GraphicsCommandList *uploadList,
                      ID3D12Resource **texture, ID3D12Resource **upload) {
    std::uint64_t texel = 0; UINT bytes = 0;
    if (!RedTexel(format, &texel, &bytes)) return false;
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td{}; td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; td.Width = kMarkerSize; td.Height = kMarkerSize;
    td.DepthOrArraySize = 1; td.MipLevels = 1; td.Format = format; td.SampleDesc.Count = 1;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                               IID_PPV_ARGS(texture)))) return false;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{}; UINT64 total = 0;
    device->GetCopyableFootprints(&td, 0, 1, 0, &layout, nullptr, nullptr, &total);
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC bd{}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = total; bd.Height = 1;
    bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                               IID_PPV_ARGS(upload)))) return false;
    std::uint8_t *mapped = nullptr; D3D12_RANGE none{0, 0};
    if (FAILED((*upload)->Map(0, &none, reinterpret_cast<void **>(&mapped)))) return false;
    for (UINT y = 0; y < kMarkerSize; ++y)
        for (UINT x = 0; x < kMarkerSize; ++x)
            std::memcpy(mapped + layout.Offset + std::size_t(y) * layout.Footprint.RowPitch + std::size_t(x) * bytes, &texel, bytes);
    (*upload)->Unmap(0, nullptr);
    D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
    dst.pResource = *texture; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.pResource = *upload; src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; src.PlacedFootprint = layout;
    uploadList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition.pResource = *texture;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST; b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    uploadList->ResourceBarrier(1, &b);
    return true;
}

void MenuMarkerBind(ID3D12Resource *texture) { g_marker = texture; }

bool MenuMarkerPass(MenuFrame &frame) {
    if (!g_marker || frame.width < kMarkerAt + kMarkerSize || frame.height < kMarkerAt + kMarkerSize) return false;
    frame.list->CopyResource(frame.output, frame.capture);
    D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
    dst.pResource = frame.output; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.pResource = g_marker; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    frame.list->CopyTextureRegion(&dst, kMarkerAt, kMarkerAt, 0, &src, nullptr);
    return true;
}

} // namespace ofps::reshade
