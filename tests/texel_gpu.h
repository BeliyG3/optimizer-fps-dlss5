#pragma once

// A D3D12 WARP device with one command list for byte-exact texture tests (test_temporal_raw.cpp): textures and
// buffers, uploads of texel bytes, captures read back as texel bytes (each row without its padding), and the
// debug layer's errors.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace texel_gpu {

using Microsoft::WRL::ComPtr;

inline D3D12_RESOURCE_DESC Desc(DXGI_FORMAT format, std::uint32_t w, std::uint32_t h)
{
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = w;
    d.Height = h;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = format;
    d.SampleDesc.Count = 1;
    return d;
}

inline void Transition(ID3D12GraphicsCommandList *list, ID3D12Resource *resource, D3D12_RESOURCE_STATES before,
                       D3D12_RESOURCE_STATES after)
{
    if (before == after) return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = resource;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    list->ResourceBarrier(1, &b);
}

struct Gpu {
    ComPtr<ID3D12Debug> debug;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    UINT64 value = 0;                         // the fence value Run() signalled last
    std::vector<ComPtr<ID3D12Resource>> keep; // uploads of the list being recorded

    // A WARP device under the debug layer (when installed), its direct queue, a closed list and a fence.
    bool Create()
    {
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();
        ComPtr<IDXGIFactory4> factory;
        ComPtr<IDXGIAdapter> warp;
        D3D12_COMMAND_QUEUE_DESC desc{};
        desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        return SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) && SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))) &&
               SUCCEEDED(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))) &&
               SUCCEEDED(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue))) &&
               SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) &&
               SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list))) &&
               SUCCEEDED(list->Close()) && SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
    }
    bool Begin() { return SUCCEEDED(allocator->Reset()) && SUCCEEDED(list->Reset(allocator.Get(), nullptr)); }
    // Closes, runs and waits for the list; it signals value + 1.
    bool Run()
    {
        if (FAILED(list->Close())) return false;
        ID3D12CommandList *lists[] = {list.Get()};
        queue->ExecuteCommandLists(1, lists);
        HANDLE done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        const bool ok = done && SUCCEEDED(queue->Signal(fence.Get(), ++value)) &&
                        SUCCEEDED(fence->SetEventOnCompletion(value, done)) && WaitForSingleObject(done, 60000) == WAIT_OBJECT_0;
        if (done) CloseHandle(done);
        return ok && device->GetDeviceRemovedReason() == S_OK;
    }
    ComPtr<ID3D12Resource> Texture(DXGI_FORMAT format, std::uint32_t w, std::uint32_t h)
    {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        const auto desc = Desc(format, w, h);
        ComPtr<ID3D12Resource> texture;
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                   IID_PPV_ARGS(&texture))))
            texture.Reset();
        return texture;
    }
    ComPtr<ID3D12Resource> Buffer(D3D12_HEAP_TYPE type, UINT64 bytes, D3D12_RESOURCE_STATES state)
    {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = type;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = bytes;
        desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> buffer;
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&buffer))))
            buffer.Reset();
        return buffer;
    }
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT Footprint(ID3D12Resource *texture, UINT64 *bytes) const
    {
        const auto desc = texture->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT f{};
        device->GetCopyableFootprints(&desc, 0, 1, 0, &f, nullptr, nullptr, bytes);
        return f;
    }
    // Records the upload of `texels` (`stride` bytes each, row after row) into `texture`, resting in COMMON, which
    // then rests in `rest`.
    bool Upload(ID3D12Resource *texture, const std::vector<std::uint8_t> &texels, std::uint32_t stride, D3D12_RESOURCE_STATES rest)
    {
        UINT64 bytes = 0;
        const auto f = Footprint(texture, &bytes);
        auto upload = Buffer(D3D12_HEAP_TYPE_UPLOAD, bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
        void *mapped = nullptr;
        const std::size_t row = static_cast<std::size_t>(f.Footprint.Width) * stride;
        if (!upload || texels.size() < row * f.Footprint.Height || FAILED(upload->Map(0, nullptr, &mapped))) return false;
        for (UINT y = 0; y < f.Footprint.Height; ++y)
            std::memcpy(static_cast<std::uint8_t *>(mapped) + f.Offset + y * f.Footprint.RowPitch, texels.data() + y * row, row);
        upload->Unmap(0, nullptr);
        D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
        src.pResource = upload.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint = f;
        dst.pResource = texture;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        Transition(list.Get(), texture, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        Transition(list.Get(), texture, D3D12_RESOURCE_STATE_COPY_DEST, rest);
        keep.push_back(upload);
        return true;
    }
    // Records the copy of `texture` (resting in `rest`) into a readback buffer; Bytes() reads it after Run().
    ComPtr<ID3D12Resource> Capture(ID3D12Resource *texture, D3D12_RESOURCE_STATES rest)
    {
        UINT64 bytes = 0;
        const auto f = Footprint(texture, &bytes);
        auto readback = Buffer(D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST);
        if (!readback) return readback;
        D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
        src.pResource = texture;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.pResource = readback.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = f;
        Transition(list.Get(), texture, rest, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        Transition(list.Get(), texture, D3D12_RESOURCE_STATE_COPY_SOURCE, rest);
        return readback;
    }
    std::vector<std::uint8_t> Bytes(ID3D12Resource *readback, ID3D12Resource *texture, std::uint32_t stride) const
    {
        UINT64 bytes = 0;
        const auto f = Footprint(texture, &bytes);
        std::vector<std::uint8_t> texels;
        void *mapped = nullptr;
        if (!readback || FAILED(readback->Map(0, nullptr, &mapped))) return texels;
        const std::size_t row = static_cast<std::size_t>(f.Footprint.Width) * stride;
        for (UINT y = 0; y < f.Footprint.Height; ++y) {
            const auto *source = static_cast<const std::uint8_t *>(mapped) + f.Offset + y * f.Footprint.RowPitch;
            texels.insert(texels.end(), source, source + row);
        }
        const D3D12_RANGE none{0, 0};
        readback->Unmap(0, &none);
        return texels;
    }
    // Errors and corruption the debug layer stored (nothing without the layer).
    std::vector<std::string> DebugErrors() const
    {
        std::vector<std::string> errors;
        ComPtr<ID3D12InfoQueue> info;
        if (!debug || FAILED(device.As(&info))) return errors;
        for (UINT64 i = 0, n = info->GetNumStoredMessages(); i < n; ++i) {
            SIZE_T size = 0;
            if (FAILED(info->GetMessage(i, nullptr, &size))) continue;
            std::vector<char> storage(size);
            auto *message = reinterpret_cast<D3D12_MESSAGE *>(storage.data());
            if (SUCCEEDED(info->GetMessage(i, message, &size)) && message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR)
                errors.emplace_back(message->pDescription);
        }
        return errors;
    }
};

} // namespace texel_gpu
