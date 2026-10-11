#pragma once

// A Background machine (TemporalMode 3) driven on D3D12 WARP the way core/temporal/async_scheduler.cpp drives it, one
// frame at a time: accumulation of both chains, the adoption of the kick in flight (its residual, measured on the
// kick's frame, through the kick's private copies as the kick described them: UseKickDepth), a kick every `every`
// frames with private copies of colour, depth and the chain, then the carried frame. The core-API route is not used:
// there the adoption frame depends on when the background queue finishes, so two runs are not frame-aligned. Used by
// test_temporal_sky_background.cpp and test_temporal_depth_rect.cpp.
#include "temporal_d3d12_fixture.h"
#include "core/temporal/machine.h"

namespace temporal_d3d12 {

using ofps::core::temporal::FrameInputs;
using ofps::core::temporal::Machine;
using ofps::core::temporal::Rect;

inline ComPtr<ID3D12Resource> Plain(ID3D12Device *device, std::uint32_t width, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags)
{
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width; desc.Height = kH; desc.DepthOrArraySize = 1; desc.MipLevels = 1;
    desc.Format = format; desc.SampleDesc.Count = 1; desc.Flags = flags;
    ComPtr<ID3D12Resource> texture;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                               IID_PPV_ARGS(&texture))))
        texture.Reset();
    return texture;
}

// Whole-resource copy between two textures that rest readable.
inline void Copy(ID3D12GraphicsCommandList *list, ID3D12Resource *from, ID3D12Resource *to)
{
    BarrierExternal(list, from, kRead, D3D12_RESOURCE_STATE_COPY_SOURCE);
    BarrierExternal(list, to, kRead, D3D12_RESOURCE_STATE_COPY_DEST);
    list->CopyResource(to, from);
    BarrierExternal(list, to, D3D12_RESOURCE_STATE_COPY_DEST, kRead);
    BarrierExternal(list, from, D3D12_RESOURCE_STATE_COPY_SOURCE, kRead);
}

// The private copies of a kick (AsyncJob's colorBg, depthBg, accBg, mvBg), all resting readable, and how the kick's
// frame described its depth (the scheduler's frameSnapshot and depthBgRect).
struct Job {
    ComPtr<ID3D12Resource> color, depth, chain, motion;
    bool inflight = false, chainIsAcc = false;
    int sinceKick = 1 << 20, kicks = 0;
    Rect kickRect{};
    bool kickInverted = false;
    // Optional, as the scheduler does when the chain is copied (mvIsAcc): per kick (index = the kick's number) a
    // texture of the motion size that receives the validated model vectors (R16G16_FLOAT, resting readable).
    ID3D12Resource *const *modelVectors = nullptr;
    int discardKick = -1; // the kick whose result is dropped at adoption, as after a host reset in flight
    unsigned vectorKicks = 0; // the kicks (bits) that validated model vectors
    bool validate = true;     // RecordBackgroundKick's validate (false: the copied chain itself, TemporalModelMarker=0)

    bool Create(ID3D12Device *device, ID3D12GraphicsCommandList *list, std::uint32_t depthWidth)
    {
        color = Plain(device, kW, kColor, D3D12_RESOURCE_FLAG_NONE);
        depth = Plain(device, depthWidth, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_NONE);
        chain = Plain(device, kW, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_NONE);
        motion = Plain(device, kW, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_NONE);
        for (ID3D12Resource *copy : {color.Get(), depth.Get(), chain.Get(), motion.Get()})
            if (copy) BarrierExternal(list, copy, D3D12_RESOURCE_STATE_COMMON, kRead);
        return color && depth && chain && motion;
    }
};

// One frame. `in` is the frame as the host gives it (colour, depth and motion with their descriptions, background
// set); the adoption takes the kick's copies and `fresh[(kicks - 1) % 2]` as the model's answer, or drops them for
// job.discardKick (the scheduler's discard: ResetPending, no residual). kickDepth false reads the kick's depth copy as
// the adoption frame describes its own depth (the bug of 83ad206). The carried frame goes to `output` once there is a
// residual.
inline void RecordBackgroundFrame(Machine &machine, ID3D12GraphicsCommandList *list, Job &job, const FrameInputs &in,
                                  ID3D12Resource *const (&fresh)[2], ID3D12Resource *output, int every, bool kickDepth = true)
{
    machine.RecordBackgroundAccumulate(list, in, job.kicks > 0 && (job.inflight || machine.HasResidual()));
    if (job.inflight && job.kicks - 1 == job.discardKick) { // a discarded pass: nothing adopted, the pending chain restarts
        machine.ResetPending();
        job.inflight = false;
    }
    if (job.inflight) { // adoption: the residual of the kick's frame
        FrameInputs bg = in;
        bg.color = job.color.Get();
        bg.motion = job.chainIsAcc ? job.chain.Get() : job.motion.Get();
        if (kickDepth) {
            OfpsResource kick{};
            kick.res = job.depth.Get(); kick.view = in.depthView;
            kick.rect = {job.kickRect.x, job.kickRect.y, job.kickRect.w, job.kickRect.h};
            ofps::core::temporal::UseKickDepth(bg, job.depth.Get(), kick, job.kickInverted);
        } else {
            bg.depth = job.depth.Get();
        }
        bg.residualBlend = job.chainIsAcc ? 0.6f : 0.0f;
        bg.blendFromMotion = true;
        bg.motionIsPassChain = job.chainIsAcc;
        if (job.chainIsAcc) { bg.mvScaleX = bg.mvScaleY = 1.0f; bg.motionSign = 1.0f; }
        machine.RecordResidual(list, bg, fresh[(job.kicks - 1) % 2], kRead);
        machine.PromotePending();
        job.inflight = false;
    }
    if (!job.inflight && job.sinceKick >= every) { // kick: the private copies, as the scheduler takes them
        if (machine.PendingMirrorsAcc() && machine.AccValid()) { machine.RecordCopyAcc(list, false, job.chain.Get(), kRead); job.chainIsAcc = true; }
        else if (machine.PendingValid()) { machine.RecordCopyAcc(list, true, job.chain.Get(), kRead); job.chainIsAcc = true; }
        else { Copy(list, in.motion, job.motion.Get()); job.chainIsAcc = false; }
        ID3D12Resource *vectors = job.modelVectors && job.chainIsAcc ? job.modelVectors[job.kicks] : nullptr;
        if (vectors) job.vectorKicks |= 1u << job.kicks;
        machine.RecordBackgroundKick(list, in, !machine.PendingMirrorsAcc(), vectors, kRead, job.color.Get(), job.depth.Get(),
                                     job.kickRect, job.validate);
        Copy(list, in.color, job.color.Get());
        Copy(list, in.depth, job.depth.Get());
        job.kickRect = in.depthRect;
        job.kickInverted = in.depthInverted;
        job.inflight = true;
        job.sinceKick = 0;
        ++job.kicks;
        machine.ResetPending();
    }
    ++job.sinceKick;
    if (machine.HasResidual()) machine.RecordReproject(list, in, output, D3D12_RESOURCE_STATE_COMMON, 0, 0);
}

} // namespace temporal_d3d12
