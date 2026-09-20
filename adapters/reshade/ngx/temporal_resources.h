#pragma once

// What the temporal machine owns on the GPU: the root signature and the pipeline per pass, the
// descriptor-table ring and every texture the passes read or write, plus the two small
// helpers that turn a frame's inputs into root constants and into a descriptor table.
//
// ngx_temporal.cpp records the passes; nothing here knows the order they run in.

#include "ngx_common.h"
#include "ngx_temporal.h"
#include "temporal_phase.h"
#include "temporal_history.h"
#include "../../../shaders/temporal_layout.h"

#include <cstdint>

namespace pwtemporal {

// Shared t0..t19, u0/u1 and b0/b1 contract. The add-on reserves t9 for optional optical flow.
using namespace pwtemporalcontract;
constexpr int kTableUsed = PW_TEMPORAL_SRV_COUNT;
constexpr std::uint32_t kTableSlots = 256; // fresh tables for both chains, adoption and kick, with queue headroom
constexpr std::uint32_t kLowDivisor = 16; // low-res residual and cell grid: native / 16 per axis
constexpr D3D12_RESOURCE_STATES kReadable = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
// The accumulated displacement is also read by the model, which is a compute shader.
constexpr D3D12_RESOURCE_STATES kAccState = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;

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

// Mirrors cbuffer PwTemporalConstants in shaders/temporal.hlsl: 9 float4 of root constants. The
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
};
static_assert(sizeof(Constants) == 36 * sizeof(float));
constexpr UINT kConstantDwords = 36;

struct SlotKey {
    ID3D12Resource *res[kTableUsed] = {};
    DXGI_FORMAT fmt[kTableUsed] = {};
    bool valid = false;
};

struct Resources {
    ID3D12Device *device = nullptr;
    ID3D12RootSignature *rootSignature = nullptr;
    // The complete pass set is required: partial shader installs fail explicitly.
#define PW_TEMPORAL_PASS(name, member, reads, outputs, extent) ID3D12PipelineState *member##Pso = nullptr;
#include "../../../shaders/temporal_passes.def"
#undef PW_TEMPORAL_PASS
    ID3D12DescriptorHeap *srvHeap = nullptr;
    UINT srvIncrement = 0;
    struct Output { ID3D12Resource *resource = nullptr; DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN; };
    Output targets[kTargetCount]{};

    ID3D12Resource *residual = nullptr, *depthF = nullptr, *acc[2] = {}, *interp = nullptr;
    ID3D12Resource *residualPrev = nullptr; // the previous full pass's residual (ping-pong with `residual`)
    // 26.6.X: the reprojection's addition + acceptance (RGBA16F native), smoothed by the compose pass.
    ID3D12Resource *toneAcc = nullptr;
    ID3D12Resource *accP[2] = {}; // pending chain (background mode)
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

    std::uint32_t nextSlot = 0; // the descriptor-table ring
    DXGI_FORMAT colorFFormat = DXGI_FORMAT_UNKNOWN;
    std::uint32_t colorFW = 0, colorFH = 0;
    std::uint32_t nativeW = 0, nativeH = 0, motionW = 0, motionH = 0, depthW = 0, depthH = 0, lowW = 0, lowH = 0;
    DXGI_FORMAT outputFormat = DXGI_FORMAT_UNKNOWN, outputView = DXGI_FORMAT_UNKNOWN, depthFormat = DXGI_FORMAT_UNKNOWN;

    ~Resources();

    // Builds everything; `error` receives the reason on failure.
    bool Create(ID3D12Device *device, const pwngx::Shaders &shaders, std::uint32_t nativeWidth, std::uint32_t nativeHeight,
                DXGI_FORMAT outputFormat, DXGI_FORMAT outputView, std::uint32_t motionWidth, std::uint32_t motionHeight,
                DXGI_FORMAT depthFormat, std::uint32_t depthWidth, std::uint32_t depthHeight, char *error, std::size_t errorSize);

    Output Target(int index) const { return targets[index]; }
    // A fresh table for every dispatch from a ring deep enough that the GPU has long finished with the
    // entry being rewritten (a keyed cache was rewritten under in-flight frames once the host rotated
    // its input textures every frame: garbage/black frames).
    D3D12_GPU_DESCRIPTOR_HANDLE Table(const SlotKey &key, Output output, const Output *second);
    void Dispatch(ID3D12GraphicsCommandList *cmd, Pass pass, Output output, std::uint32_t w,
              std::uint32_t h, const SlotKey &key, const Constants &constants,
              const Output *second = nullptr);

    // The inputs as the passes see them: with a base colour, it stands in for the host colour.
    FrameInputs Resolve(const FrameInputs &raw) const;
    Constants BaseConstants(const FrameInputs &in) const;
    // The bindings every pass shares (t0..t11); each pass overrides what it reads differently.
    SlotKey BaseKey(const FrameInputs &in, ID3D12Resource *accPrev) const;
    // Makes sure `colorF` matches the host colour's description; false when it could not be created.
    bool EnsureColorSnapshot(const FrameInputs &in);
};

} // namespace pwtemporal
