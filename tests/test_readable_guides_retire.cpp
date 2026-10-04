// Queue-hook fix round 1 (Codex I): the readable guides hand a retired twin or hold to the core's RetireResource
// WITHOUT holding their own mutex. RetireResource takes the core's mutex; a submit thread (the hook's sink, or ReShade's
// execute event) holding the guides' mutex while it waits for that would hold up ReadableGuidesPresented on the present
// path. The fake core below checks, from inside RetireResource, that another thread can take the guides' mutex.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "hosts/reshade/readable_guides.h"
#include "hosts/reshade/shell_host.h"
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>

using Microsoft::WRL::ComPtr;
using namespace ofps::reshade;

namespace {
int failures = 0;
void Check(bool ok, const char *what) { if (!ok) { std::cerr << "FAIL: " << what << '\n'; ++failures; } }

struct FakeCore final : IOfpsCore {
    unsigned retired = 0;
    bool lockFree = false, fenced = false;
    ID3D12Resource *probe = nullptr; // any resource: ReadableGuidesHoldBusy only needs the guides' mutex
    std::thread checker;
    int CreateFeature(ID3D12GraphicsCommandList *, const OfpsFeatureDesc *, IOfpsModelHost *, IOfpsFeature **) override { return OFPS_E_STATE; }
    int AdoptFeature(ID3D12GraphicsCommandList *, const OfpsFeatureDesc *, void *, IOfpsModelHost *, IOfpsFeature **) override { return OFPS_E_STATE; }
    void NotifyForeignReleased(void *) override {}
    int SetSettings(const OfpsSettingsValues *) override { return OFPS_OK; }
    void GetSettings(OfpsSettingsValues *) override {}
    void GetSettingRange(uint32_t, float *, float *) override {}
    void Status(OfpsStatus *out) override { out->size = sizeof(*out); }
    uint32_t StatusLines(OfpsStatusRow *, uint32_t) override { return 0; }
    void LayoutPreview(OfpsLayoutPreview *out) override { out->size = sizeof(*out); }
    void RegisterQueue(ID3D12Device *, ID3D12CommandQueue *) override {}
    void UnregisterQueue(ID3D12CommandQueue *) override {}
    void OnCommandListExecuted(ID3D12CommandQueue *, ID3D12CommandList *) override {}
    void RetireResource(IUnknown *object, const OfpsFencePoint *extra) override {
        ++retired;
        fenced = extra && extra->fence && extra->value > 0;
        // Another thread takes the guides' mutex; under the old code it waited until this call returned (joined by main).
        done.store(false);
        checker = std::thread([this] { ReadableGuidesHoldBusy(probe); done.store(true); });
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(1000);
        while (!done.load() && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        lockFree = done.load();
        (void) object;
    }
    std::atomic<bool> done{false};
    void SetDirectHost(IOfpsHost *, uint32_t) override {}
    void SetHostCaps(IOfpsHost *, const OfpsHostCaps *) override {}
    void SetHostMotionGrid(uint32_t) override {}
    void Housekeeping() override {}
    void UnregisterHost(IOfpsHost *) override {}
    void Release() override {}
};
FakeCore g_core;

ComPtr<ID3D12Resource> Buffer(ID3D12Device *device) {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = 256;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> buffer;
    return SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                     IID_PPV_ARGS(&buffer))) ? buffer : nullptr;
}
} // namespace

namespace ofps::reshade {
IOfpsCore *Core() { return &g_core; }
bool DirectHostActive() { return false; }
ShellHost &Host() { static ShellHost host; return host; }
void ShellHost::Log(OfpsLogLevel, const char *text) { std::cout << text << '\n'; }
void ShellHost::OnEvent(OfpsEvent, const OfpsEventData *) {}
} // namespace ofps::reshade

int main() {
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter> warp;
    ComPtr<ID3D12Device> device;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || FAILED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))) ||
        FAILED(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) {
        std::cerr << "FAIL: WARP device\n";
        return 1;
    }
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    auto held = Buffer(device.Get()), probe = Buffer(device.Get());
    if (FAILED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))) ||
        FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list))) || !held || !probe) {
        std::cerr << "FAIL: D3D12 objects\n";
        return 1;
    }
    g_core.probe = probe.Get();
    // A hold written on the host list, dropped, then submitted: the present after the submission signals its fence and
    // retires it to the core.
    Check(ReadableGuidesHoldWrite(list.Get(), held.Get()), "hold recorded");
    ReadableGuidesHoldDrop(held.Get());
    Check(g_core.retired == 0, "an unsubmitted hold is not retired");
    Check(SUCCEEDED(list->Close()), "close");
    ID3D12CommandList *lists[] = {list.Get()};
    queue->ExecuteCommandLists(1, lists);
    ReadableGuidesExecuted(queue.Get(), list.Get());
    Check(g_core.retired == 0, "a submitted hold waits for its fence");
    ReadableGuidesPresented(); // the fence goes on the queue after the list, and the hold goes to the core
    if (g_core.checker.joinable()) g_core.checker.join();
    Check(g_core.retired == 1 && g_core.fenced, "the dropped hold is retired to the core, gated on its fence");
    Check(g_core.lockFree, "RetireResource runs without the guides' mutex held");
    ShutdownReadableGuides();
    if (failures == 0) std::cout << "readable guides retire: all checks passed\n";
    return failures == 0 ? 0 : 1;
}
