#include "async_scheduler.h"

#include "debug_readback.h"
#include "feature_state.h"
#include "hook_context.h"
#include "host_depth_state.h"
#include "ngx_params.h"
#include "temporal_controller.h"
#include "timing.h"
#include "warp_recorder.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>

namespace pwhook {

// ---------------------------------------------------------------------------------------------
// Temporal mode 3: the model in the background.
// ---------------------------------------------------------------------------------------------
// Private-data tag on the host's command list that carries a kick's input copies: the submission
// event sees the native list while the hook sees ReShade's proxy, so pointers cannot be compared.
// {7B3C1D42-5A8E-4F0B-9C21-PeripheralWarpAsync}
static const GUID kAsyncKickTag = {0x7b3c1d42, 0x5a8e, 0x4f0b, {0x9c, 0x21, 0x50, 0x57, 0x41, 0x73, 0x79, 0x6e}};

bool CreateReadbackBuffer(ID3D12Device *device, UINT64 size, ID3D12Resource **out)
{
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC d{}; d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; d.Width = size; d.Height = 1; d.DepthOrArraySize = 1; d.MipLevels = 1;
    d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(out)));
}

bool EnsureAsync(FeatureState &st, const D3D12_RESOURCE_DESC &colorDesc, const D3D12_RESOURCE_DESC &depthDesc,
                 const D3D12_RESOURCE_DESC &motionDesc, const D3D12_RESOURCE_DESC &outputDesc)
{
    if (st.async && st.async->Matches(colorDesc, depthDesc, motionDesc, outputDesc) && !st.async->wantDirect) return true;
    // Direct by default: on the bench a compute queue was starved by the saturated host queue (the pass
    // only finished when the host stalled); DebugAsyncCompute=1 tries the compute queue instead.
    const bool direct = (st.async && st.async->wantDirect) || !Ctx().diag.asyncCompute;
    if (st.async) {
        pwngx::GateSet gate;
        BuryAsync(st, &gate);
    }
    if (st.realDevice == nullptr) { TemporalReason(st, "background mode: no device"); return false; }
    auto job = std::make_unique<AsyncJob>();
    AsyncJob &a = *job;
    a.device = st.realDevice; a.device->AddRef();
    a.type = direct ? D3D12_COMMAND_LIST_TYPE_DIRECT : D3D12_COMMAND_LIST_TYPE_COMPUTE;
    a.colorDesc = colorDesc; a.depthDesc = depthDesc; a.motionDesc = motionDesc; a.outputDesc = outputDesc;
    auto fail = [&](const char *what) { TemporalReason(st, "background mode: %s", what); return false; };
    // The queue and the lists come from the command list's device (ReShade's proxy when present) so the
    // host's own hooks, which look the list up through ReShade, still recognise what the model records into.
    ID3D12Device *listDevice = st.device ? st.device : a.device;
    // High priority: with the host's queue saturated (uncapped fps) a normal-priority queue may not be
    // scheduled until the host stalls, and the pass would only finish at the forced wait.
    D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = a.type; qd.Priority = Ctx().diag.asyncNormalPriority ? D3D12_COMMAND_QUEUE_PRIORITY_NORMAL : D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
    pwngx::SetQueueRegistrationSuppressed(true); // the proxy announces the new queue through ReShade; it is not a host queue
    HRESULT hrQueue = E_FAIL;
    const char *priorityName = "high";
    // 26.25: GLOBAL_REALTIME pre-empts the host's queue at the hardware scheduler, so the pass finishes sooner and the
    // residual's age stays low even at uncapped fps. It needs SeIncreaseBasePriorityPrivilege in the process token
    // (elevated games / admin accounts); the request is made, and a refusal falls back to HIGH. DebugAsyncNoRealtime=1 skips it.
    if (!Ctx().diag.asyncNoRealtime && qd.Priority == D3D12_COMMAND_QUEUE_PRIORITY_HIGH) {
        HANDLE token = nullptr;
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
            TOKEN_PRIVILEGES tp{}; tp.PrivilegeCount = 1; tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
            if (LookupPrivilegeValueW(nullptr, L"SeIncreaseBasePriorityPrivilege", &tp.Privileges[0].Luid)) AdjustTokenPrivileges(token, FALSE, &tp, sizeof(tp), nullptr, nullptr);
            CloseHandle(token);
        }
        D3D12_COMMAND_QUEUE_DESC rt = qd; rt.Priority = D3D12_COMMAND_QUEUE_PRIORITY_GLOBAL_REALTIME;
        hrQueue = listDevice->CreateCommandQueue(&rt, IID_PPV_ARGS(&a.queue));
        if (SUCCEEDED(hrQueue)) priorityName = "global realtime";
        else Log(false, "Optimizer FPS NGX hook: background queue: GLOBAL_REALTIME priority refused (0x%08X; the process lacks the privilege) - using HIGH", (unsigned) hrQueue);
    }
    if (FAILED(hrQueue)) hrQueue = listDevice->CreateCommandQueue(&qd, IID_PPV_ARGS(&a.queue));
    pwngx::SetQueueRegistrationSuppressed(false);
    if (FAILED(hrQueue)) return fail("queue creation failed");
    Log(false, "Optimizer FPS NGX hook: background queue priority: %s", priorityName);
    if (FAILED(a.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&a.fInputs))) || FAILED(a.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&a.fModel))))
        return fail("fence creation failed");
    a.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (FAILED(a.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&a.fHost)))) return fail("host fence creation failed");
    a.hostEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    for (std::uint32_t i = 0; i < AsyncJob::kSlots; ++i) {
        if (FAILED(listDevice->CreateCommandAllocator(a.type, IID_PPV_ARGS(&a.allocators[i])))) return fail("allocator creation failed");
        if (FAILED(listDevice->CreateCommandList(0, a.type, a.allocators[i], nullptr, IID_PPV_ARGS(&a.lists[i])))) return fail("command list creation failed");
        a.lists[i]->Close();
    }
    if (!pwngx::CreateTexture(a.device, (UINT) colorDesc.Width, colorDesc.Height, colorDesc.Format, D3D12_RESOURCE_FLAG_NONE, &a.colorBg) ||
        !pwngx::CreateTexture(a.device, (UINT) depthDesc.Width, depthDesc.Height, depthDesc.Format, D3D12_RESOURCE_FLAG_NONE, &a.depthBg) ||
        !pwngx::CreateTexture(a.device, (UINT) motionDesc.Width, motionDesc.Height, motionDesc.Format, D3D12_RESOURCE_FLAG_NONE, &a.mvBg) ||
        !pwngx::CreateTexture(a.device, (UINT) motionDesc.Width, motionDesc.Height, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_NONE, &a.accBg) ||
        !pwngx::CreateTexture(a.device, (UINT) outputDesc.Width, outputDesc.Height, outputDesc.Format, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, &a.outputBg[0]) ||
        !pwngx::CreateTexture(a.device, (UINT) outputDesc.Width, outputDesc.Height, outputDesc.Format, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, &a.outputBg[1]))
        return fail("private copies could not be allocated");
    if (st.warped && st.outputView != DXGI_FORMAT_UNKNOWN) {
        const DXGI_FORMAT view = st.outputView;
        if (!pwngx::CreateTexture(a.device, st.nativeWidth, st.nativeHeight, view, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, &a.baseBg[0]) ||
            !pwngx::CreateTexture(a.device, st.nativeWidth, st.nativeHeight, view, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, &a.baseBg[1]))
            return fail("base targets could not be allocated");
        D3D12_DESCRIPTOR_HEAP_DESC hd{}; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = 2;
        if (FAILED(listDevice->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&a.baseRtvHeap)))) return fail("base RTV heap creation failed");
        a.rtvIncrement = listDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        D3D12_RENDER_TARGET_VIEW_DESC rd{}; rd.Format = view; rd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        listDevice->CreateRenderTargetView(a.baseBg[0], &rd, a.BaseRtv(0));
        listDevice->CreateRenderTargetView(a.baseBg[1], &rd, a.BaseRtv(1));
    }
    D3D12_QUERY_HEAP_DESC qh{}; qh.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP; qh.Count = 2 * AsyncJob::kSlots;
    if (SUCCEEDED(a.device->CreateQueryHeap(&qh, IID_PPV_ARGS(&a.queryHeap))) && CreateReadbackBuffer(a.device, 16 * AsyncJob::kSlots, &a.queryReadback))
        a.queue->GetTimestampFrequency(&a.frequency);
    // A proxy queue announces itself through ReShade's init_command_queue (graphics type only); the
    // background pass is waited for explicitly in Release() either way.
    QueryPerformanceCounter(&a.windowStart);
    st.async = std::move(job);
    st.temporal->Invalidate();
    std::snprintf(Ctx().status.asyncQueue, sizeof(Ctx().status.asyncQueue), "%s", direct ? "direct" : "compute");
    Log(false, "Optimizer FPS NGX hook: background model queue ready (%s; colour %llux%u, output %llux%u)", direct ? "direct" : "compute",
        (unsigned long long) colorDesc.Width, colorDesc.Height, (unsigned long long) outputDesc.Width, outputDesc.Height);
    return true;
}

struct AsyncCtx {
    FeatureState *st;
    ID3D12GraphicsCommandList *cmd;
    void *params;
    void *callback;
    ID3D12Resource *color, *depth, *motion, *output, *ui, *uiAlpha, *backbuffer;
    D3D12_RESOURCE_STATES depthState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE; // 26.7.2
    UINT depthSub = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;                          // 26.7.3
    Subrect colorRect, depthRect, motionRect, outputRect;
    float mvScaleX, mvScaleY;
    bool depthInverted;
    pwtemporal::FrameInputs tin;
    bool hostReset;
    bool paramsRewritten;
    const EvalContext *host; // warped path: the host frame's pack slot etc. (null in the native path)
    bool useBase;            // warped path with the temporal base on
    volatile int stage;
    int result;
};

// The host's view of the parameter block, for RestoreParams after a background kick rewrote it.
EvalContext HostContext(const AsyncCtx &c)
{
    EvalContext h{};
    h.st = c.st; h.cmd = c.cmd; h.params = c.params; h.callback = c.callback;
    h.color = c.color; h.depth = c.depth; h.motion = c.motion; h.hostMotion = c.motion; h.output = c.output;
    h.ui = c.ui; h.uiAlpha = c.uiAlpha; h.backbuffer = c.backbuffer;
    h.colorRect = c.colorRect; h.depthRect = c.depthRect; h.motionRect = c.motionRect; h.outputRect = c.outputRect;
    h.mvScaleX = c.mvScaleX; h.mvScaleY = c.mvScaleY;
    return h;
}

void AsyncRestoreParams(AsyncCtx &c)
{
    if (!c.paramsRewritten) return;
    EvalContext h = HostContext(c);
    RestoreParams(h);
    c.paramsRewritten = false;
}

// Copies a host input into its private twin on the host's list; both end up in kHostInputState.
void AsyncCopyInput(ID3D12GraphicsCommandList *cmd, ID3D12Resource *host, ID3D12Resource *bg, D3D12_RESOURCE_STATES &bgState,
                    D3D12_RESOURCE_STATES hostState = kHostInputState, UINT hostSub = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES)
{
    BarrierExternal(cmd, host, hostState, D3D12_RESOURCE_STATE_COPY_SOURCE, hostSub);
    Barrier(cmd, bg, bgState, D3D12_RESOURCE_STATE_COPY_DEST);
    if (hostSub != D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES) {
        D3D12_TEXTURE_COPY_LOCATION dstP{}, srcP{};
        dstP.pResource = bg; dstP.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dstP.SubresourceIndex = 0;
        srcP.pResource = host; srcP.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; srcP.SubresourceIndex = 0;
        cmd->CopyTextureRegion(&dstP, 0, 0, 0, &srcP, nullptr);
    } else {
        cmd->CopyResource(bg, host);
    }
    Barrier(cmd, bg, bgState, kHostInputState);
    BarrierExternal(cmd, host, D3D12_RESOURCE_STATE_COPY_SOURCE, hostState, hostSub);
}

int AsyncBody(AsyncCtx &c)
{
    FeatureState &st = *c.st;
    AsyncJob &a = *st.async;
    if (AsyncVerbose())
        Log(false, "Optimizer FPS NGX hook [async] eval %llu: inflight %d job %llu fModel %llu fInputs %llu sinceKick %u age %u signalPending %d",
            static_cast<unsigned long long>(Ctx().evalCounter), a.inflight ? 1 : 0, static_cast<unsigned long long>(a.jobId),
            static_cast<unsigned long long>(a.fModel->GetCompletedValue()), static_cast<unsigned long long>(a.fInputs->GetCompletedValue()), a.sinceKick, a.age, a.signalPending ? 1 : 0);
    ID3D12GraphicsCommandList *cmd = c.cmd;
    const int every = std::clamp(Ctx().temporal.every, 1, 8);
    const std::uint32_t maxAge = static_cast<std::uint32_t>(std::clamp(Ctx().temporal.maxAge, 3, 16));
    D3D12_RESOURCE_STATES bgInputState = kHostInputState; // private copies rest in the host's input state

    if (a.startSync) {
        a.startSync = false;
        LARGE_INTEGER f0, t0, t1;
        QueryPerformanceFrequency(&f0);
        QueryPerformanceCounter(&t0);
        const bool ok = WaitForGpu(st.realDevice, st.device);
        QueryPerformanceCounter(&t1);
        Log(false, "Optimizer FPS NGX hook: background mode activated; initial GPU synchronisation %s (%.1f ms)", ok ? "done" : "skipped (no registered queue or timeout)",
            static_cast<float>(t1.QuadPart - t0.QuadPart) * 1000.0f / static_cast<float>(f0.QuadPart));
    }

    // 0. Queue cap: the previous evaluate's list is already submitted (the host executes each frame's
    //    list before the next evaluate), so a signal issued now lands behind it; then wait on the CPU
    //    until at most maxQueue frames are unfinished.
    const int maxQueue = std::clamp(Ctx().temporal.maxQueue, 0, 8);
    if (maxQueue > 0 && a.fHost && a.hostEvent && Ctx().evalCounter > 1) {
        const std::uint64_t prev = Ctx().evalCounter - 1;
        if (prev > a.hostSignalled && pwngx::SignalRegisteredQueues(st.realDevice, st.device, a.fHost, prev)) a.hostSignalled = prev;
        const std::uint64_t target = a.hostSignalled > static_cast<std::uint64_t>(maxQueue) ? a.hostSignalled - static_cast<std::uint64_t>(maxQueue) : 0;
        float waited = 0.0f;
        if (target > 0 && a.fHost->GetCompletedValue() < target) {
            LARGE_INTEGER f0, t0, t1;
            QueryPerformanceFrequency(&f0);
            QueryPerformanceCounter(&t0);
            ResetEvent(a.hostEvent);
            if (SUCCEEDED(a.fHost->SetEventOnCompletion(target, a.hostEvent))) WaitForSingleObject(a.hostEvent, 200);
            QueryPerformanceCounter(&t1);
            waited = static_cast<float>(t1.QuadPart - t0.QuadPart) * 1000.0f / static_cast<float>(f0.QuadPart);
            ++a.queueWaits;
        }
        a.queueWaitMs = a.queueWaitMs * 0.95f + waited * 0.05f;
    } else {
        a.queueWaitMs *= 0.95f;
    }

    // The submission event fires before the host's ExecuteCommandLists, so the signal is issued here,
    // one evaluate later, when the tagged list is certainly queued ahead of it. Without the event the
    // fallback signals every registered queue of the device.
    if (a.signalPending) {
        if (a.submitSeen && a.submitQueue) {
            a.submitQueue->Signal(a.fInputs, a.jobId);
        } else {
            pwngx::SignalRegisteredQueues(st.realDevice, st.device, a.fInputs, a.jobId);
            if (!a.fallbackLogged) { a.fallbackLogged = true; Log(false, "Optimizer FPS NGX hook: background mode: host submission not observed, signalling the registered queues one frame later"); }
        }
        a.signalPending = false;
        a.submitSeen = false;
    }
    if (c.hostReset) {
        st.temporal->Invalidate();
        if (a.inflight) a.discard = true;
    }

    // 1. Motion accumulation: the main chain (to the residual on screen) and the pending chain (to the
    //    frame of the last kick). The pending chain runs from the frame after a kick until the next
    //    kick, whether or not the pass is still in flight: the next pass's model needs the displacement
    //    to the previous pass's frame (its history), which is several frames back, not one. (Before
    //    26.5 it was only accumulated while the pass was in flight, and a kick on the adoption frame
    //    handed the model the plain one-frame vector: its history was then misaligned by age-1 frames,
    //    which doubled the edges of far objects on every pass - the background-mode flicker.)
    c.stage = StageTemporalAccumulate;
    if (st.temporal->HasResidual()) st.temporal->RecordAccumulate(cmd, c.tin);
    if (a.jobId > 0 && !st.temporal->PendingMirrorsAcc()) st.temporal->RecordAccumulatePending(cmd, c.tin);

    // 2. Adopt the finished pass (or force the host queue to wait for it when the residual is too old).
    bool ready = a.inflight && a.fModel->GetCompletedValue() >= a.jobId;
    if (!ready && a.inflight && a.age >= maxAge) {
        if (a.signalPending) { // never let the host queue wait for a pass that still waits for the host's signal
            if (a.submitSeen && a.submitQueue) a.submitQueue->Signal(a.fInputs, a.jobId);
            else pwngx::SignalRegisteredQueues(st.realDevice, st.device, a.fInputs, a.jobId);
            a.signalPending = false; a.submitSeen = false;
        }
        pwngx::WaitRegisteredQueues(st.realDevice, st.device, a.fModel, a.jobId);
        ++a.forcedWaits;
        ready = true;
    }
    bool adoptedThisFrame = false;
    if (ready) {
        adoptedThisFrame = true;
        if (!a.discard) {
            c.stage = StageTemporalResidual;
            pwtemporal::FrameInputs bg = c.tin;
            bg.color = a.colorBg; bg.depth = a.depthBg; bg.motion = a.mvIsAcc ? a.accBg : a.mvBg;
            bg.hostInputState = bgInputState;
            bg.depthState = bgInputState;
            if (c.useBase && a.baseBg[a.outIndex]) { bg.base = a.baseBg[a.outIndex]; bg.baseState = a.baseState[a.outIndex]; }
            bg.residualBlend = a.mvIsAcc ? kResidualBlend : 0.0f; // mvBg = displacement to the previous pass's frame
            bg.blendFromMotion = true;
            bg.motionIsPassChain = a.mvIsAcc;
            if (a.mvIsAcc) {
                // mvBg holds the displacement from this pass's frame back to the previous pass's frame (scale 1).
                bg.mvScaleX = bg.mvScaleY = 1.0f; // the chain is already scaled and signed
                bg.motionSign = 1.0f;
                bg.motionView = DXGI_FORMAT_R16G16_FLOAT;
            }
            st.temporal->RecordResidual(cmd, bg, a.outputBg[a.outIndex], kHostOutputState);
            st.temporal->PromotePending();
            const std::uint32_t adoptedAge = a.age;
            a.ageSum += adoptedAge;
            a.age = 0;
            ++a.passes; ++a.windowPasses;
            if (a.queryReadback && a.frequency) {
                const std::uint32_t slot = static_cast<std::uint32_t>(a.jobId % AsyncJob::kSlots);
                const D3D12_RANGE range{slot * 16, slot * 16 + 16};
                UINT64 *ticks = nullptr;
                if (SUCCEEDED(a.queryReadback->Map(0, &range, reinterpret_cast<void **>(&ticks)))) {
                    if (ticks[slot * 2 + 1] > ticks[slot * 2]) a.lastModelMs = static_cast<float>(ticks[slot * 2 + 1] - ticks[slot * 2]) * 1000.0f / static_cast<float>(a.frequency);
                    const D3D12_RANGE none{0, 0};
                    a.queryReadback->Unmap(0, &none);
                }
            }
            LARGE_INTEGER now, freq; QueryPerformanceCounter(&now); QueryPerformanceFrequency(&freq);
            const double elapsed = static_cast<double>(now.QuadPart - a.windowStart.QuadPart) / static_cast<double>(freq.QuadPart);
            if (elapsed >= 1.0) { a.passesPerSecond = static_cast<float>(a.windowPasses / elapsed); a.windowPasses = 0; a.windowStart = now; }
            if (a.passes <= 3 || a.passes % 30 == 0 || Ctx().temporal.debugLog)
                Log(false, "Optimizer FPS NGX hook: background pass %llu adopted (age %u frames at adoption, average %.1f, %.2f ms on the %s queue, %.1f passes/s, forced waits %llu, stalls %llu, failures %llu, queue cap wait %.2f ms/frame over %llu frames)",
                    static_cast<unsigned long long>(a.passes), adoptedAge, static_cast<double>(a.ageSum) / static_cast<double>(a.passes), a.lastModelMs,
                    a.type == D3D12_COMMAND_LIST_TYPE_DIRECT ? "direct" : "compute", a.passesPerSecond, static_cast<unsigned long long>(a.forcedWaits),
                    static_cast<unsigned long long>(a.stalls), static_cast<unsigned long long>(a.failures), a.queueWaitMs, static_cast<unsigned long long>(a.queueWaits));
        } else {
            a.discard = false;
            st.temporal->ResetPending();
        }
        a.inflight = false;
    }

    // 3. Kick the next pass: copies of the inputs on the host's list, the model on ours.
    if (!a.inflight && a.sinceKick >= static_cast<std::uint32_t>(every) && !a.wantDirect && !a.warpFailed) {
        const std::uint64_t nextJob = a.jobId + 1;
        const std::uint32_t slot = static_cast<std::uint32_t>(nextJob % AsyncJob::kSlots);
        if (a.fModel->GetCompletedValue() < a.slotJob[slot]) {
            ResetEvent(a.event);
            if (SUCCEEDED(a.fModel->SetEventOnCompletion(a.slotJob[slot], a.event))) WaitForSingleObject(a.event, 3000);
            ++a.stalls;
        }
        c.stage = StageCopyOut;
        D3D12_RESOURCE_STATES s = bgInputState;
        AsyncCopyInput(cmd, c.color, a.colorBg, s);
        AsyncCopyInput(cmd, c.depth, a.depthBg, s, c.depthState, c.depthSub);
        if (st.temporal->PendingMirrorsAcc() && st.temporal->AccValid() && !Ctx().temporal.debugSingleFrameMotion) {
            // Adopted since the last kick: the main chain is the displacement to the last pass's frame.
            st.temporal->RecordCopyAcc(cmd, false, a.accBg, bgInputState);
            a.mvIsAcc = true;
        } else if (st.temporal->PendingValid() && !Ctx().temporal.debugSingleFrameMotion) {
            st.temporal->RecordCopyAcc(cmd, true, a.accBg, bgInputState);
            a.mvIsAcc = true;
        } else {
            AsyncCopyInput(cmd, c.motion, a.mvBg, s);
            a.mvIsAcc = false;
        }
        const int next = 1 - a.outIndex;
        ID3D12CommandAllocator *alloc = a.allocators[slot];
        ID3D12GraphicsCommandList *list = a.lists[slot];
        const HRESULT hrAlloc = alloc->Reset();
        const HRESULT hrList = SUCCEEDED(hrAlloc) ? list->Reset(alloc, nullptr) : E_FAIL;
        if (FAILED(hrAlloc) || FAILED(hrList)) {
            ++a.failures;
            if (a.failures <= 3) Log(true, "Optimizer FPS NGX hook: background list reset failed (allocator 0x%08lX, list 0x%08lX)", (unsigned long) hrAlloc, (unsigned long) hrList);
            FallbackOutput(st, cmd, c.color, c.colorRect, c.output, c.outputRect, &c.tin, "background list reset failed");
            return kNgxSuccess;
        }
        Barrier(list, a.outputBg[next], a.outputState[next], kHostOutputState);
        int result = kNgxSuccess;
        if (a.queryHeap) list->EndQuery(a.queryHeap, D3D12_QUERY_TYPE_TIMESTAMP, slot * 2);
        if (st.warped && st.adapter) {
            // Warped: Pack -> model -> Unpack on the background list, all from the private copies; the
            // reserved pack slot is written once per motion source (host vectors / accumulated displacement).
            if (!a.bgPackValid || a.bgPackIsAcc != a.mvIsAcc) {
                pw::InputDescriptionV2 input = pw::DefaultInputDescriptionV2(st.nativeWidth, st.nativeHeight);
                input.colorRect = {c.colorRect.x, c.colorRect.y, c.colorRect.w, c.colorRect.h};
                input.depthRect = {c.depthRect.x, c.depthRect.y, c.depthRect.w, c.depthRect.h};
                input.motionRect = {c.motionRect.x, c.motionRect.y, c.motionRect.w, c.motionRect.h};
                input.confidenceRect = {0, 0, 1, 1};
                const float adjust = Ctx().motionScaleAdjust * (Ctx().motionInvert ? -1.0f : 1.0f);
                input.motionScaleX = adjust * (a.mvIsAcc ? 1.0f : c.mvScaleX) * static_cast<float>(st.nativeWidth) / static_cast<float>(std::max(1u, c.motionRect.w));
                input.motionScaleY = adjust * (a.mvIsAcc ? 1.0f : c.mvScaleY) * static_cast<float>(st.nativeHeight) / static_cast<float>(std::max(1u, c.motionRect.h));
                input.motionDirection = pw::MotionDirection::CurrentToPrevious;
                input.depthConvention = c.depthInverted ? pw::DepthConvention::Reversed : pw::DepthConvention::Normal;
                input.colorEncoding = EncodingFor(st.colorView);
                const pw::D3D12SourceResources sources = {{a.colorBg, st.colorView},
                                                          {a.depthBg, TypedView(a.depthDesc.Format, true)},
                                                          {a.mvIsAcc ? a.accBg : a.mvBg, a.mvIsAcc ? DXGI_FORMAT_R16G16_FLOAT : TypedView(a.motionDesc.Format, false)},
                                                          {nullptr, DXGI_FORMAT_UNKNOWN}};
                st.packKeys[FeatureState::kBgPackSlot] = FeatureState::SlotKey{};
                st.unpackValid[FeatureState::kBgPackSlot] = false;
                const pw::AdapterStatus ds = st.adapter->WriteSourceDescriptorsV2(FeatureState::kBgPackSlot, sources, input);
                if (ds != pw::AdapterStatus::Ok) {
                    a.warpFailed = true;
                    TemporalReason(st, "background mode: pack descriptors for the private copies: %s", pw::AdapterStatusString(ds));
                    Log(true, "Optimizer FPS NGX hook: %s", Ctx().status.temporalReason);
                    list->Close();
                    FallbackOutput(st, cmd, c.color, c.colorRect, c.output, c.outputRect, &c.tin, Ctx().status.temporalReason);
                    return kNgxSuccess;
                }
                a.bgPackValid = true;
                a.bgPackIsAcc = a.mvIsAcc;
            }
            EvalContext bg{};
            bg.st = &st; bg.cmd = list; bg.params = c.params; bg.callback = c.callback;
            bg.color = a.colorBg; bg.depth = a.depthBg; bg.motion = a.mvIsAcc ? a.accBg : a.mvBg; bg.output = a.outputBg[next];
            bg.ui = c.ui; bg.uiAlpha = c.uiAlpha; bg.backbuffer = c.backbuffer;
            bg.colorRect = c.colorRect; bg.depthRect = c.depthRect; bg.motionRect = c.motionRect; bg.outputRect = c.outputRect;
            bg.mvScaleX = a.mvIsAcc ? 1.0f : c.mvScaleX; bg.mvScaleY = a.mvIsAcc ? 1.0f : c.mvScaleY;
            bg.slot = bg.packSlot = bg.packSet = FeatureState::kBgPackSlot;
            bg.unpackSlot = FeatureState::kPackSlots + FeatureState::kBgPackSlot;
            bg.depthConvention = c.depthInverted ? pw::DepthConvention::Reversed : pw::DepthConvention::Normal;
            bg.motionIsAcc = false; // the private copy rests in the input state and takes the same barriers as the host's vectors
            if (c.useBase && a.baseBg[next]) { bg.wantBase = true; bg.baseTarget = a.baseBg[next]; bg.baseTargetState = &a.baseState[next]; bg.baseRtv = a.BaseRtv(next); }
            c.paramsRewritten = true;
            c.stage = StageModel;
            result = WarpedBody(bg);
            c.stage = StageParamsRestore;
            AsyncRestoreParams(c);
            if (result == kPackFailed) {
                a.warpFailed = true;
                Log(true, "Optimizer FPS NGX hook: background mode: %s; no more background passes", Ctx().status.reason);
            }
        } else {
            c.stage = StageParamsWrite;
            SetResource(c.params, "DLSSNR.Color", a.colorBg);
            SetResource(c.params, "DLSSNR.Depth", a.depthBg);
            SetResource(c.params, "DLSSNR.MVec", a.mvIsAcc ? a.accBg : a.mvBg);
            SetResource(c.params, "DLSSNR.Output", a.outputBg[next]);
            if (a.mvIsAcc) { SetFloat(c.params, "DLSSNR.MVecScaleX", 1.0f); SetFloat(c.params, "DLSSNR.MVecScaleY", 1.0f); }
            c.paramsRewritten = true;
            c.stage = StageModel;
            result = CallEvaluate(list, st.realHandle, c.params, c.callback);
            c.stage = StageParamsRestore;
            AsyncRestoreParams(c);
        }
        if (a.queryHeap) {
            list->EndQuery(a.queryHeap, D3D12_QUERY_TYPE_TIMESTAMP, slot * 2 + 1);
            list->ResolveQueryData(a.queryHeap, D3D12_QUERY_TYPE_TIMESTAMP, slot * 2, 2, a.queryReadback, slot * 16);
        }
        list->Close();
        Ctx().status.lastNgxResult = result;
        if (result != kNgxSuccess) {
            ++a.failures;
            if (a.type == D3D12_COMMAND_LIST_TYPE_COMPUTE && a.failures == 1) {
                a.wantDirect = true;
                Log(true, "Optimizer FPS NGX hook: background evaluate failed on the compute queue (%d); switching to a direct queue", result);
            } else if (a.failures <= 3) {
                Log(true, "Optimizer FPS NGX hook: background evaluate failed (%d)", result);
            }
        } else {
            a.jobId = nextJob;
            a.slotJob[slot] = nextJob;
            a.outIndex = next;
            a.kickCmd = cmd;
            cmd->SetPrivateData(kAsyncKickTag, sizeof(nextJob), &nextJob);
            a.signalPending = true;
            Ctx().anyAsyncSignalPending.store(true);
            a.inflight = true;
            a.sinceKick = 0;
            st.temporal->ResetPending();
            ID3D12CommandList *lists[] = {list};
            const HRESULT hrWait = a.queue->Wait(a.fInputs, nextJob);
            a.queue->ExecuteCommandLists(1, lists);
            const HRESULT hrSignal = a.queue->Signal(a.fModel, nextJob);
            if (AsyncVerbose()) Log(false, "Optimizer FPS NGX hook [async] kicked job %llu (slot %u, wait 0x%08lX signal 0x%08lX)", static_cast<unsigned long long>(nextJob), slot, (unsigned long) hrWait, (unsigned long) hrSignal);
        }
    }
    ++a.sinceKick;

    // 4. The frame the host gets: colour + reprojected residual; the plain colour until the first pass.
    //    Warped: the frame's colour through Pack -> Unpack (no model) is the base, as on the pass.
    pwtemporal::FrameInputs tinOut = c.tin;
    if (c.useBase && c.host && st.unpackBase) {
        EvalContext hc = *c.host;
        hc.cmd = cmd;
        hc.wantBase = true; hc.baseTarget = st.unpackBase; hc.baseTargetState = &st.unpackBaseState; hc.baseRtv = BaseRtv(st);
        c.stage = StagePackDraw;
        if (RecordPackStage(hc) == kNgxSuccess) {
            PackedGuidesToPixel(st, cmd, hc.slot, hc.packSlot);
            c.stage = StageUnpackDraw;
            if (RecordBaseUnpack(hc) == pw::AdapterStatus::Ok) { tinOut.base = st.unpackBase; tinOut.baseState = st.unpackBaseState; }
        }
    }
    const bool showPass = Ctx().diag.asyncShowPass; // DebugAsyncShowPass: the last pass's output as is (no reprojection)
    if (showPass && a.passes > 0 && a.outputDesc.Format == c.output->GetDesc().Format) {
        c.stage = StageCopyOut;
        ID3D12Resource *src = a.outputBg[a.inflight ? 1 - a.outIndex : a.outIndex];
        BarrierExternal(cmd, src, kHostOutputState, D3D12_RESOURCE_STATE_COPY_SOURCE);
        BarrierExternal(cmd, c.output, kHostOutputState, D3D12_RESOURCE_STATE_COPY_DEST);
        cmd->CopyResource(c.output, src);
        BarrierExternal(cmd, c.output, D3D12_RESOURCE_STATE_COPY_DEST, kHostOutputState);
        BarrierExternal(cmd, src, D3D12_RESOURCE_STATE_COPY_SOURCE, kHostOutputState);
    } else if (st.temporal->HasResidual()) {
        c.stage = StageTemporalReproject;
        st.temporal->RecordReproject(cmd, tinOut, KeepOutputOnInterpolation() ? nullptr : c.output, kHostOutputState,
                                     c.outputRect.x, c.outputRect.y);
    } else if (a.colorDesc.Format == a.outputDesc.Format) {
        c.stage = StageCopyOut;
        BarrierExternal(cmd, c.color, kHostInputState, D3D12_RESOURCE_STATE_COPY_SOURCE);
        BarrierExternal(cmd, c.output, kHostOutputState, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource = c.output; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION src{}; src.pResource = c.color; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        const D3D12_BOX box{c.tin.colorRect.x, c.tin.colorRect.y, 0, c.tin.colorRect.x + st.nativeWidth, c.tin.colorRect.y + st.nativeHeight, 1};
        cmd->CopyTextureRegion(&dst, c.outputRect.x, c.outputRect.y, 0, &src, &box);
        BarrierExternal(cmd, c.output, D3D12_RESOURCE_STATE_COPY_DEST, kHostOutputState);
        BarrierExternal(cmd, c.color, D3D12_RESOURCE_STATE_COPY_SOURCE, kHostInputState);
    } else {
        // No residual yet and the formats differ: the colour through the machine's raw path.
        FallbackOutput(st, cmd, c.color, c.colorRect, c.output, c.outputRect, &tinOut, "background mode: no residual yet");
    }
    ++a.age;
    Ctx().status.asyncPasses = a.passes; Ctx().status.asyncForcedWaits = a.forcedWaits; Ctx().status.asyncStalls = a.stalls;
    Ctx().status.asyncPassesPerSecond = a.passesPerSecond; Ctx().status.asyncModelMs = a.lastModelMs; Ctx().status.asyncAge = a.age;
    Ctx().status.asyncQueueWaits = a.queueWaits; Ctx().status.asyncQueueWaitMs = a.queueWaitMs;
    c.stage = StageDone;
    c.result = kNgxSuccess;
    return kNgxSuccess;
}

int AsyncGuarded(AsyncCtx &c)
{
    __try {
        return AsyncBody(c);
    } __except (RecordCrash(GetExceptionInformation(), c.stage)) {
        return kCrashed;
    }
}

int AsyncTemporalEvaluate(FeatureState &st, ID3D12GraphicsCommandList *cmd, void *params, void *callback, const EvalContext *host)
{
    if (st.warped && host == nullptr) return kNotHandled; // the warped path needs the host frame's context
    ID3D12Resource *color = GetResource(params, "DLSSNR.Color");
    ID3D12Resource *depth = GetResource(params, "DLSSNR.Depth");
    ID3D12Resource *motion = GetResource(params, "DLSSNR.MVec");
    ID3D12Resource *output = GetResource(params, "DLSSNR.Output");
    if (color == nullptr || depth == nullptr || motion == nullptr || output == nullptr || cmd->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT)
        return kNotHandled;
    const D3D12_RESOURCE_DESC colorDesc = color->GetDesc();
    const D3D12_RESOURCE_DESC depthDesc = depth->GetDesc();
    const D3D12_RESOURCE_DESC motionDesc = motion->GetDesc();
    const D3D12_RESOURCE_DESC outputDesc = output->GetDesc();
    if (colorDesc.Width < st.nativeWidth || colorDesc.Height < st.nativeHeight || outputDesc.Width < st.nativeWidth || outputDesc.Height < st.nativeHeight) {
        TemporalReason(st, "host colour/output smaller than the feature");
        return kNotHandled;
    }
    if (!EnsureTemporal(st, cmd, output, motion, depth)) return kNotHandled;
    if (!EnsureAsync(st, colorDesc, depthDesc, motionDesc, outputDesc)) return kNotHandled;
    Ctx().status.temporalMode = 3;
    Ctx().status.temporalReason[0] = 0;

    AsyncCtx c{};
    c.st = &st; c.cmd = cmd; c.params = params; c.callback = callback;
    c.host = host;
    c.useBase = st.warped && Ctx().temporal.warpBase && st.unpackBase != nullptr;
    c.color = color; c.depth = depth; c.motion = motion; c.output = output;
    c.depthState = HostDepthState(depth);
    c.depthSub = pwngx::DepthBarrierSubresource(depth);
    c.ui = GetResource(params, "DLSSNR.UI"); c.uiAlpha = GetResource(params, "DLSSNR.UIAlpha"); c.backbuffer = GetResource(params, "DLSSNR.Backbuffer");
    const Subrect colorRect = ReadSubrect(params, "Color", (unsigned) colorDesc.Width, colorDesc.Height);
    const Subrect depthRect = ReadSubrect(params, "Depth", (unsigned) depthDesc.Width, depthDesc.Height);
    const Subrect motionRect = ReadSubrect(params, "MVec", (unsigned) motionDesc.Width, motionDesc.Height);
    c.outputRect = ReadSubrect(params, "Output", (unsigned) outputDesc.Width, outputDesc.Height);
    c.colorRect = colorRect; c.depthRect = depthRect; c.motionRect = motionRect;
    unsigned int depthInverted = 0;
    GetUInt(params, "DLSSNR.DepthInverted", &depthInverted);
    c.depthInverted = depthInverted != 0;
    unsigned int hostReset = 0;
    GetUInt(params, "DLSSNR.Reset", &hostReset);
    c.hostReset = hostReset != 0;
    float mvScaleX = 1.0f, mvScaleY = 1.0f;
    if (!GetFloat(params, "DLSSNR.MVecScaleX", &mvScaleX) || !std::isfinite(mvScaleX) || mvScaleX == 0.0f) mvScaleX = 1.0f;
    if (!GetFloat(params, "DLSSNR.MVecScaleY", &mvScaleY) || !std::isfinite(mvScaleY) || mvScaleY == 0.0f) mvScaleY = 1.0f;
    c.mvScaleX = mvScaleX; c.mvScaleY = mvScaleY;
    c.tin = TemporalInputs(color, motion, depth, TypedView(colorDesc.Format, false), TypedView(motionDesc.Format, false),
                           TypedView(depthDesc.Format, true), colorRect, motionRect, depthRect, mvScaleX, mvScaleY, depthInverted != 0);
    c.tin.depthState = c.depthState;
    c.tin.depthSubresource = c.depthSub;
    const int result = AsyncGuarded(c);
    if (result == kCrashed) {
        AsyncRestoreParams(c);
        st.temporalDisabled = true;
        TemporalReason(st, "exception 0x%08lX in stage %s; temporal mode disabled for this feature", Ctx().crash.code, StageName(Ctx().crash.stage));
        Log(true, "Optimizer FPS NGX hook: exception 0x%08lX at %p (%s) during stage %d (%s); background mode disabled",
            Ctx().crash.code, Ctx().crash.address, Ctx().crash.module, Ctx().crash.stage, StageName(Ctx().crash.stage));
        return kNgxSuccess;
    }
    if (result == kNgxSuccess) {
        ++Ctx().status.evaluations;
        Ctx().status.active = true;
        SetReason(st.warped ? "temporal mode 3 (model in the background, warped)" : "temporal mode 3 (model in the background, no warp)");
        ++Ctx().status.interpFrames;
        if (st.temporal->HasResidual()) TemporalDebugReadback(st, cmd, c.tin);
    }
    return result;
}

void OnCommandListExecuted(ID3D12CommandQueue *queue, ID3D12GraphicsCommandList *list)
{
    if (queue == nullptr || list == nullptr) return;
    // 26.7.4: nothing to do (and nothing touched on the host's list) unless a background pass waits for its submit.
    if (!Ctx().anyAsyncSignalPending.load()) return;
    std::lock_guard<std::mutex> lock(Ctx().mutex);
    bool anyPending = false;
    for (auto &entry : Ctx().features) if (entry.second->async && entry.second->async->signalPending) anyPending = true;
    if (!anyPending) { Ctx().anyAsyncSignalPending.store(false); return; }
    UINT64 tag = 0;
    UINT size = sizeof(tag);
    const bool tagged = SUCCEEDED(list->GetPrivateData(kAsyncKickTag, &size, &tag)) && size == sizeof(tag);
    for (auto &entry : Ctx().features) {
        FeatureState &st = *entry.second;
        if (!st.async || !st.async->signalPending) continue;
        if (st.async->kickCmd != list && !(tagged && tag == st.async->jobId)) continue;
        st.async->submitQueue = queue;
        st.async->submitSeen = true;
        if (AsyncVerbose()) Log(false, "Optimizer FPS NGX hook [async] host submits the kick list of job %llu (%s match); the signal follows at the next evaluate", static_cast<unsigned long long>(st.async->jobId), tagged ? "tag" : "pointer");
    }
}

} // namespace pwhook
