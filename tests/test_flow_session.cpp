// The optical flow session on real hardware, under the D3D12 debug layer: a host list that touches the session's
// luminance images right after Acquire() must not race the engine's own registration work, once the core ordered its
// registered queues behind it (OrderAfterRegistration, a GPU wait), and registration must not wait on fence value 0.
// Skipped (77) only for a missing capability: no debug layer (Graphics Tools), no NVIDIA GPU, no optical flow engine
// or driver interface. Any other failure (a refused registration, a removed device, no ordering) fails.
#include "core/flow/OpticalFlow.h"
#include "core/gpu/queues.h"

#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {
constexpr int kSkip = 77;

ComPtr<ID3D12Device> NvidiaDevice() {
    ComPtr<IDXGIFactory6> factory;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) return nullptr;
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        if (desc.VendorId != 0x10DE || (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) continue;
        ComPtr<ID3D12Device> device;
        if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)))) return device;
    }
    return nullptr;
}

// Session::Create's reasons for hardware or a driver without the engine; every other reason is a failure.
bool MissingCapability(const std::string& reason) {
    for (const char* missing : {"nvofapi64.dll is not present", "this driver has no D3D12 optical flow interface",
                                "the GPU has no optical flow engine"})
        if (reason.rfind(missing, 0) == 0) return true;
    return false;
}

void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* image, D3D12_RESOURCE_STATES from,
                D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = image;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = from;
    barrier.Transition.StateAfter = to;
    list->ResourceBarrier(1, &barrier);
}

// Messages of error severity (or worse) plus the value-0 Wait warnings; each printed.
int Problems(ID3D12InfoQueue* info) {
    int problems = 0;
    for (UINT64 i = 0; i < info->GetNumStoredMessages(); ++i) {
        SIZE_T bytes = 0;
        info->GetMessage(i, nullptr, &bytes);
        std::vector<BYTE> data(bytes);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(data.data());
        if (FAILED(info->GetMessage(i, message, &bytes))) continue;
        const bool zeroWait = std::strstr(message->pDescription, "fence value of zero") != nullptr;
        if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR || zeroWait) {
            std::printf("[d3d12:%d] %s\n", int(message->Severity), message->pDescription);
            ++problems;
        }
    }
    info->ClearStoredMessages();
    return problems;
}

int Fail(const char* what) {
    std::printf("FAIL: %s\n", what);
    return 1;
}
} // namespace

int main() {
    ComPtr<ID3D12Debug> debug;
    if (FAILED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
        std::puts("skip: no D3D12 debug layer");
        return kSkip;
    }
    debug->EnableDebugLayer();
    ComPtr<ID3D12Device> device = NvidiaDevice();
    ComPtr<ID3D12InfoQueue> info;
    if (!device || FAILED(device.As(&info))) {
        std::puts("skip: no NVIDIA D3D12 device with the debug layer");
        return kSkip;
    }
    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> idle;
    if (FAILED(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue))) ||
        FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                         IID_PPV_ARGS(&list))) ||
        FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&idle))))
        return Fail("D3D12 objects");
    // The host's queue, handed to the core as a host does (IOfpsCore::RegisterQueue).
    if (!ofps::core::gpu::RegisterQueue(device.Get(), queue.Get())) return Fail("register the host queue");
    info->ClearStoredMessages();

    std::string reason;
    ofps::core::flow::Session* session = ofps::core::flow::Session::Acquire(device.Get(), 960, 540, reason);
    if (!session) {
        if (MissingCapability(reason)) {
            std::printf("skip: no optical flow engine (%s)\n", reason.c_str());
            return kSkip;
        }
        std::printf("FAIL: the session was refused: %s\n", reason.c_str());
        return 1;
    }
    // What the temporal machine does before its first capture (FrameState::Acquire).
    if (!session->OrderAfterRegistration(device.Get(), nullptr)) return Fail("the host queue cannot wait for the registration");
    // The first capture's barriers on both images, submitted at once (no delay to hide a race).
    for (ID3D12Resource* image : {session->Now(), session->Then()}) {
        Transition(list.Get(), image, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Transition(list.Get(), image, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    }
    if (FAILED(list->Close())) return Fail("close the list");
    ID3D12CommandList* lists[] = {list.Get()};
    queue->ExecuteCommandLists(1, lists);
    if (FAILED(queue->Signal(idle.Get(), 1))) return Fail("signal the idle fence");
    if (idle->GetCompletedValue() < 1) {
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (event == nullptr) return Fail("create an event");
        const bool armed = SUCCEEDED(idle->SetEventOnCompletion(1, event));
        const DWORD waited = armed ? WaitForSingleObject(event, 5000) : WAIT_FAILED;
        CloseHandle(event);
        if (waited != WAIT_OBJECT_0) return Fail("the capture list did not complete (timeout or wait error)");
    }
    const UINT64 reached = idle->GetCompletedValue();
    if (reached == UINT64_MAX || reached < 1 || FAILED(device->GetDeviceRemovedReason()))
        return Fail("the idle fence did not reach its value (device removed?)");
    const int problems = Problems(info.Get());
    ofps::core::gpu::UnregisterQueue(queue.Get());
    if (problems) {
        std::printf("FAIL: %d debug-layer problems after Acquire and the first capture\n", problems);
        return 1;
    }
    std::puts("ok: no race with the registration and no wait on fence value 0");
    return 0;
}
