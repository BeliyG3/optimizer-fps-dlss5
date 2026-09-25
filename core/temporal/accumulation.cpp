#include "core/temporal/machine.h"

#include "core/temporal/resources.h"

#include <algorithm>

namespace ofps::core::temporal {
namespace {

using ofps::core::gpu::Barrier;
using ofps::core::gpu::BarrierExternal;

} // namespace

// ---------------------------------------------------------------------------------------------
// The expectation (26.28, PW_T_EXPECT). One link a frame, recorded before the accumulation reads it:
// the depth a surface had in the residual's frame travels with the chain, so the depth tests compare
// a surface with itself as it was THEN and not with itself as it is now. Without it a camera flying
// forward makes every surface fail its own depth test more the longer since the pass, and the whole
// frame pulses once per cadence.
// ---------------------------------------------------------------------------------------------
// The pass runs on EVERY frame, switched off included: t11 is bound by every later pass, and the
// only way to say "there is no expectation here" is the -2 the pass writes when PwTParams.x is 0.
// Skipping it would leave the readers on whatever the texture happened to hold.
void Machine::RecordExpect(ID3D12GraphicsCommandList *cmd, const FrameInputs &in, bool on, bool pending)
{
    Resources &m = *res_;
    auto &textures = pending ? m.expectP : m.expect;
    auto &states = pending ? m.expectPState : m.expectState;
    auto &outputs = pending ? m.expectPTarget : m.expectTarget;
    int &current = pending ? m.expectPCurrent : m.expectCurrent;
    bool &valid = pending ? expectPendingValid_ : expectValid_;
    const int next = 1 - current;
    if (on) Barrier(cmd, m.depthPrev, m.depthPrevState, kReadable);
    if (on && valid) Barrier(cmd, textures[current], states[current], kReadable);
    Barrier(cmd, textures[next], states[next], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    SlotKey key = m.BaseKey(in, m.acc[m.accCurrent]);
    key.res[11] = textures[current]; key.fmt[11] = DXGI_FORMAT_R32G32B32A32_FLOAT;
    key.res[6] = m.depthPrev; key.fmt[6] = in.depthView; // t6 here is the PREVIOUS frame's depth
    Constants c = m.BaseConstants(in);
    c.params[0] = on ? 1.0f : 0.0f;
    c.params[1] = (on && valid) ? 1.0f : 0.0f; // the previous frame's expectation is usable
    m.Dispatch(cmd, kExpect, m.Target(outputs[next]), m.motionW, m.motionH, key, c);
    Barrier(cmd, textures[next], states[next], kReadable);
    current = next;
    valid = on;
}

// This frame's depth becomes the one the next frame's link is checked against.
void Machine::RecordDepthPrev(ID3D12GraphicsCommandList *cmd, const FrameInputs &in)
{
    Resources &m = *res_;
    if (m.depthPrev == nullptr) return;
    BarrierExternal(cmd, in.depth, in.depthState, D3D12_RESOURCE_STATE_COPY_SOURCE, in.depthSubresource);
    Barrier(cmd, m.depthPrev, m.depthPrevState, D3D12_RESOURCE_STATE_COPY_DEST);
    CopyDepth(cmd, in, m.depthPrev);
    Barrier(cmd, m.depthPrev, m.depthPrevState, kReadable);
    BarrierExternal(cmd, in.depth, D3D12_RESOURCE_STATE_COPY_SOURCE, in.depthState, in.depthSubresource);
}

void Machine::CopyDepth(ID3D12GraphicsCommandList *cmd, const FrameInputs &in, ID3D12Resource *dst)
{
    if (in.depthSubresource != D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES) {
        // Planar depth-stencil: copy the depth plane only (CopyResource would need both planes in COPY_SOURCE).
        D3D12_TEXTURE_COPY_LOCATION dstP{}, srcP{};
        dstP.pResource = dst; dstP.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dstP.SubresourceIndex = 0;
        srcP.pResource = in.depth; srcP.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; srcP.SubresourceIndex = in.depthSubresource;
        cmd->CopyTextureRegion(&dstP, 0, 0, 0, &srcP, nullptr);
    } else {
        cmd->CopyResource(dst, in.depth);
    }
}

void Machine::RecordAccumulate(ID3D12GraphicsCommandList *cmd, const FrameInputs &rawIn)
{
    Resources &m = *res_;
    const FrameInputs in = m.Resolve(rawIn);
    RecordFlowSeed(cmd, in);
    const int prev = m.accCurrent;
    const int next = 1 - prev;
    // The chain's link test and the expectation both read the frame and the depth in a pixel shader.
    BarrierExternal(cmd, in.motion, in.motionState, kReadable, in.motionSubresource);
    BarrierExternal(cmd, in.color, in.colorState, kReadable, in.colorSubresource);
    BarrierExternal(cmd, in.depth, in.depthState, kReadable, in.depthSubresource);
    Barrier(cmd, m.depthF, m.depthFState, kReadable);
    if (m.colorF) Barrier(cmd, m.colorF, m.colorFState, kReadable);
    if (m.expectPso) {
        Barrier(cmd, m.acc[prev], m.accState[prev], kReadable);
        RecordExpect(cmd, in, in.expectedDepth);
    }
    if (accValid_) Barrier(cmd, m.acc[prev], m.accState[prev], kReadable);
    Barrier(cmd, m.acc[next], m.accState[next], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Constants c = m.BaseConstants(in);
    c.params[0] = 1.0f; // validate each link against the residual's frame (depth + colour); the pending chain has no stored frame to test against
    c.params[1] = accValid_ ? 1.0f : 0.0f;
    m.Dispatch(cmd, kAccumulate, m.Target(m.accTarget[next]), m.motionW, m.motionH, m.BaseKey(in, m.acc[prev]), c);
    Barrier(cmd, m.acc[next], m.accState[next], kAccState);
    // Match the fork: register the accumulated chain against the two real pictures. Keep the
    // correction in the chain so later links do not keep amplifying subpixel drift.
    m.accCurrent = next;
    if (m.colorF && in.refine) {
        Barrier(cmd, m.acc[prev], m.accState[prev], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        m.Dispatch(cmd, kRefine, m.Target(m.accTarget[prev]), m.motionW, m.motionH, m.BaseKey(in, m.acc[next]), c);
        Barrier(cmd, m.acc[prev], m.accState[prev], kAccState);
        m.accCurrent = prev;
    }
    BarrierExternal(cmd, in.color, kReadable, in.colorState, in.colorSubresource);
    BarrierExternal(cmd, in.motion, kReadable, in.motionState, in.motionSubresource);
    BarrierExternal(cmd, in.depth, kReadable, in.depthState, in.depthSubresource);
    if (m.expectPso && in.expectedDepth && !in.background) RecordDepthPrev(cmd, in);
    accValid_ = true;
}

void Machine::RecordAccumulatePending(ID3D12GraphicsCommandList *cmd, const FrameInputs &rawIn)
{
    Resources &m = *res_;
    const FrameInputs in = m.Resolve(rawIn);
    const int prev = m.accPCurrent;
    const int next = 1 - prev;
    BarrierExternal(cmd, in.motion, in.motionState, kReadable, in.motionSubresource);
    BarrierExternal(cmd, in.depth, in.depthState, kReadable, in.depthSubresource);
    if (m.expectPso && in.background) RecordExpect(cmd, in, in.expectedDepth, true);
    if (pendingValid_) Barrier(cmd, m.accP[prev], m.accPState[prev], kReadable);
    Barrier(cmd, m.accP[next], m.accPState[next], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Constants c = m.BaseConstants(in);
    c.params[1] = pendingValid_ ? 1.0f : 0.0f;
    SlotKey key = m.BaseKey(in, m.accP[prev]);
    if (m.expectP[m.expectPCurrent]) key.res[11] = m.expectP[m.expectPCurrent];
    m.Dispatch(cmd, kAccumulate, m.Target(m.accPTarget[next]), m.motionW, m.motionH, key, c);
    Barrier(cmd, m.accP[next], m.accPState[next], kAccState);
    BarrierExternal(cmd, in.motion, kReadable, in.motionState, in.motionSubresource);
    BarrierExternal(cmd, in.depth, kReadable, in.depthState, in.depthSubresource);
    m.accPCurrent = next;
    pendingValid_ = true;
}

// ---------------------------------------------------------------------------------------------
// The chain as the model's OWN motion vectors (26.28, PSModelMotion). The model keeps a history N
// frames old and finds it through these vectors; where the chain does not end on the pixel's own
// surface in the residual's frame the vector must leave the picture instead, or the model blends its
// old picture of the occluder into the wall he has left - a translucent copy beside every character.
// ---------------------------------------------------------------------------------------------
void Machine::RecordModelMotion(ID3D12GraphicsCommandList *cmd, const FrameInputs &rawIn)
{
    Resources &m = *res_;
    if (m.modelMotionPso == nullptr || !accValid_) return;
    const FrameInputs in = m.Resolve(rawIn);
    BarrierExternal(cmd, in.color, in.colorState, kReadable, in.colorSubresource);
    BarrierExternal(cmd, in.depth, in.depthState, kReadable, in.depthSubresource);
    Barrier(cmd, m.depthF, m.depthFState, kReadable);
    if (m.colorF) Barrier(cmd, m.colorF, m.colorFState, kReadable);
    Barrier(cmd, m.acc[m.accCurrent], m.accState[m.accCurrent], kReadable);
    Barrier(cmd, m.modelMv, m.modelMvState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Constants c = m.BaseConstants(in);
    if (m.colorF == nullptr) c.tune[0] = 0.0f; // no snapshot yet: no colour test
    m.Dispatch(cmd, kModelMotion, m.Target(kTargetModelMv), m.motionW, m.motionH, m.BaseKey(in, m.acc[m.accCurrent]), c);
    Barrier(cmd, m.modelMv, m.modelMvState, kAccState);
    Barrier(cmd, m.acc[m.accCurrent], m.accState[m.accCurrent], kAccState);
    BarrierExternal(cmd, in.color, kReadable, in.colorState, in.colorSubresource);
    BarrierExternal(cmd, in.depth, kReadable, in.depthState, in.depthSubresource);
}

} // namespace ofps::core::temporal
