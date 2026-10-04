#pragma once

// D3D12 WARP fixtures of the temporal machine's grid tests (test_temporal_grid_machine.cpp,
// test_temporal_grid_switch.cpp): textures painted by clears, the temporal shader set read from a directory, and the
// RGBA16F outputs read back as floats after the recorded list ran.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "core/api/ofps_core.h"
#include "core/gpu/barriers.h"
#include "core/gpu/shaders.h"

#include <d3d12.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

namespace temporal_d3d12 {

using Microsoft::WRL::ComPtr;
using ofps::core::gpu::BarrierExternal;
inline constexpr std::uint32_t kW = 256, kH = 128;
inline constexpr DXGI_FORMAT kColor = DXGI_FORMAT_R16G16B16A16_FLOAT;
inline constexpr D3D12_RESOURCE_STATES kRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

inline ComPtr<ID3D12Resource> Texture(ID3D12Device *device, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags)
{
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = kW;
    desc.Height = kH;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Flags = flags;
    ComPtr<ID3D12Resource> texture;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                               IID_PPV_ARGS(&texture))))
        texture.Reset();
    return texture;
}

// The list, its allocator, an RTV heap for painting and the fence the machine's use point names.
struct Recording {
    ComPtr<ID3D12DescriptorHeap> heap;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    UINT painted = 0;

    bool Create(ID3D12Device *device)
    {
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heapDesc.NumDescriptors = 16;
        return SUCCEEDED(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&heap))) &&
               SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) &&
               SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list))) &&
               SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
    }

    // `base` everywhere, `patch` in a block (when given); the texture then rests readable.
    ComPtr<ID3D12Resource> Paint(ID3D12Device *device, DXGI_FORMAT format, const float (&base)[4], const float (&patch)[4], bool withPatch)
    {
        ComPtr<ID3D12Resource> texture = Texture(device, format, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        if (!texture || painted >= 16) return nullptr;
        auto rtv = heap->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += static_cast<SIZE_T>(painted++) * device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        device->CreateRenderTargetView(texture.Get(), nullptr, rtv);
        BarrierExternal(list.Get(), texture.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
        list->ClearRenderTargetView(rtv, base, 0, nullptr);
        const D3D12_RECT block{40, 30, 140, 90};
        if (withPatch) list->ClearRenderTargetView(rtv, patch, 1, &block);
        BarrierExternal(list.Get(), texture.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, kRead);
        return texture;
    }

    OfpsFencePoint Point() const
    {
        OfpsFencePoint point{};
        point.size = sizeof(point);
        point.fence = fence.Get();
        point.value = 1;
        return point;
    }
};

inline float Half(std::uint16_t h)
{
    const std::uint32_t sign = static_cast<std::uint32_t>(h & 0x8000u) << 16, exponent = (h >> 10) & 0x1fu, mantissa = h & 0x3ffu;
    if (exponent == 0) return (sign ? -1.0f : 1.0f) * std::ldexp(static_cast<float>(mantissa), -24);
    const std::uint32_t bits = sign | (exponent == 31 ? 0x7f800000u : (exponent + 112) << 23) | (mantissa << 13);
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

using Picture = std::vector<float>; // RGBA, kW x kH; decoded from fp16 one to one, so equal bits mean equal bytes

inline bool SameBits(const Picture &a, const Picture &b) { return std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0; }

// The temporal pass set as the core loads it; `grid` adds the optional grid variants.
inline bool LoadShaders(const char *directory, ofps::core::gpu::Shaders &shaders, bool grid)
{
    auto read = [&](const char *name, std::vector<char> &code) {
        std::ifstream file(std::filesystem::path(directory) / name, std::ios::binary);
        code.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    };
    read("fullscreen_vs.dxbc", shaders.vertex);
#define PW_TEMPORAL_PASS(name, member, reads, outputs, extent) read("temporal_" #name "_cs.dxbc", shaders.temporal##name);
#include "core/shaders/temporal_passes.def"
#undef PW_TEMPORAL_PASS
    read("temporal_RefineModel_cs.dxbc", shaders.temporalRefineModel);
    read("temporal_FlowLumaModel_cs.dxbc", shaders.temporalFlowLumaModel);
    if (grid) {
        read("temporal_ReprojectGrid_cs.dxbc", shaders.temporalReprojectGrid);
        read("temporal_ComposeGrid_cs.dxbc", shaders.temporalComposeGrid);
        read("temporal_CellsGrid_cs.dxbc", shaders.temporalCellsGrid);
    }
    return shaders.TemporalLoaded() && shaders.TemporalGridLoaded() == grid;
}

// Copies `outputs` (kColor, resting in COMMON) into a readback buffer, runs the list and returns them as floats;
// empty when the GPU did not finish.
inline std::vector<Picture> Execute(ID3D12Device *device, ID3D12CommandQueue *queue, Recording &r,
                                    const std::vector<ComPtr<ID3D12Resource>> &outputs)
{
    constexpr std::uint32_t kPitch = kW * 8;
    const std::size_t image = static_cast<std::size_t>(kPitch) * kH;
    D3D12_HEAP_PROPERTIES props{};
    props.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = outputs.size() * image;
    buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> readback;
    if (FAILED(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                               IID_PPV_ARGS(&readback))))
        return {};
    for (std::size_t i = 0; i < outputs.size(); ++i) {
        BarrierExternal(r.list.Get(), outputs[i].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
        dst.pResource = readback.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Offset = i * image;
        dst.PlacedFootprint.Footprint = {kColor, kW, kH, 1, kPitch};
        src.pResource = outputs[i].Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        r.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    if (FAILED(r.list->Close())) return {};
    ID3D12CommandList *lists[] = {r.list.Get()};
    queue->ExecuteCommandLists(1, lists);
    HANDLE done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    const bool finished = done && SUCCEEDED(queue->Signal(r.fence.Get(), 1)) && SUCCEEDED(r.fence->SetEventOnCompletion(1, done)) &&
                          WaitForSingleObject(done, 60000) == WAIT_OBJECT_0;
    if (done) CloseHandle(done);
    void *data = nullptr;
    const D3D12_RANGE all{0, static_cast<SIZE_T>(buffer.Width)};
    if (!finished || device->GetDeviceRemovedReason() != S_OK || FAILED(readback->Map(0, &all, &data))) return {};
    std::vector<Picture> pictures(outputs.size(), Picture(static_cast<std::size_t>(kW) * kH * 4));
    for (std::size_t i = 0; i < outputs.size(); ++i)
        for (std::size_t t = 0; t < pictures[i].size(); ++t) {
            std::uint16_t h;
            std::memcpy(&h, static_cast<const unsigned char *>(data) + i * image + t * 2, sizeof(h));
            pictures[i][t] = Half(h);
        }
    const D3D12_RANGE none{0, 0};
    readback->Unmap(0, &none);
    return pictures;
}

} // namespace temporal_d3d12
