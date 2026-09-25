#include "core/frame/model_passes.h"
#include "core/temporal/async_scheduler.h"

#include "core/frame/debug_readback.h"
#include "core/frame/feature_state.h"
#include "core/context.h"
#include "core/frame/host_depth_state.h"
#include "core/temporal/controller.h"
#include "core/frame/timing.h"
#include "core/frame/warp_recorder.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>

#include "core/temporal/async_frame.h"

namespace ofps::core {
bool CreateReadbackBuffer(ID3D12Device *device, UINT64 size, ID3D12Resource **out)
{
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC d{}; d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; d.Width = size; d.Height = 1; d.DepthOrArraySize = 1; d.MipLevels = 1;
    d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(out)));
}

bool EnsureAsync(FeatureState &st, const D3D12_RESOURCE_DESC &colorDesc, const D3D12_RESOURCE_DESC &depthDesc,
                 const D3D12_RESOURCE_DESC &motionDesc, const D3D12_RESOURCE_DESC &outputDesc,
                 const OfpsModelInputs &inputs, const OfpsFrameInputs &frame)
{
    ID3D12Resource *extras[7] = {inputs.ui.res, inputs.uiAlpha.res, inputs.backbuffer.res};
    for (unsigned i = 0; i < 4; ++i) extras[i + 3] = frame.codecInputs[i].res;
    bool extrasMatch = st.async != nullptr;
    if (st.async) for (unsigned i = 0; i < 7; ++i) {
        if (!extras[i] || !st.async->extraBg[i]) { extrasMatch &= extras[i] == st.async->extraBg[i]; continue; }
        const auto x = extras[i]->GetDesc(), y = st.async->extraBg[i]->GetDesc();
        extrasMatch &= x.Dimension == y.Dimension && x.Width == y.Width && x.Height == y.Height &&
            x.DepthOrArraySize == y.DepthOrArraySize && x.MipLevels == y.MipLevels && x.Format == y.Format &&
            x.SampleDesc.Count == y.SampleDesc.Count && x.SampleDesc.Quality == y.SampleDesc.Quality;
    }
    if (st.async && extrasMatch && st.async->Matches(colorDesc, depthDesc, motionDesc, outputDesc) && !st.async->wantDirect) return true;
    // Direct by default: on the bench a compute queue was starved by the saturated host queue (the pass
    // only finished when the host stalled); DebugAsyncCompute=1 tries the compute queue instead.
    const bool direct = (st.async && st.async->wantDirect) || !Ctx().diag.asyncCompute ||
        st.warpPath == warp::PackPath::Pixel;
    if (st.async) {
        ofps::core::gpu::GateSet gate;
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
    ofps::core::gpu::SetQueueRegistrationSuppressed(true); // the proxy announces the new queue through ReShade; it is not a host queue
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
    ofps::core::gpu::SetQueueRegistrationSuppressed(false);
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
    if (!ofps::core::gpu::CreateTexture(a.device, (UINT) colorDesc.Width, colorDesc.Height, colorDesc.Format, D3D12_RESOURCE_FLAG_NONE, &a.colorBg) ||
        !ofps::core::gpu::CreateTexture(a.device, (UINT) depthDesc.Width, depthDesc.Height, depthDesc.Format, D3D12_RESOURCE_FLAG_NONE, &a.depthBg) ||
        !ofps::core::gpu::CreateTexture(a.device, (UINT) motionDesc.Width, motionDesc.Height, motionDesc.Format, D3D12_RESOURCE_FLAG_NONE, &a.mvBg) ||
        !ofps::core::gpu::CreateTexture(a.device, (UINT) motionDesc.Width, motionDesc.Height, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_NONE, &a.accBg) ||
        !ofps::core::gpu::CreateTexture(a.device, (UINT) motionDesc.Width, motionDesc.Height, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_NONE, &a.modelAccBg) ||
        !ofps::core::gpu::CreateTexture(a.device, (UINT) outputDesc.Width, outputDesc.Height, outputDesc.Format, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, &a.outputBg[0]) ||
        !ofps::core::gpu::CreateTexture(a.device, (UINT) outputDesc.Width, outputDesc.Height, outputDesc.Format, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, &a.outputBg[1]))
        return fail("private copies could not be allocated");
    for (unsigned i = 0; i < 7; ++i) {
        if (!extras[i]) continue;
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc = extras[i]->GetDesc();
        desc.Flags = D3D12_RESOURCE_FLAG_NONE;
        if (FAILED(a.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&a.extraBg[i]))))
            return fail("private copies could not be allocated");
    }
    if (st.warped && st.outputView != DXGI_FORMAT_UNKNOWN) {
        const DXGI_FORMAT view = st.outputView;
        const auto flags = st.warpPath == warp::PackPath::Compute ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS :
            D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        if (!ofps::core::gpu::CreateTexture(a.device, st.nativeWidth, st.nativeHeight, view, flags, &a.baseBg[0]) ||
            !ofps::core::gpu::CreateTexture(a.device, st.nativeWidth, st.nativeHeight, view, flags, &a.baseBg[1]))
            return fail("base targets could not be allocated");
        if (st.warpPath == warp::PackPath::Pixel) {
            D3D12_DESCRIPTOR_HEAP_DESC hd{}; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = 2;
            if (FAILED(listDevice->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&a.baseRtvHeap)))) return fail("base RTV heap creation failed");
            a.rtvIncrement = listDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
            D3D12_RENDER_TARGET_VIEW_DESC rd{}; rd.Format = view; rd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
            listDevice->CreateRenderTargetView(a.baseBg[0], &rd, a.BaseRtv(0));
            listDevice->CreateRenderTargetView(a.baseBg[1], &rd, a.BaseRtv(1));
        }
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

int AsyncTemporalEvaluate(FeatureState &st, ID3D12GraphicsCommandList *cmd, const OfpsModelInputs &inputs, const EvalContext *host, const OfpsFrameInputs &frame)
{
    if (st.warped && host == nullptr) return kNotHandled; // the warped path needs the host frame's context
    ID3D12Resource *color = inputs.color.res;
    ID3D12Resource *depth = inputs.depth.res;
    ID3D12Resource *motion = inputs.motion.res;
    ID3D12Resource *output = inputs.output.res;
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
    if (!EnsureAsync(st, colorDesc, depthDesc, motionDesc, outputDesc, inputs, frame)) return kNotHandled;
    Ctx().status.temporalMode = 3;
    Ctx().status.temporalReason[0] = 0;

    AsyncCtx c{};
    c.st = &st; c.cmd = cmd; c.inputs = inputs;
    c.frame = frame;
    c.host = host;
    c.useBase = st.warped && st.async->codecIdentity && Ctx().temporal.warpBase && st.unpackBase != nullptr;
    c.color = color; c.depth = depth; c.motion = motion; c.output = output;
    c.depthState = HostDepthState(inputs.depth);
    c.depthSub = inputs.depth.subresource;
    const OfpsRect colorRect = OfpsRect{inputs.color.rect.x, inputs.color.rect.y, inputs.color.rect.w, inputs.color.rect.h};
    const OfpsRect depthRect = OfpsRect{inputs.depth.rect.x, inputs.depth.rect.y, inputs.depth.rect.w, inputs.depth.rect.h};
    const OfpsRect motionRect = OfpsRect{inputs.motion.rect.x, inputs.motion.rect.y, inputs.motion.rect.w, inputs.motion.rect.h};
    c.outputRect = OfpsRect{inputs.output.rect.x, inputs.output.rect.y, inputs.output.rect.w, inputs.output.rect.h};
    c.colorRect = colorRect; c.depthRect = depthRect; c.motionRect = motionRect;
    const unsigned int depthInverted = inputs.depthInverted;
    c.depthInverted = depthInverted != 0;
    const unsigned int hostReset = inputs.reset;
    c.hostReset = hostReset != 0;
    float mvScaleX = inputs.mvScaleX, mvScaleY = inputs.mvScaleY;
    if (!std::isfinite(mvScaleX) || mvScaleX == 0.0f) mvScaleX = 1.0f;
    if (!std::isfinite(mvScaleY) || mvScaleY == 0.0f) mvScaleY = 1.0f;
    c.mvScaleX = mvScaleX; c.mvScaleY = mvScaleY;
    c.tin = TemporalInputs(inputs.color, inputs.motion, inputs.depth, mvScaleX, mvScaleY, depthInverted != 0, st.modelResolution);
    c.tin.depthState = c.depthState;
    c.tin.depthSubresource = c.depthSub;
    const int result = AsyncGuarded(c);
    if (result == kCrashed) {
        st.temporalDisabled = true;
        TemporalReason(st, "exception 0x%08lX in stage %s; temporal mode disabled for this feature", Ctx().crash.code, StageName(Ctx().crash.stage));
        Log(true, "Optimizer FPS NGX hook: exception 0x%08lX at %p (%s) during stage %d (%s); background mode disabled",
            Ctx().crash.code, Ctx().crash.address, Ctx().crash.module, Ctx().crash.stage, StageName(Ctx().crash.stage));
        return OFPS_OK;
    }
    if (result == OFPS_OK) {
        ++Ctx().status.evaluations;
        Ctx().status.active = true;
        SetReason(st.warped ? "temporal mode 3 (model in the background, warped)" : "temporal mode 3 (model in the background, no warp)");
        ++Ctx().status.interpFrames;
        if (st.temporal->HasResidual()) TemporalDebugReadback(st, cmd, c.tin);
    }
    return result;
}

void AsyncNoteSubmissions(const gpu::SubmissionEntry *entries, std::uint32_t count)
{
    // Called under the core mutex, after submission callbacks have returned.
    if (!Ctx().anyAsyncSignalPending.load()) return;
    bool anyPending = false;
    for (auto &entry : Ctx().features) {
        FeatureState &st = *entry.second;
        if (!st.async || !st.async->signalPending) continue;
        anyPending = true;
        for (std::uint32_t i = 0; i < count; ++i) {
            // Tags were captured by Push while the list was alive; reject reused addresses.
            if (entries[i].list != st.async->kickCmd || entries[i].asyncTag != st.async->jobId) continue;
            auto *queue = gpu::RetainRegisteredQueue(entries[i].queue);
            if (!queue) { st.async->warpFailed = true; continue; }
            if (st.async->submitQueue) st.async->submitQueue->Release();
            st.async->submitQueue = queue;
            st.async->submitSeen = true;
            if (AsyncVerbose()) Log(false, "Optimizer FPS NGX hook [async] host submits the kick list of job %llu (%s match); the signal follows at the next evaluate", static_cast<unsigned long long>(st.async->jobId), "tag");
        }
    }
    Ctx().anyAsyncSignalPending.store(anyPending);
}

} // namespace ofps::core
