#pragma once

// What one warped evaluate records: the context that crosses the structured-exception frames, the
// Pack / model / Unpack body, the interpolated-frame body and the fallback that writes the host's
// own colour when no path produced the frame.

#include "core/frame/feature_state.h"
#include "core/temporal/machine.h"

#include <cstdint>

namespace ofps::core {

struct CodecFrame;
struct EvalContext {
    FeatureState *st;
    ID3D12GraphicsCommandList *cmd;
    OfpsModelInputs modelInputs;
    OfpsResource colorResource, motionResource, outputResource;
    const OfpsFrameInputs *frame = nullptr;
    CodecFrame *codec = nullptr;
    ID3D12Resource *color, *depth, *motion, *output, *ui, *uiAlpha, *backbuffer;
    // 26.26: `motion` is what Pack reads - the accumulated displacement on a temporal full frame. The
    // host's own MVec is kept apart from the accumulated vectors used by Pack.
    ID3D12Resource *hostMotion = nullptr;
    D3D12_RESOURCE_STATES depthState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE; // 26.7.2
    UINT depthSub = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;                          // 26.7.3: 0 for planar depth-stencil guides
    OfpsRect colorRect, depthRect, motionRect, outputRect;
    float mvScaleX, mvScaleY;
    std::uint32_t slot, packSlot, unpackSlot;
    std::uint32_t packSet; // external source set the pack draw reads (a ring entry for host frames, the slot for the background job)
    ofps::sdk::DepthConvention depthConvention;
    volatile int stage;
    // Temporal modes (all POD: this struct crosses __try frames).
    bool temporalActive;     // the machine is ready and a temporal mode is on
    bool temporalFull;       // this frame runs the model on the whole frame
    bool accumulate;         // record the motion accumulation this frame
    bool motionIsAcc;        // Pack reads the accumulated displacement instead of the host's motion
    bool modelMotion;        // 26.28: that displacement is the tested one (PSModelMotion), recorded after the accumulation
    ofps::core::temporal::FrameInputs tin;
    // Temporal base (stage 26.3): after Pack, unpack the packed colour without the model into
    // `baseTarget` (native, output format) and base the residual / reprojection on it.
    bool wantBase;
    ID3D12Resource *baseTarget;
    D3D12_RESOURCE_STATES *baseTargetState;
    D3D12_CPU_DESCRIPTOR_HANDLE baseRtv;
    ofps::sdk::D3D12SourceResources packSources{};
    ofps::sdk::InputDescriptionV2 packInput{};
    OfpsFencePoint privateUsePoint{};
    // Set by WarpedBody when a temporal consumer's transfer, expected by TransferReplacesBase, was
    // declined: no residual from this frame. WarpedBody resets a host temporal frame's history itself;
    // the background pass is discarded and the spread cycle restarted by their callers.
    bool transferDeclined = false;
};

// Colour filters 2/3: the compute Unpack returns the full-size host frame plus the model's edit.
inline bool DetailTransferOn(const FeatureState &st) {
    return static_cast<std::uint32_t>(st.layout.colorFilter) >=
           static_cast<std::uint32_t>(ofps::sdk::ColorFilter::DetailTransfer);
}
// With the transfer there is no stretched frame to measure a temporal residual against, so the base
// (Pack -> Unpack without the model) is not recorded. Only where the transfer will apply: the pixel
// path (soft filter) and a colour/answer view pairing the transfer declines keep their base. The other
// declines (a Pack from another evaluate, a source aliasing the target) cannot happen on a temporal,
// spread or background frame; WarpedBody resets the history if one does.
inline bool TransferReplacesBase(const ofps::sdk::LayoutV2 &layout, warp::PackPath path, DXGI_FORMAT colorView,
                                 DXGI_FORMAT outputView) {
    return static_cast<std::uint32_t>(layout.colorFilter) >=
               static_cast<std::uint32_t>(ofps::sdk::ColorFilter::DetailTransfer) &&
           path == warp::PackPath::Compute && warp::TransferEligible(colorView, outputView);
}
inline bool TransferReplacesBase(const FeatureState &st) {
    return TransferReplacesBase(st.layout, st.warpPath, st.colorView, st.outputView);
}
// After a compute Unpack: false when the base was dropped for a transfer that was requested but not
// applied - a residual measured from that plain, stretched unpack would carry the stretch blur.
// Over-cautious by design: a decline on a reproject-only frame or on the non-identity codec path (no
// residual, no unpack base) also invalidates the history; unreachable today, and harmless.
constexpr bool KeepResidualAfterUnpack(bool requested, bool applied, bool replacesBase) {
    return !(requested && !applied && replacesBase);
}

D3D12_CPU_DESCRIPTOR_HANDLE BaseRtv(const FeatureState &st);
ofps::sdk::D3D12PackedViews PackedViewsForFeature(const FeatureState &st, std::uint32_t slot);
int RecordPackStage(EvalContext &c);
ofps::sdk::AdapterStatus RecordBaseUnpack(EvalContext &c);
void PackedGuidesToPixel(FeatureState &st, ID3D12GraphicsCommandList *cmd, std::uint32_t slot, std::uint32_t packSlot);
void FallbackOutput(FeatureState &st, ID3D12GraphicsCommandList *cmd, const OfpsResource &colorResource,
                    const OfpsResource &outputResource, const ofps::core::temporal::FrameInputs *tin,
                    const char *why);
void FallbackOutput(EvalContext &c, const char *why);
void FallbackFromFrame(FeatureState &st, ID3D12GraphicsCommandList *cmd, const OfpsFrameInputs &frame, const char *why);
ofps::core::temporal::FrameInputs TemporalInputsWithBase(const EvalContext &c);
// The Pack -> model -> Unpack body itself; the background pass records it on its own list.
int WarpedBody(EvalContext &c);
int FinishWarpCodec(EvalContext &c);
// The structured-exception guards. SEH cannot live in a frame that unwinds C++ objects, so each
// body has its own guarded wrapper; kCrashed says the fault was caught and recorded.
LONG RecordCrash(EXCEPTION_POINTERS *info, int stage);
int InterpolateGuarded(EvalContext &c);
int WarpedGuarded(EvalContext &c);

} // namespace ofps::core
