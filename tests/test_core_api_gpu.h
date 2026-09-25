#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_4.h>
#include <iostream>
#include <limits>
#include <vector>
#include <windows.h>
#include <wrl/client.h>
namespace coretest {
using Microsoft::WRL::ComPtr;
struct WarpDevice {
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter> warp;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
};
inline bool WaitForQueue(ID3D12Device *device, ID3D12CommandQueue *queue) {
    ComPtr<ID3D12Fence> fence;
    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
        return false;
    constexpr UINT64 value = 1;
    if (FAILED(queue->Signal(fence.Get(), value)))
        return false;
    if (fence->GetCompletedValue() >= value)
        return true;
    const HANDLE eventHandle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (eventHandle == nullptr)
        return false;
    const HRESULT setResult = fence->SetEventOnCompletion(value, eventHandle);
    const DWORD waitResult = SUCCEEDED(setResult) ? WaitForSingleObject(eventHandle, 10000) : WAIT_FAILED;
    CloseHandle(eventHandle);
    return SUCCEEDED(setResult) && waitResult == WAIT_OBJECT_0;
}
inline bool CreateWarpDevice(WarpDevice &w, bool hardware = false) {
    ComPtr<ID3D12Debug> debug;
    if (FAILED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
        std::cerr << "D3D12 debug layer is required for this integration test\n";
        return false;
    }
    debug->EnableDebugLayer();
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&w.factory))))
        return false;
    if (hardware) {
        ComPtr<IDXGIAdapter1> adapter;
        for (UINT i = 0; SUCCEEDED(w.factory->EnumAdapters1(i, &adapter)); ++i) {
            DXGI_ADAPTER_DESC1 desc{};
            if (SUCCEEDED(adapter->GetDesc1(&desc)) && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
                w.warp = adapter;
                break;
            }
            adapter.Reset();
        }
        if (!w.warp) return false;
    } else if (FAILED(w.factory->EnumWarpAdapter(IID_PPV_ARGS(&w.warp)))) return false;
    if (FAILED(D3D12CreateDevice(w.warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&w.device))))
        return false;
    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(w.device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&w.queue))))
        return false;
    if (FAILED(w.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&w.allocator))))
        return false;
    if (FAILED(w.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, w.allocator.Get(), nullptr,
                                           IID_PPV_ARGS(&w.list))))
        return false;
    return SUCCEEDED(w.list->Close());
}
inline bool BeginList(WarpDevice &w) {
    return SUCCEEDED(w.allocator->Reset()) && SUCCEEDED(w.list->Reset(w.allocator.Get(), nullptr));
}
inline bool SubmitList(WarpDevice &w) {
    if (FAILED(w.list->Close()))
        return false;
    ID3D12CommandList *lists[] = {w.list.Get()};
    w.queue->ExecuteCommandLists(1, lists);
    return true;
}
inline ComPtr<ID3D12Resource> CreateTexture(ID3D12Device *device, std::uint32_t width, std::uint32_t height,
                                            DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags,
                                            D3D12_RESOURCE_STATES initialState,
                                            const D3D12_CLEAR_VALUE *clearValue = nullptr) {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap.CreationNodeMask = 1;
    heap.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = flags;
    ComPtr<ID3D12Resource> result;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, initialState, clearValue,
                                               IID_PPV_ARGS(&result))))
        result.Reset();
    return result;
}
inline ComPtr<ID3D12DescriptorHeap> CreateRtvHeap(ID3D12Device *device, UINT count) {
    D3D12_DESCRIPTOR_HEAP_DESC desc{};
    desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    desc.NumDescriptors = count;
    ComPtr<ID3D12DescriptorHeap> heap;
    if (FAILED(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&heap))))
        heap.Reset();
    return heap;
}
inline D3D12_CPU_DESCRIPTOR_HANDLE RtvAt(ID3D12Device *device, ID3D12DescriptorHeap *heap, UINT index) {
    D3D12_CPU_DESCRIPTOR_HANDLE handle = heap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(index) * device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    return handle;
}
inline void Transition(ID3D12GraphicsCommandList *list, ID3D12Resource *resource, D3D12_RESOURCE_STATES before,
                       D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    list->ResourceBarrier(1, &barrier);
}
struct ReadbackCapture {
    ComPtr<ID3D12Resource> buffer;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 byteCount = 0;
};
inline ReadbackCapture CreateReadback(ID3D12Device *device, ID3D12Resource *source) {
    ReadbackCapture result;
    if (device == nullptr || source == nullptr)
        return result;
    const D3D12_RESOURCE_DESC sourceDesc = source->GetDesc();
    UINT rows = 0;
    UINT64 rowBytes = 0;
    device->GetCopyableFootprints(&sourceDesc, 0, 1, 0, &result.footprint, &rows, &rowBytes, &result.byteCount);
    if (result.byteCount == 0)
        return {};
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    heap.CreationNodeMask = 1;
    heap.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = result.byteCount;
    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;
    buffer.Format = DXGI_FORMAT_UNKNOWN;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST,
                                               nullptr, IID_PPV_ARGS(&result.buffer))))
        return {};
    return result;
}
inline void RecordReadback(ID3D12GraphicsCommandList *list, ID3D12Resource *source, const ReadbackCapture &readback) {
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = readback.buffer.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint = readback.footprint;
    D3D12_TEXTURE_COPY_LOCATION sourceLocation{};
    sourceLocation.pResource = source;
    sourceLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    sourceLocation.SubresourceIndex = 0;
    list->CopyTextureRegion(&destination, 0, 0, 0, &sourceLocation, nullptr);
}
inline bool ReadPixelBytes(const ReadbackCapture &capture, std::uint32_t x, std::uint32_t y, void *output,
                           std::size_t byteCount) {
    if (!capture.buffer || output == nullptr || x >= capture.footprint.Footprint.Width ||
        y >= capture.footprint.Footprint.Height)
        return false;
    const UINT64 offset = capture.footprint.Offset + static_cast<UINT64>(y) * capture.footprint.Footprint.RowPitch +
                          static_cast<UINT64>(x) * byteCount;
    if (offset + byteCount > capture.byteCount)
        return false;
    const D3D12_RANGE readRange{static_cast<SIZE_T>(offset), static_cast<SIZE_T>(offset + byteCount)};
    void *mapped = nullptr;
    if (FAILED(capture.buffer->Map(0, &readRange, &mapped)))
        return false;
    std::memcpy(output, static_cast<const std::byte *>(mapped) + offset, byteCount);
    const D3D12_RANGE noWrite{0, 0};
    capture.buffer->Unmap(0, &noWrite);
    return true;
}
inline float HalfToFloat(std::uint16_t value) {
    const float sign = (value & 0x8000u) != 0 ? -1.0f : 1.0f;
    const std::uint32_t exponent = (value >> 10) & 0x1fu;
    const std::uint32_t mantissa = value & 0x03ffu;
    if (exponent == 0)
        return sign * std::ldexp(static_cast<float>(mantissa), -24);
    if (exponent == 0x1fu)
        return mantissa == 0 ? sign * std::numeric_limits<float>::infinity() : std::numeric_limits<float>::quiet_NaN();
    return sign * std::ldexp(static_cast<float>(1024u + mantissa), static_cast<int>(exponent) - 25);
}
inline bool ReadHalf4(const ReadbackCapture &capture, std::uint32_t x, std::uint32_t y, std::array<float, 4> *result) {
    std::array<std::uint16_t, 4> raw{};
    if (result == nullptr || !ReadPixelBytes(capture, x, y, raw.data(), sizeof(raw)))
        return false;
    *result = {HalfToFloat(raw[0]), HalfToFloat(raw[1]), HalfToFloat(raw[2]), HalfToFloat(raw[3])};
    return true;
}
inline bool HasDebugErrors(ID3D12Device *device) {
    ComPtr<ID3D12InfoQueue> infoQueue;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&infoQueue)))) {
        std::cerr << "D3D12 debug info queue is unavailable\n";
        return true;
    }
    bool found = false;
    const UINT64 count = infoQueue->GetNumStoredMessagesAllowedByRetrievalFilter();
    for (UINT64 index = 0; index < count; ++index) {
        SIZE_T size = 0;
        if (FAILED(infoQueue->GetMessage(index, nullptr, &size)) || size == 0)
            continue;
        std::vector<std::byte> storage(size);
        auto *message = reinterpret_cast<D3D12_MESSAGE *>(storage.data());
        if (FAILED(infoQueue->GetMessage(index, message, &size)))
            continue;
        // Match test_d3d12: unused MRT outputs in the shared SDK shader produce
        // benign warnings; resource/state errors and corruption must always fail.
        if (message->Severity == D3D12_MESSAGE_SEVERITY_ERROR ||
            message->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION) {
            std::cerr << "D3D12 validation: " << message->pDescription << '\n';
            found = true;
        }
    }
    return found;
}
} // namespace coretest
