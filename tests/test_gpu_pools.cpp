#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "core/gpu/descriptor_pool.h"
#include "core/context.h"
#include "core/gpu/constant_ring.h"
#include "core/gpu/graveyard.h"
#include "core/gpu/queues.h"
#include "core/gpu/submission.h"
#include "core/temporal/machine.h"
#include "core/temporal/resources.h"
#include <filesystem>
#include <fstream>
#include <iterator>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <cstdint>
#include <cstdlib>
#include <iostream>
using Microsoft::WRL::ComPtr;
using namespace ofps::core::gpu;
int TestComputePipeline(ID3D12Device* device);
namespace
{
int failures = 0;
void Check(bool condition, const char *message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}
OfpsFencePoint Point(ID3D12Fence *fence, std::uint64_t value)
{
    OfpsFencePoint p{};
    p.size = sizeof(p);
    p.fence = fence;
    p.value = value;
    return p;
}
bool WaitFence(ID3D12Fence *fence, std::uint64_t value)
{
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (ev == nullptr)
        return false;
    const bool ok = WaitFenceValue(fence, value, ev, 3000);
    CloseHandle(ev);
    return ok;
}
struct Marker final : Disposable
{
    bool *released;
    explicit Marker(bool *flag) : released(flag)
    {
    }
    ~Marker() override
    {
        *released = true;
    }
};
void TestQueueLifetime(ID3D12Device *device)
{
    D3D12_COMMAND_QUEUE_DESC desc{};
    ComPtr<ID3D12CommandQueue> queue;
    Check(SUCCEEDED(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue))), "queue lifetime: create");
    Check(RegisterQueue(device, queue.Get()), "queue lifetime: register");
    auto *identity = queue.Get();
    ComPtr<ID3D12CommandQueue> retained;
    retained.Attach(RetainRegisteredQueue(identity));
    Check(retained != nullptr, "queue lifetime: retain during deferred processing");
    UnregisterQueue(identity);
    queue.Reset();
    Check(RetainRegisteredQueue(identity) == nullptr, "queue lifetime: stale ring entry is rejected");
    ComPtr<ID3D12Fence> fence;
    Check(SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))), "queue lifetime: fence");
    Check(retained && SUCCEEDED(retained->Signal(fence.Get(), 1)) && WaitFence(fence.Get(), 1),
          "queue lifetime: retained async queue survives unregistration");
}

void TestTemporalFallback(ID3D12Device *device, ID3D12CommandQueue *queue, const char *shaderDirectory)
{
    using namespace ofps::core::temporal;
    Shaders shaders;
    auto read = [&](const char *name, std::vector<char> &code) {
        std::ifstream file(std::filesystem::path(shaderDirectory) / name, std::ios::binary);
        code.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    };
    read("fullscreen_vs.dxbc", shaders.vertex);
#define PW_TEMPORAL_PASS(name, member, reads, outputs, extent) read("temporal_" #name "_cs.dxbc", shaders.temporal##name);
#include "core/shaders/temporal_passes.def"
#undef PW_TEMPORAL_PASS
    read("temporal_RefineModel_cs.dxbc", shaders.temporalRefineModel);
    read("temporal_FlowLumaModel_cs.dxbc", shaders.temporalFlowLumaModel);
    Machine machine;
    char error[256]{};
    Check(machine.Initialize(device, shaders, 16, 16, DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT,
                             16, 16, DXGI_FORMAT_R32_FLOAT, 16, 16, error, sizeof(error)), "fallback: machine initialization");
    if (!machine.Ready()) { std::cerr << error << '\n'; return; }
    ComPtr<ID3D12Resource> color, depth, motion, output;
    Check(CreateTexture(device, 16, 16, DXGI_FORMAT_R32G32B32A32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, &color) &&
          CreateTexture(device, 16, 16, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_NONE, &depth) &&
          CreateTexture(device, 16, 16, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_NONE, &motion) &&
          CreateTexture(device, 16, 16, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_NONE, &output),
          "fallback: different colour/output formats allocated");
    if (!color || !depth || !motion || !output) return;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    Check(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) &&
          SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list))),
          "fallback: command list");
    if (!list) return;
    ComPtr<ID3D12Fence> fence;
    Check(SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))), "fallback: fence");
    machine.SetUsePoint(Point(fence.Get(), 1), 0);
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; heapDesc.NumDescriptors = 1;
    ComPtr<ID3D12DescriptorHeap> heap;
    Check(SUCCEEDED(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&heap))), "fallback: RTV heap");
    if (!heap) return;
    const auto rtv = heap->GetCPUDescriptorHandleForHeapStart();
    device->CreateRenderTargetView(color.Get(), nullptr, rtv);
    BarrierExternal(list.Get(), color.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
    const float expected[4] = {1.0f, 0.5f, 0.25f, 1.0f};
    list->ClearRenderTargetView(rtv, expected, 0, nullptr);
    BarrierExternal(list.Get(), color.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    FrameInputs in{};
    in.color = color.Get(); in.depth = depth.Get(); in.motion = motion.Get();
    in.colorView = DXGI_FORMAT_R32G32B32A32_FLOAT; in.depthView = DXGI_FORMAT_R32_FLOAT; in.motionView = DXGI_FORMAT_R16G16_FLOAT;
    in.colorRect = in.depthRect = in.motionRect = {0, 0, 16, 16};
    in.hostInputState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    in.depthState = D3D12_RESOURCE_STATE_COMMON;
    in.smoothRadius = 0;
    // Exhaust normal tables behind an unsignalled fence before requesting format-converting raw output.
    for (std::uint32_t i = 0; i <= kTableSlots; ++i)
        machine.RecordReproject(list.Get(), in, nullptr, D3D12_RESOURCE_STATE_COMMON, 0, 0);
    Check(machine.TakeExhausted(), "fallback: normal descriptor capacity exhausted");
    in.debugVis = 3;
    machine.RecordReproject(list.Get(), in, output.Get(), D3D12_RESOURCE_STATE_COMMON, 0, 0);
    Check(!machine.TakeExhausted(), "fallback: independent descriptors remain available");
    D3D12_HEAP_PROPERTIES props{}; props.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC desc{}; desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; desc.Width = 256 * 16;
    desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1; desc.SampleDesc.Count = 1; desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> readback;
    Check(SUCCEEDED(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                                   nullptr, IID_PPV_ARGS(&readback))), "fallback: readback");
    if (!readback) return;
    BarrierExternal(list.Get(), output.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
    dst.pResource = readback.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint = {DXGI_FORMAT_R16G16B16A16_FLOAT, 16, 16, 1, 256};
    src.pResource = output.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    Check(SUCCEEDED(list->Close()), "fallback: close");
    ID3D12CommandList *lists[] = {list.Get()}; queue->ExecuteCommandLists(1, lists);
    Check(SUCCEEDED(queue->Signal(fence.Get(), 1)) && WaitFence(fence.Get(), 1), "fallback: GPU completion");
    void *data = nullptr;
    const D3D12_RANGE range{0, 8};
    Check(SUCCEEDED(readback->Map(0, &range, &data)), "fallback: map pixels");
    if (data) {
        const auto *pixel = static_cast<const std::uint16_t *>(data);
        Check(pixel[0] == 0x3c00 && pixel[1] == 0x3800 && pixel[2] == 0x3400 && pixel[3] == 0x3c00,
              "fallback: exhausted normal pool still returns converted host colour");
        const D3D12_RANGE none{0, 0}; readback->Unmap(0, &none);
    }
}

void TestPool(ID3D12Device *device)
{
    ComPtr<ID3D12Fence> fence;
    Check(SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))), "pool: fence is created");
    DescriptorPool pool;
    pool.Reset(4);
    Check(pool.Count() == 4, "pool: four slots");
    const OfpsFencePoint unsignalled = Point(fence.Get(), 1);
    std::uint32_t slots[4];
    for (std::uint32_t i = 0; i < 4; ++i)
        slots[i] = pool.Acquire(unsignalled, 0, kPoolWaitMilliseconds);
    Check(slots[0] == 0 && slots[1] == 1 && slots[2] == 2 && slots[3] == 3,
          "pool: the first four acquires hand out slots 0..3 in order");
    Check(pool.InFlight(0) == 4, "pool: all four slots are in flight behind the unsignalled fence");
    const ULONGLONG t0 = GetTickCount64();
    const std::uint32_t fifth = pool.Acquire(unsignalled, 0, kPoolWaitMilliseconds);
    const ULONGLONG waited = GetTickCount64() - t0;
    Check(fifth == DescriptorPool::kNone, "pool: the fifth acquire fails while the fence is unsignalled");
    Check(waited >= kPoolWaitMilliseconds - 16 && waited < 1000,
          "pool: the failed acquire waits about the bound (100 ms), not 3 s and not 0");
    Check(SUCCEEDED(fence->Signal(1)), "pool: the fence is signalled from the CPU");
    const std::uint32_t after = pool.Acquire(Point(fence.Get(), 2), 0, kPoolWaitMilliseconds);
    Check(after != DescriptorPool::kNone, "pool: an acquire succeeds once the fence passed");
    Check(pool.InFlight(0) == 1, "pool: the re-tagged slot is the only one in flight");
    DescriptorPool cpu;
    cpu.Reset(2);
    Check(cpu.Acquire(Point(nullptr, 5), 1, kPoolWaitMilliseconds) == 0 &&
              cpu.Acquire(Point(nullptr, 5), 1, kPoolWaitMilliseconds) == 1,
          "pool: evaluate-count tags fill both slots");
    const ULONGLONG c0 = GetTickCount64();
    Check(cpu.Acquire(Point(nullptr, 6), 4, kPoolWaitMilliseconds) == DescriptorPool::kNone,
          "pool: an evaluate-count slot is not free before its evaluate");
    Check(GetTickCount64() - c0 < 50, "pool: nothing to wait for with evaluate-count tags, the refusal is immediate");
    Check(cpu.Acquire(Point(nullptr, 9), 5, kPoolWaitMilliseconds) == 0,
          "pool: the slot is free once evalNow reaches its value");
    SetRing ring;
    ring.Reset(10, 4);
    Check(ring.Count() == 4 && ring.Acquire(Point(nullptr, 1), 0, kPoolWaitMilliseconds) == 10 &&
              ring.Acquire(Point(nullptr, 1), 0, kPoolWaitMilliseconds) == 11,
          "set ring: indices are offset by the first set");
}
void TestSubmission(ID3D12Device *device, ID3D12CommandQueue *queue, ID3D12CommandQueue *other)
{
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> listA, listB, listC;
    Check(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))),
          "submission: allocator");
    Check(SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                              IID_PPV_ARGS(&listA))) &&
              SUCCEEDED(listA->Close()),
          "submission: list A");
    Check(SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                              IID_PPV_ARGS(&listB))) &&
              SUCCEEDED(listB->Close()),
          "submission: list B");
    Check(SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                              IID_PPV_ARGS(&listC))) &&
              SUCCEEDED(listC->Close()),
          "submission: list C");
    Submission sub;
    Check(sub.Track(device, queue), "submission: the host queue is tracked");
    OfpsFencePoint p{};
    Check(sub.NextSubmission(queue, &p) && p.fence != nullptr && p.value == 1,
          "submission: before any push the next submission is serial 1");
    Check(!sub.NextSubmission(other, &p), "submission: an untracked queue has no serial");
    sub.Push(queue, listA.Get());
    sub.Push(queue, listB.Get());
    sub.Push(other, listC.Get());
    SubmissionEntry entries[8];
    const std::uint32_t n = sub.Drain(entries, 8, Submission::Signal::All);
    Check(n == 2, "submission: only tracked queues enter the ring");
    Check(entries[0].queue == queue && entries[0].list == listA.Get() && entries[0].serial == 1,
          "submission: entry 0 is list A, serial 1");
    Check(entries[1].queue == queue && entries[1].list == listB.Get() && entries[1].serial == 2,
          "submission: entry 1 is list B, serial 2");
    Check(sub.NextSubmission(queue, &p) && p.value == 3,
          "submission: the next submission after two pushes is serial 3");
    Check(WaitFence(p.fence, 2), "submission: the serial fence reaches 2 after the drain (empty queue)");
    Check(p.fence->GetCompletedValue() < 3, "submission: the serial fence has not reached the future serial");
    sub.Push(queue, listA.Get());
    Check(sub.Drain(entries, 8, Submission::Signal::Settled) == 1,
          "submission: the settled drain still hands out the entry");
    Check(p.fence->GetCompletedValue() < 3,
          "submission: a settled drain does not signal the serial pushed since the previous drain");
    Check(sub.Drain(entries, 8, Submission::Signal::Settled) == 0 && WaitFence(p.fence, 3),
          "submission: the next settled drain signals it");
    GateSet gate;
    sub.PendingGate(device, nullptr, &gate);
    Check(gate.Size() == 1 && gate.Completed(),
          "submission: a gate without pending recordings covers the last submitted serial");
    for (int i = 0; i < 300; ++i)
        sub.Push(queue, listB.Get());
    std::uint32_t drained = 0;
    for (;;)
    {
        const std::uint32_t k = sub.Drain(entries, 8, Submission::Signal::All);
        if (k == 0)
            break;
        drained += k;
    }
    Check(drained == kSubmissionRing && sub.Dropped() == 300 - kSubmissionRing,
          "submission: a full ring drops the oldest entries and counts them");
    Check(sub.NextSubmission(queue, &p) && p.value == 3 + 300 + 1,
          "submission: dropped entries still advanced the serial");
    Check(WaitFence(p.fence, 303), "submission: the serial fence follows the pushed count, not the ring");
    sub.Untrack(queue);
    Check(!sub.NextSubmission(queue, &p), "submission: an untracked queue has no serial any more");
}
void TestGraveyard(ID3D12Device *device, ID3D12CommandQueue *queue)
{
    Submission sub;
    Check(sub.Track(device, queue), "graveyard: the host queue is tracked");
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    Check(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) &&
              SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                                  IID_PPV_ARGS(&list))) &&
              SUCCEEDED(list->Close()),
          "graveyard: a list");
    sub.Push(queue, list.Get());
    SubmissionEntry entries[4];
    Check(sub.Drain(entries, 4, Submission::Signal::All) == 1, "graveyard: serial 1 reported");
    OfpsFencePoint next{};
    Check(sub.NextSubmission(queue, &next) && next.value == 2, "graveyard: the list being recorded is submission 2");
    bool released = false;
    Graveyard graveyard;
    {
        Grave g;
        g.disposables.push_back(std::make_unique<Marker>(&released));
        g.gate.Add(next.fence, next.value);
        graveyard.Add(std::move(g), 1);
    }
    graveyard.Drain(false, 1 + kDeferredReleaseEvaluates + 10);
    Check(!released && graveyard.Size() == 1,
          "graveyard: past dueEval the object is still held while serial 2 is not reported");
    graveyard.Drain(true, 1 + kDeferredReleaseEvaluates + 10, 50);
    Check(!released && graveyard.Size() == 1,
          "graveyard: a full drain that times out on the gate does not free the object");
    sub.Push(queue, list.Get());
    Check(sub.Drain(entries, 4, Submission::Signal::All) == 1 && WaitFence(next.fence, 2),
          "graveyard: serial 2 reported and reached");
    graveyard.Drain(false, 1 + kDeferredReleaseEvaluates + 10);
    Check(released && graveyard.Empty(), "graveyard: the object is released once its serial is reported and passed");
    sub.Untrack(queue);
}
void TestRecordingLifetime(ID3D12Device *device, ID3D12CommandQueue *queue, ID3D12CommandQueue *other)
{
    Submission sub;
    Check(sub.Track(device, queue) && sub.Track(device, other), "recording: track two queues");
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list, unrelated;
    Check(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) &&
          SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list))) &&
          SUCCEEDED(list->Close()) &&
          SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&unrelated))) &&
          SUCCEEDED(unrelated->Close()), "recording: lists");
    if (!list || !unrelated) return;
    DescriptorPool pool;
    pool.Reset(1);
    const auto use = sub.UsePoint(list.Get());
    Check(use.fence && pool.Acquire(use, 0, 0) == 0, "recording: acquire tagged slot");
    SubmissionEntry entries[kSubmissionRing];
    sub.Push(queue, unrelated.Get());
    sub.Push(other, unrelated.Get());
    sub.Drain(entries, kSubmissionRing, Submission::Signal::All);
    Check(pool.Acquire(use, 10000, 0) == DescriptorPool::kNone,
          "recording: neither unrelated submission nor frame age releases a slot");
    bool released = false;
    Graveyard graveyard;
    Grave grave;
    grave.disposables.push_back(std::make_unique<Marker>(&released));
    sub.PendingGate(device, nullptr, &grave.gate);
    graveyard.Add(std::move(grave), 0);
    const auto start = GetTickCount64();
    graveyard.Drain(false, 10000);
    Check(GetTickCount64() - start < 50 && !released, "recording: ordinary graveyard drain never waits");
    const UINT64 job = 71;
    list->SetPrivateData(kAsyncKickTag, sizeof(job), &job);
    ID3D12CommandList *lists[] = {list.Get()};
    other->ExecuteCommandLists(1, lists);
    sub.Push(other, list.Get());
    Check(pool.InFlight(10000) == 1, "recording: Push alone does not complete GPU use");
    Check(sub.Drain(entries, kSubmissionRing, Submission::Signal::All) == 1 && entries[0].asyncTag == job,
          "recording: Push snapshots the async tag before the list can be released");
    Check(WaitFence(use.fence, 1) && pool.InFlight(10000) == 0,
          "recording: only the submitted list's fence releases its slot");
    graveyard.Drain(false, 10000);
    Check(released, "recording: submission releases its grave");
    const auto submitted = sub.UsePoint(list.Get());
    Check(pool.Acquire(submitted, 10000, 0) == 0, "recording: slot for final queued use");
    queue->ExecuteCommandLists(1, lists);
    sub.Push(queue, list.Get());
    sub.Untrack(queue);
    Check(WaitFence(submitted.fence, 1) && pool.InFlight(10000) == 0,
          "recording: Untrack signals a submitted slot without a later Drain");
    Check(sub.Track(device, queue), "recording: re-track with fresh serials");
    const auto cancelled = sub.UsePoint(list.Get());
    Check(pool.Acquire(cancelled, 10000, 0) == 0, "recording: next recording has a new pending fence");
    released = false;
    Grave pending;
    pending.disposables.push_back(std::make_unique<Marker>(&released));
    sub.PendingGate(device, nullptr, &pending.gate);
    graveyard.Add(std::move(pending), 0);
    sub.Untrack(queue);
    Check(pool.InFlight(10000) == 1, "recording: another live queue can still submit the list");
    sub.Untrack(other);
    Check(WaitFence(cancelled.fence, 1) && pool.InFlight(10000) == 0,
          "recording: last Untrack cancels an unsubmitted slot");
    graveyard.Drain(false, 10000);
    Check(released, "recording: Untrack also opens retirement gates");
}
void TestRecordingExpiry(ID3D12Device *device, ID3D12CommandQueue *queue)
{
    auto &evalNow = ofps::core::Ctx().evalCounter;
    const auto savedEval = evalNow;
    evalNow = 100;
    Submission sub;
    Check(sub.Track(device, queue), "expiry: live queue tracked");
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    Check(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) &&
          SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                              IID_PPV_ARGS(&list))), "expiry: unsubmitted list");
    if (!list) { evalNow = savedEval; return; }
    const auto use = sub.UsePoint(list.Get());
    GateSet gate, all;
    sub.PendingGate(device, nullptr, &gate);
    sub.PendingGateAll(&all);
    bool released = false;
    Graveyard graveyard;
    Grave grave;
    grave.disposables.push_back(std::make_unique<Marker>(&released));
    grave.gate = gate;
    graveyard.Add(std::move(grave), evalNow);
    DescriptorPool fallback;
    fallback.Reset(1);
    const auto failed = sub.UsePoint(nullptr);
    Check(!failed.fence && failed.value == evalNow + kDeferredReleaseEvaluates &&
          fallback.Acquire(failed, evalNow, 0) == 0, "expiry: finite fallback deadline");
    list.Reset(); // Abandoned without Push, while the queue remains tracked.
    for (unsigned i = 0; i < 32; ++i) sub.Drain(nullptr, 0, Submission::Signal::All);
    Check(!gate.Completed() && !all.Completed(), "expiry: drains alone do not advance recording age");
    evalNow += kDeferredReleaseEvaluates - 1;
    sub.Drain(nullptr, 0, Submission::Signal::All);
    Check(!gate.Completed() && fallback.InFlight(evalNow) == 1, "expiry: held before deadline");
    ++evalNow;
    sub.Drain(nullptr, 0, Submission::Signal::All);
    graveyard.Drain(false, evalNow);
    Check(use.fence && gate.Completed() && all.Completed() && released && graveyard.Empty() &&
          fallback.InFlight(evalNow) == 0, "expiry: deadline opens gates and releases grave and fallback slot");
    GateSet fresh;
    sub.PendingGateAll(&fresh);
    OfpsFencePoint next{};
    Check(fresh.Empty() && sub.NextSubmission(queue, &next), "expiry: recording removed, queue still live");
    evalNow = savedEval;
}
void TestEmptyGateDrain(ID3D12Device *device, ID3D12CommandQueue *queue)
{
    ComPtr<ID3D12Fence> blocker;
    Check(SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&blocker))), "empty gate: blocker");
    Check(RegisterQueue(device, queue), "empty gate: register queue");
    queue->Wait(blocker.Get(), 1);
    bool released = false;
    Graveyard graveyard;
    Grave grave;
    grave.disposables.push_back(std::make_unique<Marker>(&released));
    graveyard.Add(std::move(grave), 0);
    const auto start = GetTickCount64();
    graveyard.Drain(false, kDeferredReleaseEvaluates);
    Check(GetTickCount64() - start < 50 && !released, "empty gate: newly captured gate does not wait");
    blocker->Signal(1);
    graveyard.Drain(true, kDeferredReleaseEvaluates);
    Check(released, "empty gate: teardown may wait for completion");
    UnregisterQueue(queue);
}
void TestInsideCore()
{
    using namespace ofps::core;
    Check(!insideCore, "housekeeping: initially outside core");
    {
        std::lock_guard<std::mutex> lock(Ctx().mutex);
        const InsideCoreScope scope;
        Check(insideCore, "housekeeping: scope sets the thread flag");
        Housekeeping(); // Would try to recursively lock std::mutex without the flag check.
    }
    Check(!insideCore, "housekeeping: scope restores the flag");
}
} // namespace
int main(int argc, char **argv)
{
    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
        debug->EnableDebugLayer();
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter> warp;
    Check(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))), "DXGI factory is available");
    Check(factory && SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))), "the WARP adapter is available");
    ComPtr<ID3D12Device> device;
    Check(warp && SUCCEEDED(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))),
          "a D3D12 WARP device is created");
    if (failures != 0)
        return EXIT_FAILURE;
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue, other;
    Check(SUCCEEDED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))) &&
              SUCCEEDED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&other))),
          "two direct queues are created");
    if (failures != 0)
        return EXIT_FAILURE;
    TestInsideCore();
    TestRecordingExpiry(device.Get(), queue.Get());
    TestEmptyGateDrain(device.Get(), queue.Get());
    TestRecordingLifetime(device.Get(), queue.Get(), other.Get());
    TestQueueLifetime(device.Get());
    if (argc > 1) TestTemporalFallback(device.Get(), queue.Get(), argv[1]);
    TestPool(device.Get());
    failures += TestComputePipeline(device.Get());
    TestSubmission(device.Get(), queue.Get(), other.Get());
    TestGraveyard(device.Get(), queue.Get());
    if (failures != 0)
    {
        std::cerr << failures << " GPU pool test failure(s)\n";
        return EXIT_FAILURE;
    }
    std::cout << "Optimizer FPS core GPU pools test passed\n";
    return EXIT_SUCCESS;
}
