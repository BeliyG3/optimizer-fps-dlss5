#include "core/temporal/machine.h"

#include "core/temporal/resources.h"

#include <algorithm>

namespace ofps::core::temporal {
namespace {

using ofps::core::gpu::Barrier;
using ofps::core::gpu::BarrierExternal;
constexpr DXGI_FORMAT kResidualFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

} // namespace

void Machine::RecordResidual(ID3D12GraphicsCommandList *cmd, const FrameInputs &rawIn, ID3D12Resource *fresh, D3D12_RESOURCE_STATES freshState, UINT freshSubresource)
{
    Resources &m = *res_;
    const FrameInputs in = m.Resolve(rawIn);
    const bool sharedColor = fresh == in.color &&
        (freshSubresource == in.colorSubresource || freshSubresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES ||
         in.colorSubresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES);
    // Cross-pass blend: the previous residual moved to this pass's frame along the chain (or the kick's
    // chain copy in `motion`), where the depth still matches.
    const bool blend = in.residualBlend > 0.0f && m.residualEverWritten && (in.blendFromMotion ? in.motion != nullptr : accValid_);
    // Ping-pong: what was the residual becomes the previous one (read), the other texture is written.
    std::swap(m.residual, m.residualPrev);
    std::swap(m.residualState, m.residualPrevState);
    std::swap(m.residualTarget, m.residualPrevTarget);
    BarrierExternal(cmd, in.color, in.colorState, kReadable, in.colorSubresource);
    if (!sharedColor) BarrierExternal(cmd, fresh, freshState, kReadable, freshSubresource);
    if (blend && in.blendFromMotion) BarrierExternal(cmd, in.motion, in.motionState, kReadable, in.motionSubresource);
    Barrier(cmd, m.residualPrev, m.residualPrevState, kReadable);
    if (blend) {
        BarrierExternal(cmd, in.depth, in.depthState, kReadable, in.depthSubresource);
        Barrier(cmd, m.depthF, m.depthFState, kReadable);
        if (!in.blendFromMotion) Barrier(cmd, m.acc[m.accCurrent], m.accState[m.accCurrent], kAccState);
    }
    Barrier(cmd, m.residual, m.residualState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    (void) m.EnsureColorSnapshot(in);
    ID3D12Resource *const chain = (blend && in.blendFromMotion) ? in.motion : m.acc[m.accCurrent];
    SlotKey key = m.BaseKey(in, chain);
    if (in.background && m.expectKick) {
        Barrier(cmd, m.expectKick, m.expectKickState, kReadable);
        key.res[11] = m.expectKick; key.fmt[11] = DXGI_FORMAT_R32G32B32A32_FLOAT;
    }
    key.res[1] = fresh; key.fmt[1] = m.outputView;
    key.res[2] = m.residualPrev; key.fmt[2] = kResidualFormat; // the previous residual (blend source)
    if (blend && m.colorF) Barrier(cmd, m.colorF, m.colorFState, kReadable); // the previous pass's colour snapshot gates the blend
    Constants rc = m.BaseConstants(in);
    rc.tune[3] = blend ? std::clamp(in.residualBlend, 0.0f, 0.9f) : 0.0f;
    if (m.colorF == nullptr) rc.tune[0] = 0.0f; // no snapshot yet: no colour gate
    m.Dispatch(cmd, kResidual, m.Target(m.residualTarget), m.nativeW, m.nativeH, key, rc);
    m.residualEverWritten = true;
    Barrier(cmd, m.residual, m.residualState, kReadable);

    // 26.28 PW_T_RAMP: the previous pass's residual moved onto THIS frame, while its chain and its
    // snapshots are still in place. The next few frames slide from it to the new residual, so the
    // model's new opinion of the whole picture fades in instead of clicking over in one frame.
    ID3D12Resource *phaseSource = m.residualPrev;
    if (blend && in.background && in.phaseInFrames > 0 && m.phase.Share() < 1.0f && m.applyPso) {
        // A quick adoption can interrupt the previous ramp. Freeze the mix that was last shown,
        // in that pass's coordinates, before residualOld is overwritten by the new alignment.
        SlotKey mix;
        mix.valid = true;
        mix.fmt[0] = kResidualFormat; // null SRV: Apply adds the mixture to zero
        mix.res[2] = m.residualPrev; mix.fmt[2] = kResidualFormat;
        mix.res[10] = m.residualOld; mix.fmt[10] = kResidualFormat;
        Barrier(cmd, m.residualOld, m.residualOldState, kReadable);
        Barrier(cmd, m.residualMix, m.residualMixState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        m.Dispatch(cmd, kApply, m.Target(kTargetResidualMix), m.nativeW, m.nativeH, mix, m.BaseConstants(in));
        Barrier(cmd, m.residualMix, m.residualMixState, kReadable);
        phaseSource = m.residualMix;
    }
    m.phase.Start(0, in.background);
    if (blend && in.phaseInFrames > 0 && m.residualOldPso != nullptr) {
        SlotKey older = key;
        older.res[1] = m.residual; older.fmt[1] = kResidualFormat; // t1 = the new residual
        older.res[2] = phaseSource; older.fmt[2] = kResidualFormat;
        Barrier(cmd, m.residualOld, m.residualOldState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Constants oc = m.BaseConstants(in);
        if (m.colorF == nullptr) oc.tune[0] = 0.0f;
        m.Dispatch(cmd, kResidualOld, m.Target(kTargetResidualOld), m.nativeW, m.nativeH, older, oc);
        Barrier(cmd, m.residualOld, m.residualOldState, kReadable);
        m.phase.Start(in.phaseInFrames, in.background);
    }

    // Preserve the replaced pass before its colour/depth snapshots are overwritten. In background
    // mode key contains the kick's chain and expectation, not the adoption frame's newer chain.
    m.history.Store(m, cmd, in, key, blend && in.history);
    if (blend) BarrierExternal(cmd, in.depth, kReadable, in.depthState, in.depthSubresource);
    if (blend && in.blendFromMotion) BarrierExternal(cmd, in.motion, kReadable, in.motionState, in.motionSubresource);
    if (m.residualLow) {
        // Box-filter the residual for the hole fill (t2 = residual, now readable; t8 must not alias the target).
        SlotKey low = key;
        low.res[2] = m.residual; low.fmt[2] = kResidualFormat;
        low.res[8] = fresh; low.fmt[8] = m.outputView;
        Barrier(cmd, m.residualLow, m.residualLowState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        m.Dispatch(cmd, kDownsample, m.Target(kTargetResidualLow), m.lowW, m.lowH, low, m.BaseConstants(in));
        Barrier(cmd, m.residualLow, m.residualLowState, kReadable);
    }
    if (!sharedColor) BarrierExternal(cmd, fresh, kReadable, freshState, freshSubresource);
    if (m.colorF) {
        // Colour snapshot of this frame: what the reprojection and the next pass's blend test against.
        BarrierExternal(cmd, in.color, kReadable, D3D12_RESOURCE_STATE_COPY_SOURCE, in.colorSubresource);
        Barrier(cmd, m.colorF, m.colorFState, D3D12_RESOURCE_STATE_COPY_DEST);
        if (in.colorSubresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES) {
            cmd->CopyResource(m.colorF, in.color);
        } else {
            D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
            dst.pResource = m.colorF; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            src.pResource = in.color; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            src.SubresourceIndex = in.colorSubresource;
            cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        }
        Barrier(cmd, m.colorF, m.colorFState, kReadable);
        BarrierExternal(cmd, in.color, D3D12_RESOURCE_STATE_COPY_SOURCE, kReadable, in.colorSubresource);
        m.colorFView = in.colorView; // the passes above read the previous snapshot with its own view
    }
    // Depth snapshot of this frame.
    BarrierExternal(cmd, in.color, kReadable, in.colorState, in.colorSubresource);
    BarrierExternal(cmd, in.depth, in.depthState, D3D12_RESOURCE_STATE_COPY_SOURCE, in.depthSubresource);
    Barrier(cmd, m.depthF, m.depthFState, D3D12_RESOURCE_STATE_COPY_DEST);
    CopyDepth(cmd, in, m.depthF);
    Barrier(cmd, m.depthF, m.depthFState, kReadable);
    if (m.depthPrev && in.expectedDepth && !in.background) {
        // The expectation restarts here too: the previous frame of the next link is this one. (Only
        // when the expectation runs: in the background mode `in.depth` is a copy of an older frame.)
        Barrier(cmd, m.depthPrev, m.depthPrevState, D3D12_RESOURCE_STATE_COPY_DEST);
        CopyDepth(cmd, in, m.depthPrev);
        Barrier(cmd, m.depthPrev, m.depthPrevState, kReadable);
    }
    BarrierExternal(cmd, in.depth, D3D12_RESOURCE_STATE_COPY_SOURCE, in.depthState, in.depthSubresource);
    hasResidual_ = true;
    accValid_ = false;  // the displacement restarts from this frame
    expectValid_ = false; // and so does the expectation
}

// A full pass shown as "the frame + the residual being phased in" instead of the model's raw answer
// (26.28, PSApply). Only recorded while a phase-in is running: with the full share it would only add
// the residual's half-float rounding to what the model already produced.
void Machine::RecordApply(ID3D12GraphicsCommandList *cmd, const FrameInputs &rawIn, ID3D12Resource *hostOutput,
                          D3D12_RESOURCE_STATES hostOutputState, std::uint32_t dstX, std::uint32_t dstY, UINT outputSubresource)
{
    Resources &m = *res_;
    if (rawIn.rawFull || !PhaseInActive() || hostOutput == nullptr) return;
    const FrameInputs in = m.Resolve(rawIn);
    BarrierExternal(cmd, in.color, in.colorState, kReadable, in.colorSubresource);
    Barrier(cmd, m.residual, m.residualState, kReadable);
    Barrier(cmd, m.residualOld, m.residualOldState, kReadable);
    Barrier(cmd, m.interp, m.interpState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    SlotKey key;
    key.valid = true;
    key.res[0] = in.color; key.fmt[0] = in.colorView;
    key.res[2] = m.residual; key.fmt[2] = kResidualFormat;
    key.res[10] = m.residualOld; key.fmt[10] = kResidualFormat;
    m.Dispatch(cmd, kApply, m.Target(kTargetInterp), m.nativeW, m.nativeH, key, m.BaseConstants(in));
    BarrierExternal(cmd, in.color, kReadable, in.colorState, in.colorSubresource);
    CopyInterp(cmd, hostOutput, hostOutputState, dstX, dstY, outputSubresource);
}

} // namespace ofps::core::temporal
