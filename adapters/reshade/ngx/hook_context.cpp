#include "hook_context.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>

namespace pwhook {

HookContext &Ctx()
{
    static HookContext context;
    return context;
}

int CallCreate(ID3D12GraphicsCommandList *cmd, int featureId, void *params, void **outHandle)
{
    return pwngx::ForwardCreate(reinterpret_cast<pwngx::PFN_Create>(Ctx().realCreate), cmd, featureId, params, outHandle);
}
int CallEvaluate(ID3D12GraphicsCommandList *cmd, void *handle, void *params, void *callback)
{
    return pwngx::ForwardEvaluate(reinterpret_cast<pwngx::PFN_Evaluate>(Ctx().realEvaluate), cmd, handle, params, callback);
}
int CallRelease(void *handle) { return pwngx::ForwardRelease(reinterpret_cast<pwngx::PFN_Release>(Ctx().realRelease), handle); }

bool KeepBackbufferEnabled() { return Ctx().diag.keepBackbuffer; } // DebugKeepBackbuffer

void NotifyFirstWarped()
{
    if (Ctx().firstWarpedFired.load(std::memory_order_relaxed)) return;
    bool expected = false;
    if (!Ctx().firstWarpedFired.compare_exchange_strong(expected, true)) return;
    if (pw_ngx::FirstWarpedFn fn = Ctx().firstWarpedCallback.load()) fn();
}

bool WaitForGpu(ID3D12Device *device, ID3D12Device *proxyDevice) { return pwngx::WaitForGpu(device, proxyDevice); }

void SetReason(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(Ctx().status.reason, sizeof(Ctx().status.reason), fmt, args);
    va_end(args);
}

bool LoadShaders() { return pwngx::LoadShaders(Ctx().shaderDirectory, Ctx().shaders); }

void DrainGraveyard(bool everything) { Ctx().graveyard.Drain(everything, Ctx().evalCounter); }

bool TimingEnabled() { return Ctx().diag.timing; } // DebugTiming=1

bool KeepOutputOnInterpolation() { return Ctx().diag.temporalKeepOutput; }

// Cross-pass residual blend (26.6.K): weight of the previous pass's residual in the new one on every
// full pass, where the depth matches. Fixed, not a setting: 0.4 turns the pass-to-pass snap of fine
// detail into a fade while real changes converge within two passes.
constexpr float kResidualBlendDefault = 0.6f; // at rest (adaptive: fades out with motion and colour change in the shader); DebugTemporalBlend overrides
float ResidualBlendWeight()
{
    return Ctx().diag.temporalBlend >= 0.0f ? std::clamp(Ctx().diag.temporalBlend, 0.0f, 0.9f) : kResidualBlendDefault; // debug / A-B override
}

bool AsyncVerbose() { return Ctx().diag.asyncLog; } // DebugAsyncLog=1

} // namespace pwhook
