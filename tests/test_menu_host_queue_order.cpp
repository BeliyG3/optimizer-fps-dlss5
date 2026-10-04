// Menu mode's host-queue fences proven by GPU data order (fix rounds 1-2, Codex I2 / review M4), and the device-bound
// entry fence (review I2). The host queue and the private queue copy through one shared buffer; the read-back value
// shows which queue ran first. "Still blocked" is a queue-progress probe, not a sleep: the blocked queue signals
// `progress` right before the ordering Wait; once the test saw it, the work behind the Wait must not have completed
// while the gate is unsignalled.
// HARDWARE-ONLY CHECK: TestDeviceFence's second-device part needs a hardware D3D12 adapter (two WARP devices are one
// device object). Without one (CI runners) it prints "SKIP: ..." and the rest of the test still runs.
#include "test_menu_host_queue_support.h"
#include "test_menu_host_queue_fakes.h"
#include "hosts/reshade/menu_host_queue.h"
#include <cstring>

using namespace ofps::reshade;
namespace menutest {
namespace {
constexpr auto kDirect = D3D12_COMMAND_LIST_TYPE_DIRECT;
constexpr auto kCompute = D3D12_COMMAND_LIST_TYPE_COMPUTE;
constexpr UINT64 kBytes = 256;

ComPtr<ID3D12Resource> Buffer(ID3D12Device *device, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES state) {
    D3D12_HEAP_PROPERTIES props{};
    props.Type = heap;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = kBytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> buffer;
    return SUCCEEDED(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&buffer)))
               ? buffer : nullptr;
}
ComPtr<ID3D12Resource> Upload(ID3D12Device *device, std::uint32_t value) {
    auto buffer = Buffer(device, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    void *data = nullptr;
    if (!buffer || FAILED(buffer->Map(0, nullptr, &data))) return nullptr;
    std::memcpy(data, &value, sizeof(value));
    buffer->Unmap(0, nullptr);
    return buffer;
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
// A closed list that copies 4 bytes from `src` to `dst`.
ComPtr<ID3D12GraphicsCommandList> CopyList(ID3D12Device *device, D3D12_COMMAND_LIST_TYPE type, ComPtr<ID3D12CommandAllocator> &allocator,
                                           ID3D12Resource *dst, ID3D12Resource *src) {
    auto list = List(device, type, allocator, false);
    if (!list) return nullptr;
    list->CopyBufferRegion(dst, 0, src, 0, sizeof(std::uint32_t));
    return SUCCEEDED(list->Close()) ? list : nullptr;
}
// True when `progress` (signalled right before the ordering Wait) was reached and `check` (after the gated work) not.
bool HeldAfterProgress(ID3D12Fence *progress, ID3D12Fence *check) {
    return Reaches(progress, 1, 5000) && check->GetCompletedValue() == 0;
}

// Entry: the host's NR work writes the shared buffer; the menu pass (private queue) reads it only after that.
void TestEntrySignal(ID3D12Device *device) {
    auto present = Queue(device, kDirect), host = Queue(device, kCompute), privateQueue = Queue(device, kDirect);
    auto gate = Fence(device), check = Fence(device), progress = Fence(device);
    auto shared = Buffer(device, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON);
    auto result = Buffer(device, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    auto zero = Upload(device, 0), hostValue = Upload(device, 0xA1A1A1A1u);
    ComPtr<ID3D12CommandAllocator> a0, a1, a2;
    auto init = CopyList(device, kDirect, a0, shared.Get(), zero.Get());
    Execute(privateQueue.Get(), init.Get());
    Check(Free(device, privateQueue.Get(), 5000), "the shared buffer starts at zero");
    auto hostList = CopyList(device, kCompute, a1, shared.Get(), hostValue.Get());
    MenuHostQueue q;
    q.ListRecorded(hostList.Get());
    host->Wait(gate.Get(), 1); // the host's NR work is still running
    q.ListExecuting(host.Get(), hostList.Get()); // ReShade's event, then the native call
    Execute(host.Get(), hostList.Get());
    q.NotePresent();
    privateQueue->Signal(progress.Get(), 1);
    Check(q.OrderEntry(device, present.Get(), privateQueue.Get()) == MenuEntryOrder::Ordered, "the entry fence is set up");
    auto menuList = CopyList(device, kDirect, a2, result.Get(), shared.Get());
    Execute(privateQueue.Get(), menuList.Get());
    privateQueue->Signal(check.Get(), 1);
    Check(HeldAfterProgress(progress.Get(), check.Get()), "the menu pass waits for the host queue");
    gate->Signal(1);
    Check(Reaches(check.Get(), 1, 5000), "and runs once the host queue passed");
    Check(ReadBack(result.Get()) == 0xA1A1A1A1u, "the menu pass read what the host's NR work wrote");
}

// Exit: the menu pass (private queue) writes the shared buffer; the host's exit list reads it only after that. With
// `otherQueue` the exit list runs on queue B although OrderExitNow ordered queue A: ListExecuting's safety net gives B
// its own GPU Wait before the native submit (fix round 2).
void TestExitWait(ID3D12Device *device, bool otherQueue) {
    auto present = Queue(device, kDirect), host = Queue(device, kCompute), privateQueue = Queue(device, kDirect);
    auto other = Queue(device, kCompute);
    ID3D12CommandQueue *runs = otherQueue ? other.Get() : host.Get();
    auto gate = Fence(device), f2 = Fence(device), check = Fence(device), progress = Fence(device);
    auto shared = Buffer(device, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON);
    auto result = Buffer(device, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    auto before = Upload(device, 0x11111111u), menuValue = Upload(device, 0xB2B2B2B2u);
    ComPtr<ID3D12CommandAllocator> a0, a1, a2, a3;
    auto init = CopyList(device, kCompute, a0, shared.Get(), before.Get());
    auto earlier = List(device, kCompute, a1);
    MenuHostQueue q;
    q.ListRecorded(earlier.Get()); // an earlier host evaluate teaches the host queue
    q.ListExecuting(host.Get(), earlier.Get());
    Execute(host.Get(), earlier.Get());
    Execute(host.Get(), init.Get());
    Check(Free(device, host.Get(), 5000), "the shared buffer starts at the old value");
    privateQueue->Wait(gate.Get(), 1); // the run's last pass is still queued
    auto menuList = CopyList(device, kDirect, a2, shared.Get(), menuValue.Get());
    Execute(privateQueue.Get(), menuList.Get());
    privateQueue->Signal(f2.Get(), 1);
    auto exitList = CopyList(device, kCompute, a3, result.Get(), shared.Get());
    runs->Signal(progress.Get(), 1); // the progress of the queue that will run the exit list, right before its Wait
    Check(q.OrderExitNow(exitList.Get(), f2.Get(), 1, present.Get()) == MenuExitOrder::GpuWaitQueued, "the exit Wait is queued");
    q.ListRecorded(exitList.Get());
    const MenuExitOrder net = q.ListExecuting(runs, exitList.Get()); // ReShade's event, then the native call
    Check(net == (otherQueue ? MenuExitOrder::GpuWaitQueued : MenuExitOrder::NotNeeded),
          otherQueue ? "the exit list on another queue gets its own GPU Wait" : "the exit list runs on the waiting queue");
    Execute(runs, exitList.Get());
    runs->Signal(check.Get(), 1);
    Check(HeldAfterProgress(progress.Get(), check.Get()), "the host's exit list waits for the last menu pass");
    gate->Signal(1);
    Check(Reaches(check.Get(), 1, 5000), "and runs once it passed");
    Check(ReadBack(result.Get()) == 0xB2B2B2B2u, "the host's exit list read what the last menu pass wrote");
    Check(Free(device, host.Get(), 5000), "queue A's own Wait passed as well");
}

// One entry on `device`: true when it was ordered and the private queue really waited for the host queue.
bool OrderedOn(MenuHostQueue &q, ID3D12Device *device) {
    auto present = Queue(device, kDirect), host = Queue(device, kCompute), privateQueue = Queue(device, kDirect);
    auto gate = Fence(device), check = Fence(device);
    ComPtr<ID3D12CommandAllocator> allocator;
    auto list = List(device, kCompute, allocator);
    if (!present || !host || !privateQueue || !gate || !check || !list) return false;
    q.ListRecorded(list.Get());
    q.ListExecuting(host.Get(), list.Get());
    q.NotePresent();
    host->Wait(gate.Get(), 1);
    auto progress = Fence(device);
    privateQueue->Signal(progress.Get(), 1);
    const bool ordered = q.OrderEntry(device, present.Get(), privateQueue.Get()) == MenuEntryOrder::Ordered;
    privateQueue->Signal(check.Get(), 1);
    const bool held = HeldAfterProgress(progress.Get(), check.Get());
    gate->Signal(1);
    const bool ran = Reaches(check.Get(), 1, 5000);
    q.QueueDestroyed(host.Get());
    return ordered && held && ran;
}

// True when the entry fence OrderEntry would signal on `device`'s host queue belongs to `device`. The Signal is refused
// by the fake queue, so a fence of another device never reaches the runtime (that use can hang it).
bool FenceBelongsTo(MenuHostQueue &q, ID3D12Device *device) {
    auto present = Queue(device, kDirect), real = Queue(device, kCompute), privateQueue = Queue(device, kDirect);
    ComPtr<ID3D12CommandAllocator> allocator;
    auto list = List(device, kCompute, allocator);
    menutest::FailingQueue host(real.Get(), true, false);
    q.ListRecorded(list.Get());
    q.ListExecuting(&host, list.Get());
    q.NotePresent();
    const bool failed = q.OrderEntry(device, present.Get(), privateQueue.Get()) == MenuEntryOrder::Failed;
    ComPtr<ID3D12Device> owner;
    const bool same = host.signalFence && SUCCEEDED(host.signalFence->GetDevice(IID_PPV_ARGS(&owner))) && owner.Get() == device;
    q.QueueDestroyed(&host);
    return failed && same;
}

// Review I2: the entry fence belongs to one device; Forget releases it and a device change re-creates it.
void TestDeviceFence(ID3D12Device *a) {
    MenuHostQueue q;
    Check(OrderedOn(q, a), "ordered on device A");
    q.Forget();
    Check(OrderedOn(q, a), "after Forget a fresh fence still orders");
    coretest::WarpDevice hw;
    if (!coretest::CreateWarpDevice(hw, true)) {
        std::printf("SKIP: menu host queue second-device checks need a hardware D3D12 adapter (hardware-only check)\n");
        return;
    }
    ID3D12Device *b = hw.device.Get();
    q.Forget();
    Check(OrderedOn(q, b), "after Forget, ordered on device B");
    // A device change without Forget: the fence is checked through a refusing fake queue before real queues get it
    // (another device's fence can hang the runtime).
    bool fresh = FenceBelongsTo(q, a);
    Check(fresh, "a device change without Forget re-creates the fence on the new device (B -> A)");
    if (!fresh) return;
    Check(OrderedOn(q, a), "and orders on device A");
    fresh = FenceBelongsTo(q, b);
    Check(fresh, "a device change without Forget re-creates the fence on the new device (A -> B)");
    if (!fresh) return;
    Check(OrderedOn(q, b), "and orders on device B");
    Check(!coretest::HasDebugErrors(b), "no debug-layer errors on device B (no cross-device fence use)");
}
} // namespace

void RunOrderTests(ID3D12Device *device) {
    TestEntrySignal(device);
    TestExitWait(device, false);
    TestExitWait(device, true);
    TestDeviceFence(device);
}

} // namespace menutest
