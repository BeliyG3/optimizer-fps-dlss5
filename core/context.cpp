#include "core/context.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>

namespace ofps::core {

namespace { EventSink eventSink = nullptr; }
void SetEventSink(EventSink sink) { eventSink = sink; }
void EmitEvent(OfpsEvent event, const OfpsEventData &data) { if (eventSink) eventSink(event, data); }

thread_local bool insideCore = false;

CoreContext &Ctx()
{
    static CoreContext context;
    return context;
}

bool KeepBackbufferEnabled() { return Ctx().diag.keepBackbuffer; } // DebugKeepBackbuffer

void NotifyFirstWarped()
{
    if (Ctx().firstWarpedFired.load(std::memory_order_relaxed)) return;
    bool expected = false;
    if (!Ctx().firstWarpedFired.compare_exchange_strong(expected, true)) return;
    EmitEvent(OFPS_EVENT_FIRST_WARPED_FRAME, {sizeof(OfpsEventData), nullptr, nullptr, nullptr});
}

bool WaitForGpu(ID3D12Device *device, ID3D12Device *proxyDevice) { return ofps::core::gpu::WaitForGpu(device, proxyDevice); }

void SetReason(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(Ctx().status.reason, sizeof(Ctx().status.reason), fmt, args);
    va_end(args);
}

bool LoadShaders() { return ofps::core::gpu::LoadShaders(Ctx().shaderDirectory, Ctx().shaders); }

void DrainGraveyard(bool everything) { Ctx().graveyard.Drain(everything, Ctx().evalCounter); }

void NoteCommandListExecuted(ID3D12CommandQueue *queue, ID3D12CommandList *list)
{
    Ctx().submission.Push(queue, list);
}

void DrainSubmissions(gpu::Submission::Signal policy)
{
    gpu::SubmissionEntry entries[gpu::kSubmissionRing];
    const auto count = Ctx().submission.Drain(entries, gpu::kSubmissionRing, policy);
    Ctx().status.submissionDrops = Ctx().submission.Dropped();
    AsyncNoteSubmissions(entries, count);
    TemporalFlowNoteSubmissions(entries, count);
}

void Housekeeping()
{
    if (insideCore) return;
    std::unique_lock<std::mutex> lock(Ctx().mutex, std::try_to_lock);
    if (!lock.owns_lock()) return;
    const InsideCoreScope coreScope;
    DrainSubmissions(gpu::Submission::Signal::Settled);
    DrainGraveyard(false);
}

OfpsFencePoint HostUsePoint(ID3D12CommandList *cmd)
{
    return Ctx().submission.UsePoint(cmd);
}

bool TimingEnabled() { return Ctx().diag.timing; } // DebugTiming=1

bool KeepOutputOnInterpolation() { return Ctx().diag.temporalKeepOutput; }

// Cross-pass residual blend (26.6.K): weight of the previous pass's residual in the new one on every
// full pass, where the depth matches. Fixed, not a setting: 0.4 turns the pass-to-pass snap of fine
// detail into a fade while real changes converge within two passes.
float ResidualBlendWeight()
{
    return std::clamp(Ctx().frameProfile.residualBlend, 0.0f, 0.9f);
}

bool AsyncVerbose() { return Ctx().diag.asyncLog; } // DebugAsyncLog=1

} // namespace ofps::core
