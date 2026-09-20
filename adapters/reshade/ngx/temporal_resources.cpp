#include "temporal_resources.h"

#include <cstdio>

namespace pwtemporal {
namespace {

constexpr std::uint32_t kTableSize = kTableUsed + 2;

void SetRect(float (&out)[4], const Rect &r)
{
    out[0] = static_cast<float>(r.x);
    out[1] = static_cast<float>(r.y);
    out[2] = static_cast<float>(r.w);
    out[3] = static_cast<float>(r.h);
}

constexpr DXGI_FORMAT kResidualFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
constexpr DXGI_FORMAT kChainFormat = DXGI_FORMAT_R16G16_FLOAT;
constexpr DXGI_FORMAT kExpectFormat = DXGI_FORMAT_R32G32B32A32_FLOAT;

} // namespace

Resources::~Resources()
{
    for (ID3D12Resource *r : {residual, residualPrev, depthF, depthPrev, acc[0], acc[1], interp, colorF, residualLow,
                              accP[0], accP[1], toneAcc, expect[0], expect[1], expectP[0], expectP[1], expectKick,
                              residualOld, residualMix, cellsAdd, cellsLook, modelMv})
        if (r) r->Release();
    if (srvHeap) srvHeap->Release();
#define PW_TEMPORAL_PASS(name, member, reads, outputs, extent) if (member##Pso) member##Pso->Release();
#include "../../../shaders/temporal_passes.def"
#undef PW_TEMPORAL_PASS
    if (rootSignature) rootSignature->Release();
    if (device) device->Release();
}

D3D12_GPU_DESCRIPTOR_HANDLE Resources::Table(const SlotKey &key, Output output, const Output *second)
{
    const std::uint32_t slot = nextSlot;
    nextSlot = (nextSlot + 1) % kTableSlots;
    D3D12_CPU_DESCRIPTOR_HANDLE h = srvHeap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(slot) * kTableSize * srvIncrement;
    for (int i = 0; i < kTableUsed; ++i) {
        D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
        const bool set = key.res[i] != nullptr || key.fmt[i] != DXGI_FORMAT_UNKNOWN;
        desc.Format = set ? key.fmt[i] : kResidualFormat;
        desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        desc.Texture2D.MipLevels = 1;
        // An explicit format with no resource requests a zero-valued null SRV (residual-only Apply).
        device->CreateShaderResourceView(key.res[i], &desc, h);
        h.ptr += srvIncrement;
    }
    const Output outputs[2] = {output, second ? *second : Output{nullptr, output.format}};
    for (const auto &view : outputs) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC desc{};
        desc.Format = view.format;
        desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(view.resource, nullptr, &desc, h);
        h.ptr += srvIncrement;
    }
    D3D12_GPU_DESCRIPTOR_HANDLE g = srvHeap->GetGPUDescriptorHandleForHeapStart();
    g.ptr += static_cast<UINT64>(slot) * kTableSize * srvIncrement;
    return g;
}

void Resources::Dispatch(ID3D12GraphicsCommandList *cmd, Pass pass, Output output,
                         std::uint32_t w, std::uint32_t h, const SlotKey &key, const Constants &constants,
                         const Output *second)
{
    // These passes record into the game's command list. Graphics draws left state behind and
    // crashed hosts in the fork; compute avoids touching the host's graphics bindings.
    ID3D12DescriptorHeap *heaps[] = {srvHeap};
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->SetComputeRootSignature(rootSignature);
    ID3D12PipelineState *pso = nullptr;
    switch (pass) {
#define PW_TEMPORAL_PASS(name, member, reads, outputs, extent) case k##name: pso = member##Pso; break;
#include "../../../shaders/temporal_passes.def"
#undef PW_TEMPORAL_PASS
    default: return;
    }
    cmd->SetPipelineState(pso);
    cmd->SetComputeRoot32BitConstants(0, kConstantDwords, &constants, 0);
    SlotKey bound = key;
    for (unsigned i = 0; i < kTableUsed; ++i)
        if ((passes[pass].reads & (1u << i)) == 0) bound.res[i] = nullptr;
    cmd->SetComputeRootDescriptorTable(1, Table(bound, output, passes[pass].outputs > 1 ? second : nullptr));
    const auto size = DispatchSize(pass, {nativeW, nativeH}, {motionW, motionH}, {lowW, lowH}, {w, h});
    w = size.width; h = size.height;
    const UINT dispatch[8] = {w, h, 0, 0, 0, 0, 0, 0};
    cmd->SetComputeRoot32BitConstants(2, 8, dispatch, 0);
    cmd->Dispatch((w + PW_TEMPORAL_THREADS_X - 1) / PW_TEMPORAL_THREADS_X, (h + PW_TEMPORAL_THREADS_Y - 1) / PW_TEMPORAL_THREADS_Y, 1);
    // Also orders consecutive dispatches writing interp without an intervening transition.
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    cmd->ResourceBarrier(1, &barrier);
}

FrameInputs Resources::Resolve(const FrameInputs &raw) const
{
    FrameInputs in = raw;
    if (raw.base) {
        in.color = raw.base;
        in.colorView = outputView;
        in.colorRect = Rect{0, 0, nativeW, nativeH};
        in.colorState = raw.baseState;
    } else {
        in.colorState = raw.hostInputState;
    }
    return in;
}

Constants Resources::BaseConstants(const FrameInputs &in) const
{
    Constants c{};
    c.native[0] = static_cast<float>(nativeW);
    c.native[1] = static_cast<float>(nativeH);
    c.native[2] = 1.0f / static_cast<float>(nativeW);
    c.native[3] = 1.0f / static_cast<float>(nativeH);
    SetRect(c.colorRect, in.colorRect);
    SetRect(c.motionRect, in.motionRect);
    SetRect(c.depthRect, in.depthRect);
    c.motionTex[0] = static_cast<float>(motionW);
    c.motionTex[1] = static_cast<float>(motionH);
    c.motionTex[2] = in.mvScaleX;
    c.motionTex[3] = in.mvScaleY;
    // params[0..2] are per-pass and stay zero here: each record function sets what its pass reads.
    c.params[3] = in.depthThreshold > 0.0f ? in.depthThreshold : 1.0e9f;
    c.tune[0] = in.colorTolerance;
    c.tune[1] = in.motionSign == 0.0f ? 1.0f : in.motionSign;
    c.tune[2] = in.rawInterpolation ? 1.0f : 0.0f;
    c.tune[3] = 0.0f; // residual blend weight, set by RecordResidual
    c.fill[0] = (in.holeFill && residualLow != nullptr) ? 1.0f : 0.0f;
    c.fill[1] = static_cast<float>(lowW);
    c.fill[2] = static_cast<float>(lowH);
    c.fill[3] = in.mvSearchRadiusPx > 1.5f ? in.mvSearchRadiusPx : 1.0f; // PSReproject fetches the chain with depth guidance
    c.smoothing[0] = 0.0f; // tap radius scale of the acceptance average (0 = 1)
    c.smoothing[1] = in.smoothRadius;
    c.smoothing[2] = 0.0f; // the add-on's frames are display-referred: no ratio domain
    c.smoothing[3] = phase.Share();
    return c;
}

SlotKey Resources::BaseKey(const FrameInputs &in, ID3D12Resource *accPrev) const
{
    SlotKey key;
    key.valid = true;
    key.res[0] = in.color; key.fmt[0] = in.colorView;
    key.res[2] = residual; key.fmt[2] = kResidualFormat;
    key.res[3] = in.motion; key.fmt[3] = in.motionView;
    key.res[4] = accPrev; key.fmt[4] = kChainFormat;
    key.res[5] = in.depth; key.fmt[5] = in.depthView;
    key.res[6] = depthF; key.fmt[6] = in.depthView;
    key.res[7] = colorF ? colorF : in.color; key.fmt[7] = in.colorView;
    key.res[8] = residualLow ? residualLow : residual; key.fmt[8] = kResidualFormat;
    // t1, t9, t10 are per pass; t11 is the expectation whenever there is one.
    if (expect[expectCurrent]) { key.res[11] = expect[expectCurrent]; key.fmt[11] = kExpectFormat; }
    return key;
}

bool Resources::EnsureColorSnapshot(const FrameInputs &in)
{
    const D3D12_RESOURCE_DESC cd = in.color->GetDesc();
    if (colorF != nullptr && colorFFormat == cd.Format && colorFW == static_cast<std::uint32_t>(cd.Width) && colorFH == cd.Height)
        return true;
    if (colorF) { colorF->Release(); colorF = nullptr; }
    if (!pwngx::CreateTexture(device, static_cast<std::uint32_t>(cd.Width), cd.Height, cd.Format, D3D12_RESOURCE_FLAG_NONE, &colorF))
        return false;
    colorFFormat = cd.Format;
    colorFW = static_cast<std::uint32_t>(cd.Width);
    colorFH = cd.Height;
    colorFState = D3D12_RESOURCE_STATE_COMMON;
    return true;
}

} // namespace pwtemporal
