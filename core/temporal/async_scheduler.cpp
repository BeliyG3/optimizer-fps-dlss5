#include "core/frame/model_passes.h"
#include "core/frame/frame_inputs.h"
#include "core/frame/codec_frame.h"
#include "core/frame/model_grid.h"
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
// Copies an input on the host list, restoring the host state and leaving its twin shader-readable.
void AsyncCopyInput(ID3D12GraphicsCommandList *cmd, ID3D12Resource *host, ID3D12Resource *bg, D3D12_RESOURCE_STATES &bgState,
                    D3D12_RESOURCE_STATES hostState, UINT hostSub)
{
    BarrierExternal(cmd, host, hostState, D3D12_RESOURCE_STATE_COPY_SOURCE, hostSub);
    Barrier(cmd, bg, bgState, D3D12_RESOURCE_STATE_COPY_DEST);
    if (hostSub != D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES) {
        D3D12_TEXTURE_COPY_LOCATION dstP{}, srcP{};
        dstP.pResource = bg; dstP.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dstP.SubresourceIndex = 0;
        srcP.pResource = host; srcP.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; srcP.SubresourceIndex = hostSub;
        cmd->CopyTextureRegion(&dstP, 0, 0, 0, &srcP, nullptr);
    } else {
        cmd->CopyResource(bg, host);
    }
    Barrier(cmd, bg, bgState, kModelInputState);
    BarrierExternal(cmd, host, D3D12_RESOURCE_STATE_COPY_SOURCE, hostState, hostSub);
}

void AsyncSignalPending(FeatureState &st)
{
    if (!st.async) return;
    AsyncJob &a = *st.async;
    // The submission event fires before the host's ExecuteCommandLists, so the signal is issued here,
    // one evaluate later, when the tagged list is certainly queued ahead of it. Without the event the
    // fallback signals every registered queue of the device.
    if (a.signalPending) {
        if (a.submitSeen && a.submitQueue) {
            a.submitQueue->Signal(a.fInputs, a.jobId);
        } else {
            ofps::core::gpu::SignalRegisteredQueues(st.realDevice, st.device, a.fInputs, a.jobId);
            if (!a.fallbackLogged) { a.fallbackLogged = true; Log(false, "Optimizer FPS NGX hook: background mode: host submission not observed, signalling the registered queues one frame later"); }
        }
        a.signalPending = false;
        a.submitSeen = false;
    }
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
    c.tin.background = true;
    const int every = std::clamp(Ctx().temporal.every, 1, 8);
    const int phaseIn = Ctx().diag.temporalBackgroundPhaseIn;
    c.tin.phaseInFrames = static_cast<std::uint32_t>(phaseIn >= 0 ? phaseIn : std::min(every - 1, 3));
    const std::uint32_t maxAge = static_cast<std::uint32_t>(std::clamp(Ctx().temporal.maxAge, 3, 16));
    D3D12_RESOURCE_STATES bgInputState = kModelInputState; // private copies rest in the host's input state

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
        if (prev > a.hostSignalled && ofps::core::gpu::SignalRegisteredQueues(st.realDevice, st.device, a.fHost, prev)) a.hostSignalled = prev;
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

    AsyncSignalPending(st);
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
    st.temporal->RecordBackgroundAccumulate(cmd, c.tin, a.jobId > 0 && !a.discard && (a.inflight || st.temporal->HasResidual()));

    // 2. Adopt the finished pass (or force the host queue to wait for it when the residual is too old).
    bool ready = a.inflight && a.fModel->GetCompletedValue() >= a.jobId;
    if (!ready && a.inflight && a.age >= maxAge) {
        if (a.signalPending) { // never let the host queue wait for a pass that still waits for the host's signal
            if (a.submitSeen && a.submitQueue) a.submitQueue->Signal(a.fInputs, a.jobId);
            else ofps::core::gpu::SignalRegisteredQueues(st.realDevice, st.device, a.fInputs, a.jobId);
            a.signalPending = false; a.submitSeen = false;
        }
        ofps::core::gpu::WaitRegisteredQueues(st.realDevice, st.device, a.fModel, a.jobId);
        ++a.forcedWaits;
        ready = true;
    }
    bool adoptedThisFrame = false;
    if (ready) {
        adoptedThisFrame = true;
        if (!a.discard) {
            c.stage = StageTemporalResidual;
            ofps::core::temporal::FrameInputs bg = c.tin;
            bg.color = a.colorBg; bg.depth = a.depthBg; bg.motion = a.mvIsAcc ? a.accBg : a.mvBg;
            // The kick owns these copies; the adoption frame may advertise different views or planes.
            bg.colorView = a.frameSnapshot.color.view;
            bg.depthView = a.frameSnapshot.depth.view;
            bg.motionView = a.frameSnapshot.motion.view;
            bg.hostInputState = bgInputState;
            bg.motionState = bgInputState;
            bg.colorSubresource = bg.motionSubresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            bg.depthSubresource = gpu::DepthBarrierSubresource(a.depthBg);
            bg.depthState = bgInputState;
            if (c.useBase && a.baseBg[a.outIndex]) { bg.base = a.baseBg[a.outIndex]; bg.baseState = a.baseState[a.outIndex]; }
            if (a.codecBefore[a.outIndex].res) {
                const auto &before = a.codecBefore[a.outIndex];
                bg.base = nullptr;
                bg.color = before.res;
                bg.colorView = before.view;
                bg.colorRect = {before.rect.x, before.rect.y, before.rect.w, before.rect.h};
                bg.hostInputState = before.restState;
                bg.colorSubresource = before.subresource;
            }
            bg.residualBlend = a.mvIsAcc ? kResidualBlend : 0.0f; // mvBg = displacement to the previous pass's frame
            bg.blendFromMotion = true;
            bg.motionIsPassChain = a.mvIsAcc;
            if (a.mvIsAcc) {
                // mvBg holds the displacement from this pass's frame back to the previous pass's frame (scale 1).
                bg.mvScaleX = bg.mvScaleY = 1.0f; // the chain is already scaled and signed
                bg.motionSign = 1.0f;
                bg.motionView = DXGI_FORMAT_R16G16_FLOAT;
            }
            st.temporal->RecordResidual(cmd, bg, a.outputBg[a.outIndex], kModelOutputState);
            st.temporal->RecordFlowCapture(cmd, bg, st.realDevice, true, true);
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
        if (st.temporal->PendingMirrorsAcc() && st.temporal->AccValid() && !Ctx().temporal.debugSingleFrameMotion) {
            // Adopted since the last kick: the main chain is the displacement to the last pass's frame.
            st.temporal->RecordCopyAcc(cmd, false, a.accBg, bgInputState);
            a.mvIsAcc = true;
        } else if (st.temporal->PendingValid() && !Ctx().temporal.debugSingleFrameMotion) {
            st.temporal->RecordCopyAcc(cmd, true, a.accBg, bgInputState);
            a.mvIsAcc = true;
        } else {
            AsyncCopyInput(cmd, c.motion, a.mvBg, s, c.inputs.motion.restState, c.inputs.motion.subresource);
            a.mvIsAcc = false;
        }
        Barrier(cmd, a.modelAccBg, a.modelAccState, bgInputState);
        st.temporal->RecordBackgroundKick(cmd, c.tin, !st.temporal->PendingMirrorsAcc(),
                                          a.mvIsAcc ? a.modelAccBg : nullptr, bgInputState,
                                          a.colorBg, a.depthBg, !Ctx().diag.temporalNoBackgroundModelMotion);
        // Validation still needs the previous kick's guides. Replace them only after the host pass.
        AsyncCopyInput(cmd, c.color, a.colorBg, s, c.inputs.color.restState, c.inputs.color.subresource);
        AsyncCopyInput(cmd, c.depth, a.depthBg, s, c.depthState, c.depthSub);
        const OfpsResource *extras[7] = {&c.inputs.ui, &c.inputs.uiAlpha, &c.inputs.backbuffer,
            &c.frame.codecInputs[0], &c.frame.codecInputs[1], &c.frame.codecInputs[2], &c.frame.codecInputs[3]};
        for (unsigned i = 0; i < 7; ++i) if (extras[i]->res)
            AsyncCopyInput(cmd, extras[i]->res, a.extraBg[i], a.extraState[i], extras[i]->restState, extras[i]->subresource);
        a.frameSnapshot = c.frame;
        for (unsigned i = 0; i < 4; ++i) {
            a.frameSnapshot.codecInputs[i].res = a.extraBg[i + 3];
            a.frameSnapshot.codecInputs[i].restState = bgInputState;
            a.frameSnapshot.codecInputs[i].subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        }
        const int next = 1 - a.outIndex;
        ID3D12CommandAllocator *alloc = a.allocators[slot];
        ID3D12GraphicsCommandList *list = a.lists[slot];
        const HRESULT hrAlloc = alloc->Reset();
        const HRESULT hrList = SUCCEEDED(hrAlloc) ? list->Reset(alloc, nullptr) : E_FAIL;
        if (FAILED(hrAlloc) || FAILED(hrList)) {
            ++a.failures;
            if (a.failures <= 3) Log(true, "Optimizer FPS NGX hook: background list reset failed (allocator 0x%08lX, list 0x%08lX)", (unsigned long) hrAlloc, (unsigned long) hrList);
            FallbackOutput(st, cmd, c.frame.color, c.frame.output, &c.tin, "background list reset failed");
            return OFPS_OK;
        }
        a.recording = true;
    st.temporal->SetUsePoint({sizeof(OfpsFencePoint), a.fModel, nextJob}, Ctx().evalCounter, Ctx().diag.timing);
        Barrier(list, a.outputBg[next], a.outputState[next], kModelOutputState);
        OfpsModelInputs privateInputs = c.inputs;
        privateInputs.color.res = a.colorBg;
        privateInputs.depth.res = a.depthBg;
        privateInputs.motion.res = a.mvIsAcc ? a.modelAccBg : a.mvBg;
        privateInputs.output.res = a.outputBg[next];
        privateInputs.color.restState = privateInputs.depth.restState = privateInputs.motion.restState = bgInputState;
        privateInputs.color.subresource = privateInputs.depth.subresource =
            privateInputs.motion.subresource = privateInputs.output.subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        privateInputs.output.restState = kModelOutputState;
        if (a.mvIsAcc) {
            privateInputs.motion.view = DXGI_FORMAT_R16G16_FLOAT;
            privateInputs.mvScaleX = privateInputs.mvScaleY = 1.0f;
        }
        OfpsResource *privateExtras[3] = {&privateInputs.ui, &privateInputs.uiAlpha, &privateInputs.backbuffer};
        for (unsigned i = 0; i < 3; ++i) {
            privateExtras[i]->res = a.extraBg[i];
            privateExtras[i]->restState = bgInputState;
            privateExtras[i]->subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        }
        a.frameSnapshot.size = sizeof(a.frameSnapshot);
        a.frameSnapshot.mvScaleX = privateInputs.mvScaleX;
        a.frameSnapshot.mvScaleY = privateInputs.mvScaleY;
        a.frameSnapshot.depthInverted = privateInputs.depthInverted;
        a.frameSnapshot.hostReset = privateInputs.reset;
        a.frameSnapshot.color = privateInputs.color;
        a.frameSnapshot.depth = privateInputs.depth;
        a.frameSnapshot.motion = privateInputs.motion;
        a.frameSnapshot.output = privateInputs.output;
        a.frameSnapshot.ui = privateInputs.ui;
        a.frameSnapshot.uiAlpha = privateInputs.uiAlpha;
        a.frameSnapshot.backbuffer = privateInputs.backbuffer;
        int result = OFPS_OK;
        if (a.queryHeap) list->EndQuery(a.queryHeap, D3D12_QUERY_TYPE_TIMESTAMP, slot * 2);
        CodecFrame codec{};
        result = BeginCodecFrame(st, list, a.frameSnapshot, codec);
        if (result == OFPS_OK) result = ConfigureModelGrid(st, list, codec);
        if (result == OFPS_S_MODEL_NEXT_FRAME) st.frameCreation = true;
        a.codecIdentity = codec.identity;
        if (!codec.identity) c.useBase = false;
        a.codecBefore[next] = {};
        if (result == OFPS_OK && codec.frameBefore.res)
            result = SnapshotAsyncCodecBase(st, list, codec.frameBefore, next);
        privateInputs.color = codec.modelColor;
        privateInputs.output = codec.answer;
        if (!codec.identity) {
            privateInputs.width = codec.modelColor.rect.w;
            privateInputs.height = codec.modelColor.rect.h;
        }
        if (result == OFPS_OK && st.warped && (st.adapter || st.compute)) {
            // Warped: Pack -> model -> Unpack on the background list, all from the private copies; the
            // reserved pack slot is written once per motion source (host vectors / accumulated displacement).
            EvalContext bg{};
            if (st.warpPath == warp::PackPath::Compute || !codec.identity || !a.bgPackValid || a.bgPackIsAcc != a.mvIsAcc ||
                a.bgPackColorView != privateInputs.color.view || a.bgPackDepthView != privateInputs.depth.view ||
                a.bgPackMotionView != privateInputs.motion.view) {
                ofps::sdk::InputDescriptionV2 input = ofps::sdk::DefaultInputDescriptionV2(st.layout.nativeWidth, st.layout.nativeHeight);
                input.colorRect = {codec.modelColor.rect.x, codec.modelColor.rect.y, codec.modelColor.rect.w, codec.modelColor.rect.h};
                input.depthRect = {c.depthRect.x, c.depthRect.y, c.depthRect.w, c.depthRect.h};
                input.motionRect = {c.motionRect.x, c.motionRect.y, c.motionRect.w, c.motionRect.h};
                input.confidenceRect = {0, 0, 1, 1};
                const float adjust = Ctx().motionScaleAdjust * (Ctx().motionInvert ? -1.0f : 1.0f);
                input.motionScaleX = adjust * (a.mvIsAcc ? 1.0f : c.mvScaleX) * static_cast<float>(st.layout.nativeWidth) / static_cast<float>(std::max(1u, c.motionRect.w));
                input.motionScaleY = adjust * (a.mvIsAcc ? 1.0f : c.mvScaleY) * static_cast<float>(st.layout.nativeHeight) / static_cast<float>(std::max(1u, c.motionRect.h));
                input.motionDirection = ofps::sdk::MotionDirection::CurrentToPrevious;
                input.depthConvention = c.depthInverted ? ofps::sdk::DepthConvention::Reversed : ofps::sdk::DepthConvention::Normal;
                input.colorEncoding = EncodingFor(codec.modelColor.view);
                const ofps::sdk::D3D12SourceResources sources = {{codec.modelColor.res, codec.modelColor.view},
                                                           {a.depthBg, privateInputs.depth.view},
                                                           {privateInputs.motion.res, privateInputs.motion.view},
                                                           {nullptr, DXGI_FORMAT_UNKNOWN}};
                bg.packInput = input; bg.packSources = sources;
                st.packKeys[FeatureState::kBgPackSlot] = FeatureState::SlotKey{};
                st.unpackValid[FeatureState::kBgPackSlot] = false;
                const ofps::sdk::AdapterStatus ds = st.warpPath == warp::PackPath::Pixel ?
                    st.adapter->WriteSourceDescriptorsV2(FeatureState::kBgPackSlot, sources, input) : ofps::sdk::AdapterStatus::Ok;
                if (ds != ofps::sdk::AdapterStatus::Ok) {
                    a.warpFailed = true;
                    TemporalReason(st, "background mode: pack descriptors for the private copies: %s", ofps::sdk::AdapterStatusString(ds));
                    Log(true, "Optimizer FPS NGX hook: %s", Ctx().status.temporalReason);
                    list->Close();
                    a.recording = false;
    st.temporal->SetUsePoint(HostUsePoint(cmd), Ctx().evalCounter, Ctx().diag.timing);
                    FallbackOutput(st, cmd, c.frame.color, c.frame.output, &c.tin, Ctx().status.temporalReason);
                    return OFPS_OK;
                }
                a.bgPackValid = true;
                a.bgPackIsAcc = a.mvIsAcc;
                a.bgPackColorView = privateInputs.color.view;
                a.bgPackDepthView = privateInputs.depth.view;
                a.bgPackMotionView = privateInputs.motion.view;
            }
            bg.st = &st; bg.cmd = list; bg.modelInputs = privateInputs; bg.codec = &codec; bg.frame = &a.frameSnapshot;
            bg.colorResource = codec.modelColor; bg.motionResource = privateInputs.motion; bg.outputResource = codec.answer;
            bg.color = codec.modelColor.res; bg.depth = a.depthBg; bg.motion = a.mvIsAcc ? a.modelAccBg : a.mvBg; bg.output = codec.answer.res;
            bg.ui = privateInputs.ui.res; bg.uiAlpha = privateInputs.uiAlpha.res; bg.backbuffer = privateInputs.backbuffer.res;
            bg.colorRect = {codec.modelColor.rect.x, codec.modelColor.rect.y, codec.modelColor.rect.w, codec.modelColor.rect.h}; bg.depthRect = c.depthRect; bg.motionRect = c.motionRect; bg.outputRect = {codec.answer.rect.x, codec.answer.rect.y, codec.answer.rect.w, codec.answer.rect.h};
            bg.mvScaleX = a.mvIsAcc ? 1.0f : c.mvScaleX; bg.mvScaleY = a.mvIsAcc ? 1.0f : c.mvScaleY;
            bg.slot = bg.packSlot = bg.packSet = FeatureState::kBgPackSlot;
            bg.privateUsePoint = {sizeof(OfpsFencePoint), a.fModel, nextJob};
            bg.unpackSlot = FeatureState::kPackSlots + FeatureState::kBgPackSlot;
            bg.depthState = bgInputState;
            bg.depthSub = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            bg.depthConvention = c.depthInverted ? ofps::sdk::DepthConvention::Reversed : ofps::sdk::DepthConvention::Normal;
            bg.motionIsAcc = false; // the private copy rests in the input state and takes the same barriers as the host's vectors
            if (c.useBase && a.baseBg[next]) { bg.wantBase = true; bg.baseTarget = a.baseBg[next]; bg.baseTargetState = &a.baseState[next];
                if (st.warpPath == warp::PackPath::Pixel) bg.baseRtv = a.BaseRtv(next); }
            c.stage = StageModel;
            result = WarpedBody(bg);
            if (result == kPackFailed) {
                a.warpFailed = true;
                Log(true, "Optimizer FPS NGX hook: background mode: %s; no more background passes", Ctx().status.reason);
            }
        } else if (result == OFPS_OK) {
            c.stage = StageModel;
            result = EvaluateModelPasses(st, list, privateInputs);
            if (result == OFPS_OK) result = ResolveCodecFrame(st, list, codec);
        }
        if (a.queryHeap) {
            list->EndQuery(a.queryHeap, D3D12_QUERY_TYPE_TIMESTAMP, slot * 2 + 1);
            list->ResolveQueryData(a.queryHeap, D3D12_QUERY_TYPE_TIMESTAMP, slot * 2, 2, a.queryReadback, slot * 16);
        }
        list->Close();
        a.recording = false;
    st.temporal->SetUsePoint(HostUsePoint(cmd), Ctx().evalCounter, Ctx().diag.timing);
        Ctx().status.lastNgxResult = result;
        if (result != OFPS_OK && result != OFPS_S_MODEL_NEXT_FRAME) {
            ++a.failures;
            if (a.type == D3D12_COMMAND_LIST_TYPE_COMPUTE && a.failures == 1) {
                a.wantDirect = true;
                Log(true, "Optimizer FPS NGX hook: background evaluate failed on the compute queue (%d); switching to a direct queue", result);
            } else if (a.failures <= 3) {
                Log(true, "Optimizer FPS NGX hook: background evaluate failed (%d)", result);
            }
        } else {
            a.discard = result == OFPS_S_MODEL_NEXT_FRAME;
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

    if (st.frameCreation) {
        FallbackOutput(st, cmd, c.frame.color, c.frame.output, &c.tin, "model creation frame");
        return OFPS_OK;
    }

    // 4. The frame the host gets: colour + reprojected residual; the plain colour until the first pass.
    //    Warped: the frame's colour through Pack -> Unpack (no model) is the base, as on the pass.
    ofps::core::temporal::FrameInputs tinOut = c.tin;
    if (c.useBase && c.host && st.unpackBase) {
        EvalContext hc = *c.host;
        hc.cmd = cmd;
        hc.wantBase = true; hc.baseTarget = st.unpackBase; hc.baseTargetState = &st.unpackBaseState;
        if (st.warpPath == warp::PackPath::Pixel) hc.baseRtv = BaseRtv(st);
        c.stage = StagePackDraw;
        if (RecordPackStage(hc) == OFPS_OK) {
            if (st.warpPath == warp::PackPath::Pixel) PackedGuidesToPixel(st, cmd, hc.slot, hc.packSlot);
            c.stage = StageUnpackDraw;
            if (RecordBaseUnpack(hc) == ofps::sdk::AdapterStatus::Ok) { tinOut.base = st.unpackBase; tinOut.baseState = st.unpackBaseState; }
        }
    }
    const bool showPass = Ctx().diag.asyncShowPass; // DebugAsyncShowPass: the last pass's output as is (no reprojection)
    if (showPass && a.passes > 0 && a.outputDesc.Format == c.output->GetDesc().Format) {
        c.stage = StageCopyOut;
        ID3D12Resource *src = a.outputBg[a.inflight ? 1 - a.outIndex : a.outIndex];
        BarrierExternal(cmd, src, kModelOutputState, D3D12_RESOURCE_STATE_COPY_SOURCE);
        BarrierExternal(cmd, c.output, c.inputs.output.restState, D3D12_RESOURCE_STATE_COPY_DEST, c.inputs.output.subresource);
        D3D12_TEXTURE_COPY_LOCATION from{}, to{};
        from.pResource = src; from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        to.pResource = c.output; to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        to.SubresourceIndex = CopySubresource(c.inputs.output);
        const D3D12_BOX box{0, 0, 0, st.nativeWidth, st.nativeHeight, 1};
        cmd->CopyTextureRegion(&to, c.outputRect.x, c.outputRect.y, 0, &from, &box);
        BarrierExternal(cmd, c.output, D3D12_RESOURCE_STATE_COPY_DEST, c.inputs.output.restState, c.inputs.output.subresource);
        BarrierExternal(cmd, src, D3D12_RESOURCE_STATE_COPY_SOURCE, kModelOutputState);
    } else if (st.temporal->HasResidual()) {
        c.stage = StageTemporalReproject;
        st.temporal->RecordReproject(cmd, tinOut, KeepOutputOnInterpolation() ? nullptr : c.output, c.inputs.output.restState,
                                     c.outputRect.x, c.outputRect.y, c.inputs.output.subresource);
    } else if (a.colorDesc.Format == a.outputDesc.Format) {
        c.stage = StageCopyOut;
        BarrierExternal(cmd, c.color, c.inputs.color.restState, D3D12_RESOURCE_STATE_COPY_SOURCE, c.inputs.color.subresource);
        BarrierExternal(cmd, c.output, c.inputs.output.restState, D3D12_RESOURCE_STATE_COPY_DEST, c.inputs.output.subresource);
        D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource = c.output; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = CopySubresource(c.inputs.output);
        D3D12_TEXTURE_COPY_LOCATION src{}; src.pResource = c.color; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = CopySubresource(c.inputs.color);
        const D3D12_BOX box{c.tin.colorRect.x, c.tin.colorRect.y, 0, c.tin.colorRect.x + st.nativeWidth, c.tin.colorRect.y + st.nativeHeight, 1};
        cmd->CopyTextureRegion(&dst, c.outputRect.x, c.outputRect.y, 0, &src, &box);
        BarrierExternal(cmd, c.output, D3D12_RESOURCE_STATE_COPY_DEST, c.inputs.output.restState, c.inputs.output.subresource);
        BarrierExternal(cmd, c.color, D3D12_RESOURCE_STATE_COPY_SOURCE, c.inputs.color.restState, c.inputs.color.subresource);
    } else {
        // No residual yet and the formats differ: the colour through the machine's raw path.
        FallbackOutput(st, cmd, c.frame.color, c.frame.output, &tinOut, "background mode: no residual yet");
    }
    if (TemporalExhausted(st)) {
        FallbackOutput(st, cmd, c.frame.color, c.frame.output, &c.tin, "temporal descriptor pool exhausted");
        return OFPS_OK;
    }
    st.temporal->RecordFlowCapture(cmd, c.tin, st.realDevice, false);
    ++a.age;
    Ctx().status.asyncPasses = a.passes; Ctx().status.asyncForcedWaits = a.forcedWaits; Ctx().status.asyncStalls = a.stalls;
    Ctx().status.asyncPassesPerSecond = a.passesPerSecond; Ctx().status.asyncModelMs = a.lastModelMs; Ctx().status.asyncAge = a.age;
    Ctx().status.asyncQueueWaits = a.queueWaits; Ctx().status.asyncQueueWaitMs = a.queueWaitMs;
    c.stage = StageDone;
    c.result = OFPS_OK;
    return OFPS_OK;
}

int AsyncGuarded(AsyncCtx &c)
{
    __try {
        return AsyncBody(c);
    } __except (RecordCrash(GetExceptionInformation(), c.stage)) {
        return kCrashed;
    }
}

} // namespace ofps::core
