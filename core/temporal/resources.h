#pragma once

// What the temporal machine owns on the GPU: the root signature and the pipeline per pass, the
// descriptor-table ring and every texture the passes read or write, plus the two small
// helpers that turn a frame's inputs into root constants and into a descriptor table.
//
// ngx_temporal.cpp records the passes; nothing here knows the order they run in.

#include "core/gpu/queues.h"
#include "core/gpu/descriptor_pool.h"
#include "core/gpu/graveyard.h"
#include "core/gpu/shaders.h"
#include "core/gpu/barriers.h"
#include "core/gpu/crash_guard_seh.h"
#include "core/log.h"
#include "core/temporal/machine.h"
#include "core/temporal/phase.h"
#include "core/temporal/history.h"
#include "core/temporal/sky_field.h"
#include "core/temporal/stats.h"
#include "core/temporal/pass_timing.h"
#include "core/shaders/temporal_layout.h"

#include <cstdint>

namespace ofps::core::temporal {

// Shared t0..t19, u0/u1 and b0/b1 contract. The add-on reserves t9 for optional optical flow.
using namespace pwtemporalcontract;
constexpr int kTableUsed = PW_TEMPORAL_SRV_COUNT;
// Fresh tables for both chains, adoption and kick, with queue headroom. 512: a host list no queue event reports submitted
// (renodx's Present hook point) frees its tables kDeferredReleaseEvaluates evaluates after use, and the synchronous mode
// takes about 15 per evaluate: 17 evaluates in flight filled 256 exactly (bench, renodx 8.5 NRHookPoint=2).
constexpr std::uint32_t kTableSlots = 512;
constexpr std::uint32_t kFallbackTableSlots = 64; // independent capacity for raw colour conversion
constexpr std::uint32_t kLowDivisor = 16; // low-res residual and cell grid: native / 16 per axis
constexpr D3D12_RESOURCE_STATES kReadable = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
// The accumulated displacement is also read by the model, which is a compute shader.
// The accumulated motion rests where compute passes and the model read it. NON_PIXEL only: a COMPUTE
// host list (DLSS5-Reshade-AIO) cannot name PIXEL_SHADER_RESOURCE; the pixel Pack moves it there and back.
constexpr D3D12_RESOURCE_STATES kAccState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

// Output texture indices. The residual and both accumulation chains ping-pong, so each gets a pair; the
// indices travel with the textures when the chains are swapped (PromotePending, RecordResidual).
enum TargetIndex : int {
    kTargetResidualA = 0,
    kTargetResidualB,
    kTargetAcc0,
    kTargetAcc1,
    kTargetAccPending0,
    kTargetAccPending1,
    kTargetInterp,
    kTargetResidualLow,
    kTargetAddition,     // the reprojection's second target (26.6.X), smoothed by the compose pass
    kTargetResidualOld,  // 26.28 PW_T_RAMP: the previous pass's residual on the new pass's frame
    kTargetExpect0,      // 26.28 PW_T_EXPECT: the expected depth, ping-pong
    kTargetExpect1,
    kTargetExpectPending0,
    kTargetExpectPending1,
    kTargetCellsAdd,     // 26.28 PW_T_CELLS: the accepted addition per cell and the look it came from
    kTargetCellsLook,
    kTargetModelMv,      // 26.28: the chain as the model's own motion vectors
    kTargetResidualMix,  // interrupted background ramp, frozen before residualOld is replaced
    kTargetCount
};

// Mirrors cbuffer PwTemporalConstants in shaders/temporal.hlsl: 11 float4 of root constants. The
// shader carries the per-lane description.
struct Constants {
    float native[4];     // native width, height, 1/width, 1/height
    float colorRect[4];  // host colour sub-rect x, y, w, h
    float motionRect[4]; // host motion sub-rect x, y, w, h
    float depthRect[4];  // host depth sub-rect x, y, w, h
    float motionTex[4];  // motion texture width, height, MVecScale X, MVecScale Y
    float params[4];     // per-pass switches: validate link, accPrev valid / Catmull-Rom, debug vis, depth threshold
    float tune[4];       // colour tolerance, motion sign, raw interpolation, residual blend weight
    float fill[4];       // hole fill on/off, low-res width, low-res height, depth-guided chain fetch radius
    float smoothing[4];  // acceptance tap scale, guided smoothing radius, ratio domain (unused here), share of the new residual
    float sky[4];        // x: the host's far-plane depth (1 standard, 0 reversed Z) for the sky field; y: the probe's accept-all
                         // (Reproject); z: the main chain's rule (Accumulate, LinkFail, AccumulatePair); w: the carry chain's
                         // rule (AccumulatePair); 0 on every other pass (PwTSky, temporal.hlsl)
    float depthFRect[4]; // the sub-rect of the depth at t6 as its own frame described it (Resources::depthFRect)
    std::uint32_t hostModelGrid = 0; // CPU pipeline selection, not part of the shader constants
    // CPU only: a carried frame's grid (0 = native). Selects the grid pipelines of the reprojection, cells and
    // compose and reaches them through dispatch lanes 6-7 (PwTGridSize), never through the root constants.
    std::uint32_t gridWidth = 0, gridHeight = 0;
};
static_assert(sizeof(Constants) == 47 * sizeof(float));
constexpr UINT kConstantDwords = 44;

struct SlotKey {
    ID3D12Resource *res[kTableUsed] = {};
    DXGI_FORMAT fmt[kTableUsed] = {};
    bool valid = false;
};

struct FlowExtent { std::uint32_t width = 0, height = 0, grid = 0; };

struct Resources {
    ID3D12Device *device = nullptr;
    ID3D12RootSignature *rootSignature = nullptr;
    // The complete pass set is required: partial shader installs fail explicitly.
#define PW_TEMPORAL_PASS(name, member, reads, outputs, extent) ID3D12PipelineState *member##Pso = nullptr;
#include "core/shaders/temporal_passes.def"
#undef PW_TEMPORAL_PASS
    ID3D12PipelineState *refineModelPso = nullptr, *flowLumaModelPso = nullptr;
    ID3D12PipelineState *refinePairModelPso = nullptr; // optional: RefinePair on the model grid (the carry chain's)
    // Optional: a carried frame's reprojection on the model's grid and the passes reading it (all three or none).
    ID3D12PipelineState *reprojectGridPso = nullptr, *composeGridPso = nullptr, *cellsGridPso = nullptr;
    bool GridReady() const { return reprojectGridPso != nullptr && composeGridPso != nullptr && cellsGridPso != nullptr; }
    // Optional: the sky field's passes (all four or none) and its pyramid.
#define PW_TEMPORAL_PASS(name, member, reads, outputs, extent) ID3D12PipelineState *member##Pso = nullptr;
#include "core/shaders/temporal_sky_passes.def"
#undef PW_TEMPORAL_PASS
    bool SkyPassesReady() const
    {
#define PW_TEMPORAL_PASS(name, member, reads, outputs, extent) if (member##Pso == nullptr) return false;
#include "core/shaders/temporal_sky_passes.def"
#undef PW_TEMPORAL_PASS
        return true;
    }
    bool SkyReady() const { return SkyPassesReady() && sky.Created(); }
    // Optional, each on its own: the marker's passes (temporal_marker_passes.def).
#define PW_TEMPORAL_PASS(name, member, reads, outputs, extent) ID3D12PipelineState *member##Pso = nullptr;
#include "core/shaders/temporal_marker_passes.def"
#undef PW_TEMPORAL_PASS
    // DebugTemporalCarryChainRule's carry chain (carry_chain.cpp) needs the two pair passes, and RefinePair's model-grid
    // variant on a host that keeps a model grid.
    bool PairReady(bool modelGrid) const
    {
        return accumulatePairPso != nullptr && refinePairPso != nullptr && (!modelGrid || refinePairModelPso != nullptr);
    }
    ID3D12DescriptorHeap *srvHeap = nullptr;
    UINT srvIncrement = 0;
    struct Output { ID3D12Resource *resource = nullptr; DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN; };
    Output targets[kTargetCount]{};

    ID3D12Resource *residual = nullptr, *depthF = nullptr, *acc[2] = {}, *interp = nullptr;
    ID3D12Resource *residualPrev = nullptr; // the previous full pass's residual (ping-pong with `residual`); none when Hidden
    // 26.6.X: the reprojection's addition + acceptance (RGBA16F native), smoothed by the compose pass.
    ID3D12Resource *toneAcc = nullptr;
    ID3D12Resource *accP[2] = {}; // pending chain (background mode; a Background machine only)
    ID3D12Resource *colorF = nullptr;     // snapshot of the host colour of the residual's frame (created on demand)
    ID3D12Resource *residualLow = nullptr; // box-filtered residual (hole fill); null when the shader is missing
    // 26.28 PW_T_EXPECT: the depth each texel's surface had in the residual's frame, carried along the
    // chain, and the previous frame's depth each link is checked against.
    ID3D12Resource *expect[2] = {}, *depthPrev = nullptr;
    ID3D12Resource *expectP[2] = {}, *expectKick = nullptr; // pending origin and K's lookup into the old residual
    ID3D12Resource *residualOld = nullptr;                  // 26.28 PW_T_RAMP
    ID3D12Resource *residualMix = nullptr;
    ID3D12Resource *cellsAdd = nullptr, *cellsLook = nullptr; // 26.28 PW_T_CELLS
    ID3D12Resource *modelMv = nullptr;                      // 26.28: vectors handed to the model on a full pass
    // DebugTemporalDumpAcc: why ModelMotion marked each texel (R8_UINT, PwTMarkerReason), created on first use.
    ID3D12Resource *markerReason = nullptr;
    D3D12_RESOURCE_STATES markerReasonState = D3D12_RESOURCE_STATE_COMMON;
    // DebugTemporalMarkerRule 3/4: every texel's base marker verdict (R8_UINT, MarkerFail), created on first use.
    ID3D12Resource *markerFail = nullptr;
    D3D12_RESOURCE_STATES markerFailState = D3D12_RESOURCE_STATE_COMMON;
    // DebugTemporalChainRule 3: every texel's rule-2 link verdict (R8_UINT, LinkFail), created on first use.
    ID3D12Resource *linkFail = nullptr;
    D3D12_RESOURCE_STATES linkFailState = D3D12_RESOURCE_STATE_COMMON;
    // DebugTemporalCarryChainRule 1..6: the carry chain the reprojection reads (carry_chain.cpp), a ping-pong pair like
    // acc, created on first use; accCarry[accCarryCurrent] holds the latest.
    ID3D12Resource *accCarry[2] = {};
    D3D12_RESOURCE_STATES accCarryState[2] = {D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON};
    int accCarryCurrent = 0;
    // DebugTemporalCarryChainRule 6: a copy of last frame's unrefined main chain (carry_chain.cpp), which AccumulatePair
    // reads around each texel while it overwrites the original. Created on first use.
    ID3D12Resource *accUnrefined = nullptr;
    D3D12_RESOURCE_STATES accUnrefinedState = D3D12_RESOURCE_STATE_COMMON;
    bool carryChainFailed = false; // a texture of the carry chain could not be created: not retried until these Resources go

    // The sub-rects the depth snapshots were copied with (their frames' depthRect; w = 0 before the first copy). A
    // host with dynamic resolution moves the depth's sub-rect between frames; the passes read a snapshot through its
    // own (PwTDepthFRect), not through the current frame's.
    Rect depthFRect{}, depthPrevRect{};
    D3D12_RESOURCE_STATES residualState = D3D12_RESOURCE_STATE_COMMON, residualPrevState = D3D12_RESOURCE_STATE_COMMON,
                          depthFState = D3D12_RESOURCE_STATE_COMMON, depthPrevState = D3D12_RESOURCE_STATE_COMMON,
                          accState[2] = {D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON},
                          accPState[2] = {D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON},
                          expectState[2] = {D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON},
                          expectPState[2] = {D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON},
                          expectKickState = D3D12_RESOURCE_STATE_COMMON,
                          interpState = D3D12_RESOURCE_STATE_COMMON, colorFState = D3D12_RESOURCE_STATE_COMMON,
                          residualLowState = D3D12_RESOURCE_STATE_COMMON, toneAccState = D3D12_RESOURCE_STATE_COMMON,
                          residualOldState = D3D12_RESOURCE_STATE_COMMON, cellsAddState = D3D12_RESOURCE_STATE_COMMON,
                          residualMixState = D3D12_RESOURCE_STATE_COMMON,
                          cellsLookState = D3D12_RESOURCE_STATE_COMMON, modelMvState = D3D12_RESOURCE_STATE_COMMON;

    int residualTarget = kTargetResidualA, residualPrevTarget = kTargetResidualB;
    int accTarget[2] = {kTargetAcc0, kTargetAcc1}, accPTarget[2] = {kTargetAccPending0, kTargetAccPending1};
    int accCurrent = 0;    // acc[accCurrent] holds the latest accumulation
    int accPCurrent = 0;
    int expectCurrent = 0; // expect[expectCurrent] holds the expectation of the frame last accumulated
    int expectPCurrent = 0;
    int expectTarget[2] = {kTargetExpect0, kTargetExpect1}, expectPTarget[2] = {kTargetExpectPending0, kTargetExpectPending1};
    bool residualEverWritten = false;
    PhaseIn phase;
    History history;
    SkyField sky;
    StatsReader stats;
    PassTimer timer;

    gpu::DescriptorPool descriptors, fallbackDescriptors;
    OfpsFencePoint usePoint{};
    std::uint64_t evalNow = 0;
    bool exhausted = false;
    DXGI_FORMAT colorFFormat = DXGI_FORMAT_UNKNOWN;
    // The view the snapshot was taken with. The snapshot keeps the format of its source, which may be
    // the host's colour or the unpacked base (output format); a later frame's colour view need not fit.
    DXGI_FORMAT colorFView = DXGI_FORMAT_UNKNOWN;
    std::uint32_t colorFW = 0, colorFH = 0;
    std::uint32_t nativeW = 0, nativeH = 0, motionW = 0, motionH = 0, depthW = 0, depthH = 0, lowW = 0, lowH = 0;
    std::uint32_t historyPictureDivisor = 3;
    MachineRole role = MachineRole::Synchronous; // decides the optional textures above (machine_role.h)
    DXGI_FORMAT outputFormat = DXGI_FORMAT_UNKNOWN, outputView = DXGI_FORMAT_UNKNOWN, depthFormat = DXGI_FORMAT_UNKNOWN;
    // DebugTemporalDumpAcc (acc_dump_copies.cpp): the readback of modelMv, the chain and markerReason, created on first
    // use; the fence point of the frame it was recorded on (fence AddRef'd, null = an evaluate count as in OfpsFencePoint).
    struct AccDump {
        ID3D12Resource *buffer = nullptr;
        ID3D12Fence *fence = nullptr;
        std::uint64_t value = 0;
        AccDumpTags tags;
        bool pending = false;
    } accDump;

    ~Resources();

    // Builds everything; `error` receives the reason on failure.
    bool Create(ID3D12Device *device, const ofps::core::gpu::Shaders &shaders, std::uint32_t nativeWidth, std::uint32_t nativeHeight,
                DXGI_FORMAT outputFormat, DXGI_FORMAT outputView, std::uint32_t motionWidth, std::uint32_t motionHeight,
                DXGI_FORMAT depthFormat, std::uint32_t depthWidth, std::uint32_t depthHeight, char *error, std::size_t errorSize,
                std::uint32_t pictureDivisor, MachineRole role);

    Output Target(int index) const { return targets[index]; }
    // A fresh table for every dispatch from a ring deep enough that the GPU has long finished with the
    // entry being rewritten (a keyed cache was rewritten under in-flight frames once the host rotated
    // its input textures every frame: garbage/black frames).
    D3D12_GPU_DESCRIPTOR_HANDLE Table(const SlotKey &key, Output output, const Output *second, bool fallback = false);
    void Dispatch(ID3D12GraphicsCommandList *cmd, Pass pass, Output output, std::uint32_t w,
              std::uint32_t h, const SlotKey &key, const Constants &constants,
              const Output *second = nullptr, FlowExtent flow = {});

    // The inputs as the passes see them: with a base colour, it stands in for the host colour.
    FrameInputs Resolve(const FrameInputs &raw) const;
    // PwTDepthFRect is depthFRect (t6 = the residual frame's snapshot); a pass binding another depth at t6 overrides it
    // with SetDepthFRect.
    Constants BaseConstants(const FrameInputs &in) const;
    static void SetDepthFRect(Constants &c, const Rect &copied, const FrameInputs &in);
    // The bindings every pass shares (t0..t11); each pass overrides what it reads differently.
    SlotKey BaseKey(const FrameInputs &in, ID3D12Resource *accPrev) const;
    // Makes sure `colorF` matches the host colour's description; false when it could not be created.
    bool EnsureColorSnapshot(const FrameInputs &in);
    // Creates `texture` (markerReason, markerFail, linkFail) on first use: motion-texture size, R8_UINT; false when it cannot.
    bool EnsureMotionByte(ID3D12Resource *&texture);
    // Before ModelMotion / MarkerReason with this key and these constants (the rule in params[2]): t1 becomes the
    // fail mask's view, and under rules 3/4 MarkerFail fills the mask and params[0] = 1 tells the pass to read it.
    // Without the pass or its texture the pass tests every neighbour inline (the same decisions).
    void RecordMarkerFail(ID3D12GraphicsCommandList *cmd, SlotKey &key, Constants &c);
    // Before a chain's Accumulate (the main one or the carry chain, carry_chain.cpp) with this key and these constants
    // (its rule in sky[2]): t1 becomes the LinkFail mask's view, and under rule 3, where the links are tested, LinkFail
    // fills the mask (one texture: each chain fills it right before its own Accumulate reads it). Without the pass or its
    // texture rule 3 runs as rule 2 (sky[2] = 2, logged once).
    void RecordLinkFail(ID3D12GraphicsCommandList *cmd, SlotKey &key, Constants &c);
    // Creates the carry chain's pair (accCarry) on first use, and with `unrefined` accUnrefined; false when it cannot (and
    // from then on, without trying again: carryChainFailed).
    bool EnsureCarryChain(bool unrefined);
};

} // namespace ofps::core::temporal
