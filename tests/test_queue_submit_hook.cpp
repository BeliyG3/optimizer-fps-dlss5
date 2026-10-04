// The ExecuteCommandLists hook (hosts/reshade/queue_submit_hook.h) on WARP, with menu mode's host queue as its consumer:
// the Fallen Order case (OptiScaler runs NR on its own D3D12 device, which ReShade never wraps, so no queue event ever
// reports the host's NR list). Here nothing reports it either: the entry waits for the host's NR queue until the hook is
// in, then the same recording pattern is ordered, and the private queue really waits for the host queue on the GPU.
// Fix round 1: the `before` sink's GPU work lands ahead of the list, the `after` sink's behind it (data order).
// The hook swaps the runtime's queue vtable entry for the whole process: this executable holds no other test.
#include "test_menu_host_queue_support.h"
#include "hosts/reshade/menu_host_queue.h"
#include "hosts/reshade/queue_submit_hook.h"
#include <cstring>
#include <string>

using namespace ofps::reshade;
using namespace menutest;
namespace {
constexpr auto kDirect = D3D12_COMMAND_LIST_TYPE_DIRECT;
constexpr auto kCompute = D3D12_COMMAND_LIST_TYPE_COMPUTE;

// What the add-on's sinks do for menu mode (queue_events.cpp: MenuListExecuting -> MenuHostQueue::ListExecuting before
// the native call); `after` counts.
MenuHostQueue *g_queue = nullptr;
ID3D12CommandQueue *g_lastQueue = nullptr;
ID3D12CommandList *g_lastList = nullptr;
unsigned g_before = 0, g_after = 0, g_lists = 0;
void Before(ID3D12CommandQueue *queue, UINT count, ID3D12CommandList *const *lists) {
    ++g_before;
    for (UINT i = 0; i < count; ++i) {
        ++g_lists;
        g_lastQueue = queue;
        g_lastList = lists[i];
        if (g_queue) g_queue->ListExecuting(queue, lists[i]);
    }
}
void After(ID3D12CommandQueue *, UINT, ID3D12CommandList *const *) { ++g_after; }
SubmitSinks Sinks() {
    SubmitSinks sinks;
    sinks.before = Before;
    sinks.after = After;
    return sinks;
}

void TestEntryThroughHook(ID3D12Device *device) {
    auto present = Queue(device, kDirect), host = Queue(device, kCompute), privateQueue = Queue(device, kDirect);
    auto gate = Fence(device);
    ComPtr<ID3D12CommandAllocator> a0, a1, a2, a3;
    auto list = List(device, kCompute, a0), second = List(device, kCompute, a1), third = List(device, kCompute, a2),
         skipped = List(device, kCompute, a3);
    MenuHostQueue q;
    g_queue = &q;

    // Before the hook: the host's NR list executes, nobody reports it, entry waits (the symptom in the game's tab).
    Check(!SubmitHookInstalled(), "no hook before the install");
    q.ListRecorded(list.Get());
    Execute(host.Get(), list.Get());
    for (int i = 0; i < 3; ++i) q.NotePresent();
    Check(q.OrderEntry(device, present.Get(), privateQueue.Get()) == MenuEntryOrder::WaitingForHostQueue,
          "without the hook: waiting for the host's NR queue");
    Check(q.UnobservedFor(3), "the recording stayed unobserved for 3 presents: the trigger");

    // The install: once, on the host list's device; a second call is a no-op. Every queue of that class is covered.
    const char *why = nullptr;
    Check(SubmitHookInstall(device, Sinks(), nullptr, 0, &why), why ? why : "the hook installs");
    Check(SubmitHookInstalled(), "the hook is reported installed");
    Check(SubmitHookInstall(device, Sinks(), nullptr, 0, &why), "a second install is a no-op");
    Check(SubmitHookCovers(host.Get()) && SubmitHookCovers(present.Get()), "the device's queues share the swapped vtable");

    // A call from a skipped module (the add-on's, the core's, ReShade's in the product: here this executable) is not fed.
    const HMODULE self = GetModuleHandleW(nullptr);
    SubmitHookSkip(&self, 1);
    q.ListRecorded(skipped.Get());
    Execute(host.Get(), skipped.Get());
    q.NotePresent();
    Check(g_before == 0 && g_after == 0, "a call made from a skipped module is not fed");
    Check(q.OrderEntry(device, present.Get(), privateQueue.Get()) == MenuEntryOrder::WaitingForHostQueue, "still waiting");
    SubmitHookSkip(nullptr, 0);

    // The host's next recording runs on its queue: the hook reports it, entry follows a present later, and the private
    // queue waits on the GPU for the host queue's Signal.
    q.ListRecorded(second.Get());
    host->Wait(gate.Get(), 1); // the host's NR work is still running on the GPU
    Execute(host.Get(), second.Get());
    Check(g_before == 1 && g_after == 1 && g_lists == 1 && g_lastQueue == host.Get() && g_lastList == second.Get(),
          "both sinks ran once, with the native queue and the list");
    Check(!q.UnobservedFor(0), "observed: the trigger is off");
    Check(q.OrderEntry(device, present.Get(), privateQueue.Get()) == MenuEntryOrder::WaitingForHostQueue,
          "observed in this very present: still waiting");
    q.NotePresent();
    Check(q.OrderEntry(device, present.Get(), privateQueue.Get()) == MenuEntryOrder::Ordered && q.Queue() == host.Get(),
          "one present later: ordered against the host's queue");
    Check(!Free(device, privateQueue.Get(), 300), "the private queue waits for the host's NR work");
    gate->Signal(1);
    Check(Free(device, privateQueue.Get(), 5000), "and runs once the host queue passed it");

    // Several lists in one call: each is fed (lists re-executed only once the queue drained them).
    Check(Free(device, host.Get(), 5000), "the host queue drained");
    q.ListRecorded(third.Get());
    ID3D12CommandList *both[] = {second.Get(), third.Get()};
    host->ExecuteCommandLists(2, both);
    Check(g_before == 2 && g_after == 2 && g_lists == 3 && g_lastList == third.Get(), "every list of one call is fed");

    // Restore: the original entry goes back, nothing is fed any more.
    Check(SubmitHookRestore(), "the vtable entry is restored");
    Check(!SubmitHookInstalled() && !SubmitHookCovers(host.Get()), "and reported removed");
    Check(Free(device, host.Get(), 5000), "the host queue drained");
    Execute(host.Get(), third.Get());
    Check(g_before == 2 && g_after == 2, "a call after the restore is not fed");
    Check(Free(device, host.Get(), 5000), "the host queue drains");
    g_queue = nullptr;
}

// ---- C1: the `before` sink's GPU work precedes the list on its queue, the `after` sink's follows it.
ComPtr<ID3D12Resource> Buffer(ID3D12Device *device, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES state) {
    D3D12_HEAP_PROPERTIES props{};
    props.Type = heap;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = 256;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> buffer;
    return SUCCEEDED(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&buffer)))
               ? buffer : nullptr;
}
std::uint32_t ReadBack(ID3D12Resource *buffer) {
    std::uint32_t value = 0;
    void *data = nullptr;
    const D3D12_RANGE range{0, sizeof(value)};
    if (FAILED(buffer->Map(0, &range, &data))) return 0;
    std::memcpy(&value, data, sizeof(value));
    const D3D12_RANGE none{0, 0};
    buffer->Unmap(0, &none);
    return value;
}
struct OrderProbe {
    ComPtr<ID3D12Fence> before, after, hold;
} g_order;
void OrderBefore(ID3D12CommandQueue *queue, UINT, ID3D12CommandList *const *) {
    queue->Signal(g_order.before.Get(), 1); // what precedes the list (menu mode's exit Wait in the product)
    queue->Wait(g_order.hold.Get(), 1);     // then the list is held, so the test can look between the two
}
void OrderAfter(ID3D12CommandQueue *queue, UINT, ID3D12CommandList *const *) {
    queue->Signal(g_order.after.Get(), 1); // a completion fence (the core's serial, a guide fence in the product)
}

void TestOrderAroundNativeCall(ID3D12Device *device) {
    auto host = Queue(device, kDirect);
    g_order.before = Fence(device);
    g_order.after = Fence(device);
    g_order.hold = Fence(device);
    auto upload = Buffer(device, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    auto result = Buffer(device, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    void *data = nullptr;
    const std::uint32_t value = 0xC1C1C1C1u;
    Check(upload && result && SUCCEEDED(upload->Map(0, nullptr, &data)), "order: buffers");
    if (!data) return;
    std::memcpy(data, &value, sizeof(value));
    upload->Unmap(0, nullptr);
    ComPtr<ID3D12CommandAllocator> allocator;
    auto list = List(device, kDirect, allocator, false);
    list->CopyBufferRegion(result.Get(), 0, upload.Get(), 0, sizeof(value));
    Check(SUCCEEDED(list->Close()), "order: list");
    SubmitSinks sinks;
    sinks.before = OrderBefore;
    sinks.after = OrderAfter;
    const char *why = nullptr;
    Check(SubmitHookInstall(device, sinks, nullptr, 0, &why), why ? why : "order: the hook installs");
    Execute(host.Get(), list.Get());
    Check(Reaches(g_order.before.Get(), 1, 5000), "order: the before sink's Signal ran");
    Check(ReadBack(result.Get()) == 0 && g_order.after->GetCompletedValue() == 0,
          "order: the list (held behind the before sink's work) has not run, and the after sink's fence has not passed");
    g_order.hold->Signal(1);
    Check(Reaches(g_order.after.Get(), 1, 5000) && ReadBack(result.Get()) == value,
          "order: the after sink's fence passes only behind the list");
    Check(SubmitHookRestore(), "order: restored");
    g_order = {};
}

// Silence: the hook only forwards.
void TestSilence(ID3D12Device *device) {
    auto host = Queue(device, kDirect);
    ComPtr<ID3D12CommandAllocator> allocator;
    auto list = List(device, kDirect, allocator);
    const char *why = nullptr;
    Check(SubmitHookInstall(device, Sinks(), nullptr, 0, &why), why ? why : "the hook installs again after a restore");
    const unsigned before = g_before;
    Execute(host.Get(), list.Get());
    Check(g_before == before + 1, "fed while installed");
    Check(Free(device, host.Get(), 5000), "the host queue drained");
    SubmitHookSilence();
    Execute(host.Get(), list.Get());
    Check(g_before == before + 1, "silenced: the call is only forwarded");
    Check(Free(device, host.Get(), 5000), "and the forwarded calls ran");
    Check(SubmitHookRestore(), "restored");
}

void RunAll(ID3D12Device *device) {
    TestEntryThroughHook(device);
    TestOrderAroundNativeCall(device);
    TestSilence(device);
}

// --no-debug-layer: a WARP device without the debug layer, so the swapped vtable is the runtime's own (D3D12Core.dll,
// as in a game) instead of D3D12SDKLayers.dll's wrapper.
ComPtr<ID3D12Device> PlainWarpDevice() {
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter> warp;
    ComPtr<ID3D12Device> device;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || FAILED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))) ||
        FAILED(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) return nullptr;
    return device;
}
} // namespace

int main(int argc, char **argv) {
    if (argc > 1 && std::string(argv[1]) == "--no-debug-layer") {
        auto device = PlainWarpDevice();
        if (!device) { std::fprintf(stderr, "FAIL: WARP device\n"); return 1; }
        RunAll(device.Get());
        if (failures == 0) std::printf("queue submit hook (no debug layer): all checks passed\n");
        return failures == 0 ? 0 : 1;
    }
    coretest::WarpDevice w;
    if (!coretest::CreateWarpDevice(w)) { std::fprintf(stderr, "FAIL: WARP device\n"); return 1; }
    RunAll(w.device.Get());
    Check(!coretest::HasDebugErrors(w.device.Get()), "no D3D12 debug-layer errors");
    if (failures == 0) std::printf("queue submit hook: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
