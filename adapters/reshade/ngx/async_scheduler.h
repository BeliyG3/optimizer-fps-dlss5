#pragma once

// Temporal mode 3: the model runs on a background queue on private copies of the host's inputs.
// This header carries the job's own objects; the scheduling around them lives in the .cpp.

#include "hook_common.h"

#include <cstdint>

namespace pwhook {

struct FeatureState;

struct AsyncJob final : public pwngx::Disposable {
    static constexpr std::uint32_t kSlots = 3;
    ID3D12Device *device = nullptr; // the real device (queue, private resources)
    ID3D12CommandQueue *queue = nullptr;
    D3D12_COMMAND_LIST_TYPE type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
    ID3D12Fence *fInputs = nullptr, *fModel = nullptr;
    HANDLE event = nullptr;
    // Queue cap (TemporalMaxQueue): fHost is signalled on the host's queue with the evaluate index once
    // that evaluate's list is queued; before recording a frame the CPU waits until at most `maxQueue`
    // frames are still unfinished on the GPU. The copies of a kick then reach the GPU within that many
    // frames instead of the host's whole queue depth (5-6 frames in BG3), which is most of the
    // residual's age; the GPU stays busy as long as one frame is queued (the low-latency principle).
    ID3D12Fence *fHost = nullptr;
    HANDLE hostEvent = nullptr;
    std::uint64_t hostSignalled = 0;
    std::uint64_t queueWaits = 0;
    float queueWaitMs = 0.0f; // smoothed CPU wait per evaluate
    // Initial synchronisation: on the first frame of the background mode the CPU waits for everything the
    // host has queued, so the first pass starts from an empty queue (the job is created on activation).
    bool startSync = true;
    ID3D12CommandAllocator *allocators[kSlots] = {};
    ID3D12GraphicsCommandList *lists[kSlots] = {};
    std::uint64_t slotJob[kSlots] = {};
    ID3D12Resource *colorBg = nullptr, *depthBg = nullptr, *mvBg = nullptr, *outputBg[2] = {};
    ID3D12Resource *accBg = nullptr; // 26.23: the machine's accumulated chain (R16G16_FLOAT) - separate from mvBg, whose format is the host's (R32G32_FLOAT in Fallen Order)
    D3D12_RESOURCE_STATES outputState[2] = {D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON};
    ID3D12Resource *baseBg[2] = {}; // warped: the pass's packed colour unpacked without the model (temporal base)
    D3D12_RESOURCE_STATES baseState[2] = {D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON};
    ID3D12DescriptorHeap *baseRtvHeap = nullptr; // [0], [1] for baseBg
    UINT rtvIncrement = 0;
    D3D12_CPU_DESCRIPTOR_HANDLE BaseRtv(int i) const { D3D12_CPU_DESCRIPTOR_HANDLE h = baseRtvHeap->GetCPUDescriptorHandleForHeapStart(); h.ptr += static_cast<SIZE_T>(i) * rtvIncrement; return h; }
    D3D12_RESOURCE_DESC colorDesc{}, depthDesc{}, motionDesc{}, outputDesc{};
    int outIndex = 0;              // outputBg the in-flight (or last) pass writes
    std::uint64_t jobId = 0;       // last kicked pass (fence value)
    bool inflight = false;
    bool discard = false;          // the in-flight pass predates a host reset: drop it
    bool mvIsAcc = false;
    bool wantDirect = false;       // evaluate failed on the compute queue: rebuild with a direct queue
    ID3D12GraphicsCommandList *kickCmd = nullptr;
    bool signalPending = false;    // the host list with the input copies has not been seen submitted yet
    ID3D12CommandQueue *submitQueue = nullptr; // the queue the tagged list was submitted on (ReShade reports the submission before it happens)
    bool submitSeen = false;
    bool fallbackLogged = false;
    std::uint32_t sinceKick = 1000, age = 0;
    std::uint64_t passes = 0, forcedWaits = 0, stalls = 0, failures = 0;
    std::uint64_t ageSum = 0; // sum of the residual ages at adoption (average = ageSum / passes)
    bool bgPackValid = false;  // warped path: descriptors of the reserved pack slot written for the private copies
    bool bgPackIsAcc = false;  // ... with the accumulated displacement (scale 1) as the motion source
    bool warpFailed = false;   // warped path: Pack/Unpack failed on the background list; no more kicks
    // Timing on the background queue: a timestamp pair per slot, resolved into a readback buffer.
    ID3D12QueryHeap *queryHeap = nullptr;
    ID3D12Resource *queryReadback = nullptr;
    UINT64 frequency = 0;
    float lastModelMs = 0.0f;
    LARGE_INTEGER windowStart{};
    std::uint64_t windowPasses = 0;
    float passesPerSecond = 0.0f;

    ~AsyncJob() override { Release(); }
    bool Matches(const D3D12_RESOURCE_DESC &c, const D3D12_RESOURCE_DESC &d, const D3D12_RESOURCE_DESC &m, const D3D12_RESOURCE_DESC &o) const
    {
        auto same = [](const D3D12_RESOURCE_DESC &a, const D3D12_RESOURCE_DESC &b) { return a.Width == b.Width && a.Height == b.Height && a.Format == b.Format; };
        return same(c, colorDesc) && same(d, depthDesc) && same(m, motionDesc) && same(o, outputDesc);
    }
    // Lets the pass in flight finish: releases its input wait if the host's signal is still owed, then
    // waits for the model fence on the CPU (3 s cap, like the other GPU waits).
    // Also flushes the host queue (a plain Signal may sit in the runtime's submission batch until the
    // next submission, which never comes while the host thread is inside this call) and waits for the
    // pass on the CPU; the graveyard gate covers the case where the wait times out.
    void Settle()
    {
        if (!queue || !fModel) return;
        if (AsyncVerbose()) pwngx::Log(false, "Optimizer FPS NGX hook [async] settle: job %llu inflight %d fModel %llu fInputs %llu signalPending %d", (unsigned long long) jobId, inflight ? 1 : 0, (unsigned long long) fModel->GetCompletedValue(), (unsigned long long) fInputs->GetCompletedValue(), signalPending ? 1 : 0);
        if (signalPending && fInputs) {
            pwngx::SignalRegisteredQueues(device, nullptr, fInputs, jobId);
            signalPending = false;
        }
        if (inflight && fModel->GetCompletedValue() < jobId) {
            const bool hostIdle = pwngx::WaitForGpu(device, nullptr); // signal + CPU wait on the host queue: flushes its batch, so our input wait resolves
            if (AsyncVerbose()) pwngx::Log(false, "Optimizer FPS NGX hook [async] settle: host queue wait %s, fModel now %llu", hostIdle ? "ok" : "FAILED", (unsigned long long) fModel->GetCompletedValue());
            // The event is reused: reset it first, or an earlier completion leaves it signalled and the wait
            // returns at once (the model would then be re-created while the pass still runs -> TDR).
            if (event) ResetEvent(event);
            if (event && fModel->GetCompletedValue() < jobId && SUCCEEDED(fModel->SetEventOnCompletion(jobId, event))) {
                if (WaitForSingleObject(event, 3000) != WAIT_OBJECT_0)
                    pwngx::Log(true, "Optimizer FPS NGX hook: background pass %llu did not finish within 3 s; its objects stay in the graveyard until its fence", static_cast<unsigned long long>(jobId));
            }
        }
        if (AsyncVerbose()) pwngx::Log(false, "Optimizer FPS NGX hook [async] settled (fModel %llu)", (unsigned long long) fModel->GetCompletedValue());
        inflight = false;
    }
    void Release()
    {
        Settle();
        if (AsyncVerbose() && queue) pwngx::Log(false, "Optimizer FPS NGX hook [async] releasing the background job objects");
        for (auto &l : lists) if (l) { l->Release(); l = nullptr; }
        for (auto &a : allocators) if (a) { a->Release(); a = nullptr; }
        for (ID3D12Resource **r : {&colorBg, &depthBg, &mvBg, &accBg, &outputBg[0], &outputBg[1], &baseBg[0], &baseBg[1], &queryReadback}) if (*r) { (*r)->Release(); *r = nullptr; }
        if (baseRtvHeap) { baseRtvHeap->Release(); baseRtvHeap = nullptr; }
        if (queryHeap) { queryHeap->Release(); queryHeap = nullptr; }
        if (fInputs) { fInputs->Release(); fInputs = nullptr; }
        if (fModel) { fModel->Release(); fModel = nullptr; }
        if (fHost) { fHost->Release(); fHost = nullptr; }
        if (event) { CloseHandle(event); event = nullptr; }
        if (hostEvent) { CloseHandle(hostEvent); hostEvent = nullptr; }
        hostSignalled = 0;
        if (queue) { queue->Release(); queue = nullptr; }
        if (device) { device->Release(); device = nullptr; }
        inflight = false;
    }
};

struct EvalContext;

// Temporal mode 3: the host's frame either kicks a new background pass or reprojects the last one.
// `host` carries the warped path's frame context (null in the no-warp path); kNotHandled declines.
int AsyncTemporalEvaluate(FeatureState &st, ID3D12GraphicsCommandList *cmd, void *params, void *callback, const EvalContext *host);
// The host submitted `list` on `queue` (ReShade execute_command_list): lets the background queue
// start the pass whose input copies were recorded on that list.
void OnCommandListExecuted(ID3D12CommandQueue *queue, ID3D12GraphicsCommandList *list);

} // namespace pwhook
