#pragma once

// The diagnostic rows and the 64x36 motion texture of the chain's WARP tests (test_temporal_carry_probe.cpp,
// test_temporal_chain_rule.cpp, test_temporal_carry_rules.cpp): a Diagnostics int row as the schema states it and as the
// core reads it from the ini, a uniform motion texture, a picture uploaded from a function, the chain's optional passes,
// and a chain copied into a readback buffer.
#include "temporal_d3d12_fixture.h"
#include "core/settings/values.h"
#include "hosts/reshade/addon/ini_schema.h"

#include <DirectXPackedVector.h>

#include <cstring>
#include <functional>
#include <vector>

namespace temporal_d3d12 {

struct OneKey {
    const char *key;
    int value;
    bool GetInt(const char *name, int &out) const
    {
        if (std::strcmp(name, key) != 0) return false;
        out = value;
        return true;
    }
    bool GetFloat(const char *, float &) const { return false; }
};

// A read-only diagnostic int of the Diagnostics group, 0..hi, 0 by default.
inline bool DiagnosticIntRow(OfpsSettingId id, const char *key, float hi)
{
    const OfpsSettingDesc &d = kOfpsSettings[id];
    return std::strcmp(d.iniKey, key) == 0 && d.type == OFPS_TYPE_INT && d.group == OFPS_GROUP_DIAGNOSTICS &&
           d.minValue == 0.0f && d.maxValue == hi && d.defaultValue.i == 0 && (d.flags & OFPS_FLAG_DIAGNOSTIC) != 0 &&
           (d.flags & OFPS_FLAG_PERSISTED) == 0;
}

// The ini value as the core reads it: the add-on's loader, then the core's clamp.
inline int LoadedDiagnostic(const char *key, int value, int ofps::core::DiagnosticsConfig::*field)
{
    auto values = ofps::reshade::SchemaDefaults();
    ofps::reshade::LoadValuesFromStore(OneKey{key, value}, values);
    ofps::core::CoreSettings settings;
    if (ofps::core::ValuesToSettings(values, &settings) != OFPS_OK) return -1;
    return settings.diag.*field;
}

inline constexpr std::uint32_t kMw = 64, kMh = 36, kPitch = kMw * 4, kSlot = kPitch * kMh; // kSlot is 512-aligned

// A 64x36 RG16F motion texture filled with one vector, resting readable.
inline ComPtr<ID3D12Resource> Motion(ID3D12Device *device, Recording &r, float x, float y)
{
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = kMw; desc.Height = kMh; desc.DepthOrArraySize = 1; desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R16G16_FLOAT; desc.SampleDesc.Count = 1; desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ComPtr<ID3D12Resource> texture;
    if (r.painted >= 16 || FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON,
                                                                  nullptr, IID_PPV_ARGS(&texture))))
        return nullptr;
    auto rtv = r.heap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(r.painted++) * device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    device->CreateRenderTargetView(texture.Get(), nullptr, rtv);
    const float value[4] = {x, y, 0.0f, 0.0f};
    BarrierExternal(r.list.Get(), texture.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
    r.list->ClearRenderTargetView(rtv, value, 0, nullptr);
    BarrierExternal(r.list.Get(), texture.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, kRead);
    return texture;
}

// A readback buffer of `slots` chains.
inline ComPtr<ID3D12Resource> AccReadback(ID3D12Device *device, std::uint32_t slots)
{
    D3D12_HEAP_PROPERTIES props{};
    props.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = static_cast<UINT64>(slots) * kSlot;
    buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> readback;
    if (FAILED(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                               IID_PPV_ARGS(&readback))))
        readback.Reset();
    return readback;
}

// Acc() (64x36 RG16F, resting readable) into slot `slot` of the readback buffer.
inline void ReadAcc(ID3D12GraphicsCommandList *list, ID3D12Resource *acc, ID3D12Resource *readback, std::uint32_t slot)
{
    BarrierExternal(list, acc, kRead, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
    dst.pResource = readback;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Offset = static_cast<UINT64>(slot) * kSlot;
    dst.PlacedFootprint.Footprint = {DXGI_FORMAT_R16G16_FLOAT, kMw, kMh, 1, kPitch};
    src.pResource = acc;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    BarrierExternal(list, acc, D3D12_RESOURCE_STATE_COPY_SOURCE, kRead);
}

// Closes and runs the list, waits for it and maps the readback buffer (`slots` chains); null when the GPU did not finish.
inline const unsigned char *RunAndMap(ID3D12Device *device, ID3D12CommandQueue *queue, Recording &r, ID3D12Resource *readback,
                                      std::uint32_t slots)
{
    if (FAILED(r.list->Close())) return nullptr;
    ID3D12CommandList *lists[] = {r.list.Get()};
    queue->ExecuteCommandLists(1, lists);
    HANDLE done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    const bool finished = done && SUCCEEDED(queue->Signal(r.fence.Get(), 1)) && SUCCEEDED(r.fence->SetEventOnCompletion(1, done)) &&
                          WaitForSingleObject(done, 60000) == WAIT_OBJECT_0;
    if (done) CloseHandle(done);
    void *data = nullptr;
    const D3D12_RANGE all{0, static_cast<SIZE_T>(slots) * kSlot};
    if (!finished || device->GetDeviceRemovedReason() != S_OK || FAILED(readback->Map(0, &all, &data))) return nullptr;
    return static_cast<const unsigned char *>(data);
}

// The chain's optional passes (temporal_marker_passes.def) beside LoadShaders' set: LinkFail and the carry chain's pair
// passes; false unless all three load.
inline bool LoadChainPasses(const char *directory, ofps::core::gpu::Shaders &shaders)
{
    const std::filesystem::path dir(directory);
    ReadInto(dir / "temporal_LinkFail_cs.dxbc", shaders.temporalLinkFail);
    ReadInto(dir / "temporal_AccumulatePair_cs.dxbc", shaders.temporalAccumulatePair);
    ReadInto(dir / "temporal_RefinePair_cs.dxbc", shaders.temporalRefinePair);
    return !shaders.temporalLinkFail.empty() && !shaders.temporalAccumulatePair.empty() && !shaders.temporalRefinePair.empty();
}

using Field = std::function<float(int, int)>; // a native pixel's value (grey for colour)

// A kW x kH texture of `format` (kColor or R32_FLOAT) filled from `value`, resting readable; its upload buffer goes to
// `keep`, alive until the list has run.
inline ComPtr<ID3D12Resource> Uploaded(ID3D12Device *device, Recording &r, DXGI_FORMAT format, const Field &value,
                                       std::vector<ComPtr<ID3D12Resource>> &keep)
{
    ComPtr<ID3D12Resource> texture = Texture(device, format, D3D12_RESOURCE_FLAG_NONE);
    if (!texture) return nullptr;
    const D3D12_RESOURCE_DESC desc = texture->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT64 total = 0;
    device->GetCopyableFootprints(&desc, 0, 1, 0, &fp, nullptr, nullptr, &total);
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = total;
    buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> upload;
    void *mapped = nullptr;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                               IID_PPV_ARGS(&upload))) ||
        FAILED(upload->Map(0, nullptr, &mapped)))
        return nullptr;
    const std::uint16_t one = DirectX::PackedVector::XMConvertFloatToHalf(1.0f);
    for (int y = 0; y < static_cast<int>(kH); ++y) {
        auto *row = static_cast<unsigned char *>(mapped) + fp.Offset + static_cast<std::size_t>(y) * fp.Footprint.RowPitch;
        for (int x = 0; x < static_cast<int>(kW); ++x) {
            const float v = value(x, y);
            if (format == DXGI_FORMAT_R32_FLOAT) {
                std::memcpy(row + x * 4, &v, 4);
                continue;
            }
            const std::uint16_t g = DirectX::PackedVector::XMConvertFloatToHalf(v), texel[4] = {g, g, g, one};
            std::memcpy(row + x * 8, texel, 8);
        }
    }
    upload->Unmap(0, nullptr);
    D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
    dst.pResource = texture.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.pResource = upload.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint = fp;
    BarrierExternal(r.list.Get(), texture.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    r.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    BarrierExternal(r.list.Get(), texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, kRead);
    keep.push_back(upload);
    return texture;
}

// Texel (x, y) of chain `slot` in a mapped readback buffer.
inline void AccAt(const unsigned char *data, std::uint32_t slot, std::uint32_t x, std::uint32_t y, float &ax, float &ay)
{
    std::uint16_t h[2];
    std::memcpy(h, data + static_cast<std::size_t>(slot) * kSlot + (static_cast<std::size_t>(y) * kMw + x) * 4, sizeof(h));
    ax = Half(h[0]);
    ay = Half(h[1]);
}

} // namespace temporal_d3d12
