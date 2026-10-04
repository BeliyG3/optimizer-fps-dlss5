#pragma once
// Shared helpers of the menu host queue tests (test_menu_host_queue.cpp, test_menu_host_queue_order.cpp).
#include "test_core_api_gpu.h"
#include <chrono>
#include <cstdio>

namespace menutest {
using coretest::ComPtr;

inline int failures = 0;
inline void Check(bool condition, const char *what) {
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", what); ++failures; }
}

inline ComPtr<ID3D12CommandQueue> Queue(ID3D12Device *device, D3D12_COMMAND_LIST_TYPE type) {
    D3D12_COMMAND_QUEUE_DESC d{};
    d.Type = type;
    ComPtr<ID3D12CommandQueue> q;
    return SUCCEEDED(device->CreateCommandQueue(&d, IID_PPV_ARGS(&q))) ? q : nullptr;
}
inline ComPtr<ID3D12Fence> Fence(ID3D12Device *device) {
    ComPtr<ID3D12Fence> f;
    return SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&f))) ? f : nullptr;
}
// An open list (Close it after recording) or, with `close`, an empty closed one.
inline ComPtr<ID3D12GraphicsCommandList> List(ID3D12Device *device, D3D12_COMMAND_LIST_TYPE type, ComPtr<ID3D12CommandAllocator> &allocator,
                                              bool close = true) {
    ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(device->CreateCommandAllocator(type, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(0, type, allocator.Get(), nullptr, IID_PPV_ARGS(&list)))) return nullptr;
    if (close && FAILED(list->Close())) return nullptr;
    return list;
}
// True when `fence` reaches `value` within `ms` (the test's own CPU wait).
inline bool Reaches(ID3D12Fence *fence, UINT64 value, DWORD ms) {
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    const bool ok = SUCCEEDED(fence->SetEventOnCompletion(value, event)) && WaitForSingleObject(event, ms) == WAIT_OBJECT_0;
    CloseHandle(event);
    return ok;
}
// True when `queue` runs a Signal enqueued now within `ms`: nothing we enqueued holds it.
inline bool Free(ID3D12Device *device, ID3D12CommandQueue *queue, DWORD ms) {
    auto probe = Fence(device);
    return probe && SUCCEEDED(queue->Signal(probe.Get(), 1)) && Reaches(probe.Get(), 1, ms);
}
inline double Ms(std::chrono::steady_clock::time_point since) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - since).count();
}
inline void Execute(ID3D12CommandQueue *queue, ID3D12CommandList *list) {
    ID3D12CommandList *lists[] = {list};
    queue->ExecuteCommandLists(1, lists);
}

void RunExitTests(ID3D12Device *device);  // test_menu_host_queue_exit.cpp
void RunOrderTests(ID3D12Device *device); // test_menu_host_queue_order.cpp (GPU data order, device-bound entry fence)

} // namespace menutest
