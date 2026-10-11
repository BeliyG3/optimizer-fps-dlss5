#pragma once

// Temporal NR machine (D3D12): the GPU side of the "model every N-th frame" mode. It owns the
// residual of the last full model pass, the depth of that frame, the accumulated motion since it
// and an interpolation target, and records the full-screen passes (shaders/temporal.hlsl) into the
// host's command list. It knows nothing about NGX or the warp layout: the hook decides which frame
// is which.

#include "core/gpu/queues.h"
#include "core/api/ofps_core.h"
#include "core/gpu/graveyard.h"
#include "core/gpu/shaders.h"
#include "core/gpu/barriers.h"
#include "core/gpu/crash_guard_seh.h"
#include "core/log.h"
#include "core/temporal/stats.h"
#include "core/temporal/pass_timing.h"
#include "core/flow/FrameState.h"
#include "core/temporal/machine_role.h"
#include "core/temporal/raw_snapshot.h"

#include <cstdint>
#include <vector>

namespace ofps::core::temporal {

// DebugTemporalDumpAcc (core/frame/acc_dump.h, acc_dump_copies.cpp): what the caller names a frame's dump with,
// and the model's motion and the chain read back from it as RG16F texel pairs of the motion texture.
struct AccDumpTags {
    std::uint64_t eval = 0;
    std::uint32_t fullPass = 0;
    float mvScaleX = 1.0f, mvScaleY = 1.0f;
    bool carried = false; // a carried frame: no model motion nor reasons; the chain fills the model plane, reasons 0
    bool carry = false;   // DebugTemporalCarryChainRule above 0: a fourth plane, the chain the reprojection reads (CarryAcc())
};
struct AccDumpHalves {
    AccDumpTags tags;
    std::uint32_t width = 0, height = 0;
    std::vector<std::uint16_t> model, chain; // 2 * width * height halves each, rows tightly packed
    std::vector<std::uint8_t> reason;        // width * height marker reasons (PwTMarkerReason), rows tightly packed
    std::vector<std::uint16_t> carry;        // tags.carry: CarryAcc() as chain; empty otherwise
};

struct Rect {
    std::uint32_t x = 0, y = 0, w = 0, h = 0;
};

// Everything the machine owns on the GPU (temporal_resources.h).
struct Resources;
struct Constants;

// The host's inputs for one frame, all in their own texture spaces.
struct FrameInputs {
    ID3D12Resource *color = nullptr;   // native colour the model saw / would see
    ID3D12Resource *motion = nullptr;  // host motion vectors
    ID3D12Resource *depth = nullptr;   // host depth
    DXGI_FORMAT colorView = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT motionView = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT depthView = DXGI_FORMAT_UNKNOWN;
    Rect colorRect, motionRect, depthRect;
    float mvScaleX = 1.0f, mvScaleY = 1.0f;
    bool depthInverted = false;
    float depthThreshold = 0.05f; // relative depth mismatch that counts as disocclusion; <= 0 disables the test
    float colorTolerance = 0.08f; // relative luma mismatch against the residual frame's colour; <= 0 disables the test
    float motionSign = 1.0f;      // +1: vectors point from the current frame to the previous one (NGX); -1 flips them
    bool rawInterpolation = false; // diagnostics: interpolated frames carry the raw colour (no residual)
    bool holeFill = true;          // rejected pixels get the box-filtered residual instead of nothing
    float mvSearchRadiusPx = 16.0f; // 26.14: when no motion texel around the pixel matches its depth, search this far (native px) for one that does; 0 = four texels only
    // RecordResidual: weight of the previous residual (moved to this pass's frame, depth-checked) in the
    // new one; 0 = the plain difference. The displacement is the machine's own accumulation chain, or
    // `motion` when `blendFromMotion` (background mode: the kick's chain copy, scale 1).
    float residualBlend = 0.0f;
    float ratioScale = 0.0f;
    bool lumaBand = false;
    bool fillFloor = false;
    bool hostModelGrid = false; // OptiScaler keeps temporal on the frame grid while encoding a separate model grid
    bool rawFull = false;
    bool history = true;
    bool historySearch = true;
    bool refine = true;
    bool compose = true;
    std::uint32_t historyPasses = 2;
    bool blendFromMotion = false;
    bool motionIsPassChain = false; // background mode: `motion` holds the displacement to the previous pass's frame (scale 1) - usable as the second-level history link
    bool residualCatmullRom = true; // reprojection samples the residual with Catmull-Rom (fine detail survives fractional displacements) instead of bilinear
    float smoothRadius = 24.0f;    // 26.6.X: the reprojection's addition is smoothed along the original's smoothness around rejected pixels (px, 0 = off)
    int debugVis = 0;             // reproject output: 0 normal, 1 displacement/weight, 2 residual, 3 raw colour
    int markerRule = 0;           // ModelMotion's no-history marker rule (PwTMarkerReason, 0..4; model_marker.h)
    int markerNeighbours = 0;     // rules 3/4's 5x5 threshold (0 = 12; model_marker.h)
    bool oracleChain = false;   // DebugTemporalCarryProbe bit 0: the host's motion IS the chain (bench oracle), sync mode only
    bool acceptAll = false;     // bit 1: reprojection accepts every in-frame pixel (attribution run R2)
    std::uint32_t dumpAccFrom = 0; // DebugTemporalDumpAccFrom: dump the chain on every evaluate of [from, from+63]
    int chainRule = 0;          // DebugTemporalChainRule: when the main chain restarts (PSAccumulate, 0..4; 0 = today's)
    int carryChainRule = 0;     // DebugTemporalCarryChainRule: the carry chain's rule (carry_chain.cpp, 0..6; 0 = no carry chain)
    // Resource states the host keeps these in (transitions to PIXEL_SHADER_RESOURCE and back).
    D3D12_RESOURCE_STATES hostInputState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    D3D12_RESOURCE_STATES depthState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE; // 26.7.2
    D3D12_RESOURCE_STATES motionState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    UINT colorSubresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    UINT motionSubresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    UINT baseSubresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    UINT depthSubresource =
        0xffffffffu; // 26.7.3: 0 for planar depth-stencil guides (depth plane only), ALL otherwise: the depth guide's own resting state (a depth-stencil resource rests in DEPTH_READ | NPSR)
    // Warped path: the colour the residual and the reprojection are based on is the *unpacked* colour
    // (native size, the output's format, no sub-rect) rather than the host's, so the residual holds
    // only the model's contribution and every frame carries the same compression blur. Null = use
    // `color`. `baseState` is the state `base` rests in.
    ID3D12Resource *base = nullptr;
    D3D12_RESOURCE_STATES baseState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    D3D12_RESOURCE_STATES colorState = D3D12_RESOURCE_STATE_COMMON; // set by the machine: the colour's resting state (hostInputState or baseState)
    // Shared compute wrapper: shaders/temporal_cs.hlsl.
    bool expectedDepth = true;      // PW_T_EXPECT: carry the depth a surface had in the residual's frame along the chain
    bool background = false;       // host depth advances separately; residual inputs belong to the saved kick
    bool opticalFlow = false;      // private OptiScaler motion source; never changes host motion grid
    bool cells = true;              // PW_T_CELLS: paint rejected pixels from cells of the accepted addition
    bool skyField = true;           // TemporalSkyField: far-plane pixels of carried frames take the sky field (sky_field.h)
    bool allowSky = true;           // false on a menu evaluation: the saved gameplay depth does not describe the menu frame
    std::uint32_t phaseInFrames = 0; // PW_T_RAMP: carried frames a new pass is phased in over (0 = shown at once)
    // A carried frame's reprojection on this grid (core/temporal/grid.h; 0 = native). Only the compose route takes
    // it, and only when the grid pipelines exist and the grid fits the native size; everything else stays native.
    std::uint32_t gridWidth = 0, gridHeight = 0;
};

// Background adoption (temporal mode 3): the residual adopted on a later frame was measured on the kick's frame, so its
// depth guide is the kick's private copy, read as the kick's frame described it (sub-rect, typed view, orientation),
// not as the adoption frame does: a host may change either in between, and the sky field (sky_field.h) classifies
// that depth by its far plane.
// A full pass's inputs (carry_chain.cpp): its RecordResidual drops the carry chain and nothing reads it before, so its
// RecordAccumulate leaves it out; a pass that ends without a residual reprojects along the main chain until the next one.
FrameInputs FullPassInputs(FrameInputs in);

inline void UseKickDepth(FrameInputs &in, ID3D12Resource *copy, const OfpsResource &kickDepth, bool kickInverted)
{
    in.depth = copy;
    in.depthView = kickDepth.view;
    in.depthRect = {kickDepth.rect.x, kickDepth.rect.y, kickDepth.rect.w, kickDepth.rect.h};
    in.depthInverted = kickInverted;
}

class Machine final : public ofps::core::gpu::Disposable {
public:
    Machine();
    ~Machine() override;
    Machine(const Machine &) = delete;
    Machine &operator=(const Machine &) = delete;

    // `device` is the command list's device (a ReShade proxy when present). `outputFormat` is the
    // resource format of the host's output (the interpolation target copies into it), `outputView`
    // its typed view. Motion/depth extents come from the host's textures. `role` decides the optional
    // textures (machine_role.h): only a Background machine can run the background mode's records, and
    // a Hidden one keeps a single residual and never blends.
    bool Initialize(ID3D12Device *device, const ofps::core::gpu::Shaders &shaders, std::uint32_t nativeWidth, std::uint32_t nativeHeight,
                    DXGI_FORMAT outputFormat, DXGI_FORMAT outputView, std::uint32_t motionWidth, std::uint32_t motionHeight,
                    DXGI_FORMAT depthFormat, std::uint32_t depthWidth, std::uint32_t depthHeight, char *error, std::size_t errorSize,
                    std::uint32_t pictureDivisor = 3, MachineRole role = MachineRole::Synchronous);
    bool Ready() const { return device_ != nullptr; }
    bool Matches(std::uint32_t nativeWidth, std::uint32_t nativeHeight, DXGI_FORMAT outputFormat, std::uint32_t motionWidth,
                 std::uint32_t motionHeight, DXGI_FORMAT depthFormat, std::uint32_t depthWidth, std::uint32_t depthHeight,
                 std::uint32_t pictureDivisor = 3, MachineRole role = MachineRole::Synchronous) const;
    MachineRole Role() const;

    // Records the residual of a full pass: fresh (the final native output, in `freshState`) minus
    // the host colour; also snapshots the host depth. Afterwards HasResidual() is true.
    void RecordResidual(ID3D12GraphicsCommandList *cmd, const FrameInputs &in, ID3D12Resource *fresh,
                        D3D12_RESOURCE_STATES freshState,
                        UINT freshSubresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES);
    // Accumulates the host's motion into the running displacement (call once per frame before any
    // use of Acc()).
    void RecordAccumulate(ID3D12GraphicsCommandList *cmd, const FrameInputs &in);
    // Writes colour + reprojected residual into the interpolation target and copies it into
    // `hostOutput` at (dstX, dstY); `hostOutput` may be null (diagnostics: the target is kept).
    void RecordReproject(ID3D12GraphicsCommandList *cmd, const FrameInputs &in, ID3D12Resource *hostOutput,
                         D3D12_RESOURCE_STATES hostOutputState, std::uint32_t dstX, std::uint32_t dstY, UINT outputSubresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES);
    // The accumulated displacement texture (motion-texture size, RG16F, pixels of the motion texture,
    // already scaled) after RecordAccumulate; readable in `hostInputState`... it is left in
    // NON_PIXEL_SHADER_RESOURCE so a model can consume it directly with MVecScale = 1.
    ID3D12Resource *Acc() const;
    // The texture the next RecordAccumulate writes (so descriptors can be prepared before recording).
    ID3D12Resource *NextAcc() const;
    // 26.28: the chain as the model's OWN motion vectors - pixels whose chain does not end on their
    // surface in the residual's frame get a vector that leaves the picture, so the model treats them as
    // its own disocclusion instead of dragging the occluder's history onto them. Record it with
    // RecordModelMotion after RecordAccumulate; null when the pass is not available (hand out Acc()).
    ID3D12Resource *ModelMv() const;
    // The chain the reprojection reads (carry_chain.cpp): the carry chain (DebugTemporalCarryChainRule 1..6) once it was
    // accumulated since the last reset, Acc() otherwise. The model, its markers, the history link, the statistics and the
    // chain dump read Acc().
    ID3D12Resource *CarryAcc() const;
    bool CarryChainCreated() const; // its textures exist (created by the first carry rule above 0)
    void RecordModelMotion(ID3D12GraphicsCommandList *cmd, const FrameInputs &in);
    // 26.28: a new pass is phased in and this frame must be shown as "colour + the residual mix"
    // (RecordApply) rather than as the model's raw answer.
    bool PhaseInActive() const;
    void RecordApply(ID3D12GraphicsCommandList *cmd, const FrameInputs &in, ID3D12Resource *hostOutput,
                     D3D12_RESOURCE_STATES hostOutputState, std::uint32_t dstX, std::uint32_t dstY, UINT outputSubresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES);
    // The colour as it is into `target` at (dstX, dstY), without advancing the displayed residual's phase
    // clock or sampling the statistics: a plain copy where that leaves the same bytes, NaN payloads aside
    // (raw_snapshot.h), the format-converting reprojection (debugVis 3) otherwise. Says which ran.
    RawRoute RecordRaw(ID3D12GraphicsCommandList *cmd, const FrameInputs &in, ID3D12Resource *target,
                   D3D12_RESOURCE_STATES state, std::uint32_t dstX = 0, std::uint32_t dstY = 0, UINT outputSubresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES);
    bool AccValid() const { return accValid_; }
    bool HasResidual() const { return hasResidual_; }
    // The frames (the evaluate counter SetUsePoint names) the chains reach back to: Acc() to its residual's frame (a
    // RecordResidual; after PromotePending, the adopted pass's kick), the pending chain to its last ResetPending.
    std::uint64_t ChainFrame() const { return chainFrame_; }
    std::uint64_t PendingFrame() const { return pendingFrame_; }
    // Second accumulation chain for the background mode: the displacement from the current frame back
    // to the frame whose model pass is in flight. RecordAccumulatePending once per frame after the kick;
    // PromotePending when that pass is adopted (it becomes the main chain, the pending one restarts).
    void RecordAccumulatePending(ID3D12GraphicsCommandList *cmd, const FrameInputs &in);
    void ResetPending(); // the pending chain restarts from this frame
    void RecordBackgroundAccumulate(ID3D12GraphicsCommandList *cmd, const FrameInputs &in, bool pending);
    // Save the kick's expectation for residual alignment; optionally validate a private model chain.
    // kickColor/kickDepth: the previous kick's private copies (the scheduler's colorBg/depthBg, replaced only after this
    // call); kickDepthRect: the sub-rect that kick described its depth with (w = 0: unknown, the current frame's).
    void RecordBackgroundKick(ID3D12GraphicsCommandList *cmd, const FrameInputs &in, bool pending,
                              ID3D12Resource *modelVectors, D3D12_RESOURCE_STATES vectorState,
                              ID3D12Resource *kickColor, ID3D12Resource *kickDepth, const Rect &kickDepthRect, bool validate);
    void PromotePending();
    bool PendingValid() const { return pendingValid_; }
    // After an adoption without a new kick the displacement to the last pass's frame is the main
    // chain itself: the pending chain mirrors it (no separate accumulation) until the next kick.
    bool PendingMirrorsAcc() const { return pendingMirror_; }
    // Copies the pending (or main) accumulation into `dst` (motion-texture size, RG16F); `dst` is
    // returned to `dstState`.
    void RecordCopyAcc(ID3D12GraphicsCommandList *cmd, bool pending, ID3D12Resource *dst, D3D12_RESOURCE_STATES dstState);
    // Forgets the residual and the accumulation (a reset, a re-created model, a layout change).
    void Invalidate();
    void SetUsePoint(const OfpsFencePoint &point, std::uint64_t evalNow, bool timing = false);
    bool TakeExhausted();
    StatsSnapshot Stats() const;
    PassTimingSnapshot Timing() const;
    // Debug: copies one 256-byte row of the host colour, the residual, the interpolation target and
    // the accumulated displacement at native pixel (x, y) into `readback` (rows at 0/256/512/768).
    void RecordDebugCopies(ID3D12GraphicsCommandList *cmd, const FrameInputs &in, ID3D12Resource *readback, std::uint32_t x, std::uint32_t y);
    // Debug (DebugTemporalDumpAcc): copies ModelMv() and Acc() whole into a readback buffer created on first use and
    // released with the machine, with markerReason (RecordMarkerReason) as a third plane, and keeps this frame's fence
    // point. Call right after RecordMarkerReason, or for tags.carried right after RecordAccumulate (the chain in both
    // planes, reasons 0). False when nothing was recorded; *dropped: an earlier copy the GPU had not passed yet was
    // replaced (lost).
    bool RecordAccDumpCopies(ID3D12GraphicsCommandList *cmd, const AccDumpTags &tags, bool *dropped);
    // Debug (DebugTemporalDumpAcc): why RecordModelMotion marked each texel (PwTMarkerReason: 0 kept, 1 off screen, 2
    // depth, 3 colour, 4 non-finite) into an R8_UINT target created on first use, for RecordAccDumpCopies' third plane.
    // Call right after RecordModelMotion with the same inputs; false when nothing was recorded.
    bool RecordMarkerReason(ID3D12GraphicsCommandList *cmd, const FrameInputs &in);
    // Never waits: 1 = the last recorded copy is in `out`, 0 = none pending, -1 = the GPU has not passed it yet.
    int TakeAccDump(std::uint64_t evalNow, AccDumpHalves &out);
    // nativeDevice keys the optical flow session; nativeDevice/proxyDevice key the host queues that wait for its setup.
    void RecordFlowCapture(ID3D12GraphicsCommandList *cmd, const FrameInputs &in, ID3D12Device *nativeDevice,
                           ID3D12Device *proxyDevice, bool full, bool referenceOnly = false);
    bool RecordFlowSeed(ID3D12GraphicsCommandList *cmd, const FrameInputs &in);
    void NoteFlowSubmission(std::uint64_t tag, ID3D12CommandQueue *queue) { flow_.Submitted(tag, queue); }
    const std::string &FlowProblem() const { return flow_.Problem(); }
    bool FlowRunning() const { return flowRunning_; }

private:
    // RecordReproject's body; `sampleStats` is false for RecordRaw, whose frame is not a carried one.
    void Reproject(ID3D12GraphicsCommandList *cmd, const FrameInputs &in, ID3D12Resource *hostOutput,
                   D3D12_RESOURCE_STATES hostOutputState, std::uint32_t dstX, std::uint32_t dstY, UINT outputSubresource,
                   bool sampleStats);
    // RecordRaw's copy: the colour's native-size sub-rect of subresource 0 into `target`.
    void CopyRaw(ID3D12GraphicsCommandList *cmd, const FrameInputs &in, ID3D12Resource *target, D3D12_RESOURCE_STATES state,
                 std::uint32_t dstX, std::uint32_t dstY, UINT outputSubresource);
    // The expectation's own link chain (PW_T_EXPECT): recorded before the accumulation, which reads it.
    void RecordExpect(ID3D12GraphicsCommandList *cmd, const FrameInputs &in, bool on, bool pending = false);
    void RecordDepthPrev(ID3D12GraphicsCommandList *cmd, const FrameInputs &in);
    // Both chains in one pass each (carry_chain.cpp): AccumulatePair and RefinePair in place of the main chain's LinkFail,
    // Accumulate and Refine, with `c` the main Accumulate's constants. `adopt`: the index of the main
    // chain's previous value when the carry chain adopts it (no carry chain since the last reset, an adopted background
    // pass, an optical-flow seed), -1 to continue the carry chain's own. False, with nothing recorded, when the pair
    // cannot run: the caller then records the main chain alone.
    bool RecordChainPair(ID3D12GraphicsCommandList *cmd, const FrameInputs &in, Constants c, int adopt);
    // The host depth into one of our snapshots; the depth plane alone for planar depth-stencil guides.
    static void CopyDepth(ID3D12GraphicsCommandList *cmd, const FrameInputs &in, ID3D12Resource *dst);
    void CopyInterp(ID3D12GraphicsCommandList *cmd, ID3D12Resource *hostOutput, D3D12_RESOURCE_STATES hostOutputState,
                    std::uint32_t dstX, std::uint32_t dstY, UINT outputSubresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES);

    Resources *res_ = nullptr;
    ID3D12Device *device_ = nullptr;
    bool accValid_ = false;
    bool pendingValid_ = false;
    bool pendingMirror_ = false;
    bool hasResidual_ = false;
    bool expectValid_ = false;
    bool expectPendingValid_ = false;
    bool carryValid_ = false; // the carry chain holds this frame's displacement (reset with the main chain)
    // acc[1 - accCurrent] still holds the main chain as the last RecordAccumulate's Accumulate left it, before its Refine:
    // DebugTemporalCarryChainRule 6 reads last frame's Refine correction from the pair (carry_chain.cpp).
    bool unrefinedKept_ = false;
    std::uint64_t chainFrame_ = 0, pendingFrame_ = 0;
    flow::FrameState flow_;
    bool flowRunning_ = false;
    bool flowLogged_ = false, flowActiveLogged_ = false;
};

} // namespace ofps::core::temporal
