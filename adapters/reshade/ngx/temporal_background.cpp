#include "ngx_temporal.h"

#include "temporal_resources.h"

namespace pwtemporal {
namespace {

using pwngx::Barrier;
using pwngx::BarrierExternal;
constexpr DXGI_FORMAT kExpectFormat = DXGI_FORMAT_R32G32B32A32_FLOAT;

} // namespace

void Machine::RecordBackgroundAccumulate(ID3D12GraphicsCommandList *cmd, const FrameInputs &in, bool pending)
{
    if (hasResidual_) RecordAccumulate(cmd, in);
    if (pending && !pendingMirror_) RecordAccumulatePending(cmd, in);
    // Both expectations read the same previous host frame. On K+1 this is precisely the depth
    // copied into depthBg at K, so the pending chain's first link needs no cross-queue read of depthBg.
    if (in.expectedDepth) RecordDepthPrev(cmd, in);
}

void Machine::RecordBackgroundKick(ID3D12GraphicsCommandList *cmd, const FrameInputs &rawIn, bool pending,
                                   ID3D12Resource *modelVectors, D3D12_RESOURCE_STATES vectorState,
                                   ID3D12Resource *kickColor, ID3D12Resource *kickDepth, bool validate)
{
    Resources &m = *res_;
    const FrameInputs in = m.Resolve(rawIn);
    // The residual adopted at T is measured at K. Save K's lookup into the still-displayed residual;
    // the main expectation will keep advancing to T while the model runs.
    if (m.expectKick && hasResidual_) {
        Barrier(cmd, m.expect[m.expectCurrent], m.expectState[m.expectCurrent], D3D12_RESOURCE_STATE_COPY_SOURCE);
        Barrier(cmd, m.expectKick, m.expectKickState, D3D12_RESOURCE_STATE_COPY_DEST);
        cmd->CopyResource(m.expectKick, m.expect[m.expectCurrent]);
        Barrier(cmd, m.expectKick, m.expectKickState, kReadable);
        Barrier(cmd, m.expect[m.expectCurrent], m.expectState[m.expectCurrent], kReadable);
    }
    if (modelVectors == nullptr) return;
    if (!validate || m.modelMotionPso == nullptr) {
        RecordCopyAcc(cmd, pending, modelVectors, vectorState);
        return;
    }

    ID3D12Resource *chain = pending ? m.accP[m.accPCurrent] : m.acc[m.accCurrent];
    auto &chainState = pending ? m.accPState[m.accPCurrent] : m.accState[m.accCurrent];
    Barrier(cmd, chain, chainState, kReadable);
    BarrierExternal(cmd, in.color, in.colorState, kReadable);
    BarrierExternal(cmd, in.depth, in.depthState, kReadable, in.depthSubresource);
    SlotKey key = m.BaseKey(in, chain);
    // These still hold the previous model kick, even when the pending chain mirrors the main one.
    // colorF may hold the unpacked base instead; the model's test must compare raw host guides.
    BarrierExternal(cmd, kickColor, vectorState, kReadable);
    BarrierExternal(cmd, kickDepth, vectorState, kReadable, in.depthSubresource);
    key.res[6] = kickDepth;
    key.res[7] = kickColor;
    if (pending) {
        if (m.expectP[m.expectPCurrent]) {
            key.res[11] = m.expectP[m.expectPCurrent]; key.fmt[11] = kExpectFormat;
            Barrier(cmd, m.expectP[m.expectPCurrent], m.expectPState[m.expectPCurrent], kReadable);
        }
    }
    Barrier(cmd, m.modelMv, m.modelMvState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Constants c = m.BaseConstants(in);
    m.Dispatch(cmd, kModelMotion, m.Target(kTargetModelMv), m.motionW, m.motionH, key, c);
    // Only this private copy is consumed on the background queue. The host remains free to reuse
    // its model-motion target and descriptor ring on subsequent frames.
    Barrier(cmd, m.modelMv, m.modelMvState, D3D12_RESOURCE_STATE_COPY_SOURCE);
    BarrierExternal(cmd, modelVectors, vectorState, D3D12_RESOURCE_STATE_COPY_DEST);
    cmd->CopyResource(modelVectors, m.modelMv);
    BarrierExternal(cmd, modelVectors, D3D12_RESOURCE_STATE_COPY_DEST, vectorState);
    Barrier(cmd, m.modelMv, m.modelMvState, kAccState);
    Barrier(cmd, chain, chainState, kAccState);
    BarrierExternal(cmd, kickColor, kReadable, vectorState);
    BarrierExternal(cmd, kickDepth, kReadable, vectorState, in.depthSubresource);
    BarrierExternal(cmd, in.color, kReadable, in.colorState);
    BarrierExternal(cmd, in.depth, kReadable, in.depthState, in.depthSubresource);
}

} // namespace pwtemporal
