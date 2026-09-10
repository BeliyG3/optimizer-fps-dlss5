#pragma once

// What one warped evaluate records: the context that crosses the structured-exception frames, the
// Pack / model / Unpack body, the interpolated-frame body and the fallback that writes the host's
// own colour when no path produced the frame.

#include "feature_state.h"
#include "ngx_temporal.h"

#include <cstdint>

namespace pwhook {

struct EvalContext {
    FeatureState *st;
    ID3D12GraphicsCommandList *cmd;
    void *params;
    void *callback;
    ID3D12Resource *color, *depth, *motion, *output, *ui, *uiAlpha, *backbuffer;
    // 26.26: `motion` is what Pack reads - the accumulated displacement on a temporal full frame. The
    // host's own MVec is kept apart so RestoreParams hands the block back exactly as the host left it.
    ID3D12Resource *hostMotion = nullptr;
    D3D12_RESOURCE_STATES depthState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE; // 26.7.2
    UINT depthSub = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;                          // 26.7.3: 0 for planar depth-stencil guides
    Subrect colorRect, depthRect, motionRect, outputRect;
    float mvScaleX, mvScaleY;
    std::uint32_t slot, packSlot, unpackSlot;
    std::uint32_t packSet; // external source set the pack draw reads (a ring entry for host frames, the slot for the background job)
    pw::DepthConvention depthConvention;
    volatile int stage;
    bool paramsRewritten;
    // Temporal modes (all POD: this struct crosses __try frames).
    bool temporalActive;     // the machine is ready and a temporal mode is on
    bool temporalFull;       // this frame runs the model on the whole frame
    bool accumulate;         // record the motion accumulation this frame
    bool motionIsAcc;        // Pack reads the accumulated displacement instead of the host's motion
    pwtemporal::FrameInputs tin;
    // Temporal base (stage 26.3): after Pack, unpack the packed colour without the model into
    // `baseTarget` (native, output format) and base the residual / reprojection on it.
    bool wantBase;
    ID3D12Resource *baseTarget;
    D3D12_RESOURCE_STATES *baseTargetState;
    D3D12_CPU_DESCRIPTOR_HANDLE baseRtv;
};

D3D12_CPU_DESCRIPTOR_HANDLE BaseRtv(const FeatureState &st);
void RestoreParams(EvalContext &c);
int RecordPackStage(EvalContext &c);
pw::AdapterStatus RecordBaseUnpack(EvalContext &c);
void PackedGuidesToPixel(FeatureState &st, ID3D12GraphicsCommandList *cmd, std::uint32_t slot, std::uint32_t packSlot);
void FallbackOutput(FeatureState &st, ID3D12GraphicsCommandList *cmd, ID3D12Resource *color, const Subrect &colorRect,
                    ID3D12Resource *output, const Subrect &outputRect, const pwtemporal::FrameInputs *tin, const char *why);
void FallbackOutput(EvalContext &c, const char *why);
void FallbackFromParams(FeatureState &st, ID3D12GraphicsCommandList *cmd, void *params, const char *why);
pwtemporal::FrameInputs TemporalInputsWithBase(const EvalContext &c);
// The Pack -> model -> Unpack body itself; the background pass records it on its own list.
int WarpedBody(EvalContext &c);
// The structured-exception guards. SEH cannot live in a frame that unwinds C++ objects, so each
// body has its own guarded wrapper; kCrashed says the fault was caught and recorded.
LONG RecordCrash(EXCEPTION_POINTERS *info, int stage);
int InterpolateGuarded(EvalContext &c);
int WarpedGuarded(EvalContext &c);
void RestoreGuarded(EvalContext &c);

} // namespace pwhook
