// DebugTemporalDumpAcc: a full pass's model motion, chain and marker reasons (a carried frame's chain alone), and under
// a carry rule the chain the reprojection reads, copied whole
// to a readback buffer and handed over once the GPU has passed that frame (core/frame/acc_dump.h writes the file). A side
// copy: the textures go to COPY_SOURCE and back to the state they were in, nothing the frame reads or writes changes.
#include "core/temporal/machine.h"

#include "core/temporal/resources.h"

#include <cstring>

namespace ofps::core::temporal {
namespace {

using ofps::core::gpu::Barrier;

constexpr DXGI_FORMAT kChainFormat = DXGI_FORMAT_R16G16_FLOAT;
constexpr DXGI_FORMAT kReasonFormat = DXGI_FORMAT_R8_UINT;

// Model motion at 0, the chain at chainOffset (both RG16F rows of rowPitch), the reasons at reasonOffset (R8 rows of
// reasonPitch), the carry plane (OFAC v3) at carryOffset (RG16F rows of rowPitch).
struct Layout {
    UINT rowPitch, reasonPitch;
    UINT64 chainOffset, reasonOffset, carryOffset, size;
};

Layout LayoutFor(std::uint32_t w, std::uint32_t h)
{
    Layout l{};
    const UINT pitch = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
    l.rowPitch = (w * 4u + pitch - 1u) & ~(pitch - 1u);
    l.reasonPitch = (w + pitch - 1u) & ~(pitch - 1u);
    const UINT64 placement = D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
    l.chainOffset = (static_cast<UINT64>(l.rowPitch) * h + placement - 1u) & ~(placement - 1u);
    l.reasonOffset = l.chainOffset * 2u;
    l.carryOffset = (l.reasonOffset + static_cast<UINT64>(l.reasonPitch) * h + placement - 1u) & ~(placement - 1u);
    l.size = l.carryOffset + static_cast<UINT64>(l.rowPitch) * h;
    return l;
}

} // namespace

bool Machine::RecordAccDumpCopies(ID3D12GraphicsCommandList *cmd, const AccDumpTags &tags, bool *dropped)
{
    if (dropped) *dropped = false;
    if (res_ == nullptr || !accValid_ || (!tags.carried && (res_->modelMv == nullptr || res_->markerReason == nullptr))) return false;
    Resources &m = *res_;
    Resources::AccDump &d = m.accDump;
    const Layout l = LayoutFor(m.motionW, m.motionH);
    if (d.buffer == nullptr) {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = l.size;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(m.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                     IID_PPV_ARGS(&d.buffer))))
            return false;
    }
    if (dropped) *dropped = d.pending;
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = d.buffer;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    const auto copy = [&](ID3D12Resource *src, D3D12_RESOURCE_STATES &state, UINT64 offset, DXGI_FORMAT format, UINT rowPitch) {
        const D3D12_RESOURCE_STATES rest = state;
        dst.PlacedFootprint.Footprint = {format, m.motionW, m.motionH, 1, rowPitch};
        Barrier(cmd, src, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION s{};
        s.pResource = src;
        s.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        s.SubresourceIndex = 0;
        dst.PlacedFootprint.Offset = offset;
        cmd->CopyTextureRegion(&dst, 0, 0, 0, &s, nullptr);
        Barrier(cmd, src, state, rest);
    };
    // The main chain, the model's view, also on carried frames; DebugTemporalCarryChainRule's carry chain is the fourth plane.
    if (tags.carried) copy(m.acc[m.accCurrent], m.accState[m.accCurrent], 0, kChainFormat, l.rowPitch);
    else copy(m.modelMv, m.modelMvState, 0, kChainFormat, l.rowPitch);
    copy(m.acc[m.accCurrent], m.accState[m.accCurrent], l.chainOffset, kChainFormat, l.rowPitch);
    if (!tags.carried) copy(m.markerReason, m.markerReasonState, l.reasonOffset, kReasonFormat, l.reasonPitch);
    if (tags.carry) { // the main chain on a full pass, which records no carry chain
        if (carryValid_) copy(m.accCarry[m.accCarryCurrent], m.accCarryState[m.accCarryCurrent], l.carryOffset, kChainFormat, l.rowPitch);
        else copy(m.acc[m.accCurrent], m.accState[m.accCurrent], l.carryOffset, kChainFormat, l.rowPitch);
    }
    if (m.usePoint.fence) m.usePoint.fence->AddRef();
    if (d.fence) d.fence->Release();
    d.fence = m.usePoint.fence;
    d.value = m.usePoint.value;
    d.tags = tags;
    d.pending = true;
    return true;
}

int Machine::TakeAccDump(std::uint64_t evalNow, AccDumpHalves &out)
{
    if (res_ == nullptr || !res_->accDump.pending) return 0;
    Resources &m = *res_;
    Resources::AccDump &d = m.accDump;
    const UINT64 done = d.fence ? d.fence->GetCompletedValue() : evalNow;
    if (done == UINT64_MAX || done < d.value) return -1;
    d.pending = false;
    const Layout l = LayoutFor(m.motionW, m.motionH);
    void *mapped = nullptr;
    const D3D12_RANGE range{0, static_cast<SIZE_T>(l.size)};
    if (FAILED(d.buffer->Map(0, &range, &mapped)) || mapped == nullptr) {
        Log(true, "Optimizer FPS core: could not map the acc dump's readback buffer; this pass's dump is dropped");
        return 0;
    }
    out.tags = d.tags;
    out.width = m.motionW;
    out.height = m.motionH;
    const std::size_t row = 2u * m.motionW;
    out.model.resize(row * m.motionH);
    out.chain.resize(row * m.motionH);
    out.reason.assign(static_cast<std::size_t>(m.motionW) * m.motionH, 0); // a carried frame's stay 0
    out.carry.resize(d.tags.carry ? row * m.motionH : 0);
    const auto *bytes = static_cast<const unsigned char *>(mapped);
    for (std::uint32_t y = 0; y < m.motionH; ++y) {
        std::memcpy(out.model.data() + y * row, bytes + static_cast<UINT64>(y) * l.rowPitch, row * 2u);
        std::memcpy(out.chain.data() + y * row, bytes + l.chainOffset + static_cast<UINT64>(y) * l.rowPitch, row * 2u);
        if (d.tags.carry) std::memcpy(out.carry.data() + y * row, bytes + l.carryOffset + static_cast<UINT64>(y) * l.rowPitch, row * 2u);
        if (!d.tags.carried)
            std::memcpy(out.reason.data() + static_cast<std::size_t>(y) * m.motionW,
                        bytes + l.reasonOffset + static_cast<UINT64>(y) * l.reasonPitch, m.motionW);
    }
    const D3D12_RANGE none{0, 0};
    d.buffer->Unmap(0, &none);
    return 1;
}

} // namespace ofps::core::temporal
