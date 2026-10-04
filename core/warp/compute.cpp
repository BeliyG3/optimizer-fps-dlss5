#include "core/warp/compute.h"

#include "core/context.h"
#include "core/gpu/barriers.h"
#include "core/gpu/compute_pipeline.h"
#include "core/warp/format_support.h"
#include "core/warp/resources.h"

#include <array>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace ofps::core::warp {
using detail::FormatName;
using detail::ShaderReadable;
using detail::TypedStore;
using detail::ViewCompatible;
namespace {
constexpr auto kRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
constexpr auto kWrite = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

D3D12_CPU_DESCRIPTOR_HANDLE CpuHandle(const WarpResources& r, std::uint32_t set,
                                       std::uint32_t entry) {
    auto h = r.heap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(set * kWarpDescriptorsPerSet + entry) * r.descriptorStride;
    return h;
}

D3D12_GPU_DESCRIPTOR_HANDLE GpuHandle(const WarpResources& r, std::uint32_t set) {
    auto h = r.heap->GetGPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<UINT64>(set) * kWarpDescriptorsPerSet * r.descriptorStride;
    return h;
}

bool SrgbView(DXGI_FORMAT f) {
    return f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
           f == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
}

void Srv(ID3D12Device* device, ID3D12Resource* resource, DXGI_FORMAT view,
         D3D12_CPU_DESCRIPTOR_HANDLE handle) {
    D3D12_SHADER_RESOURCE_VIEW_DESC d{};
    d.Format = view;
    d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    d.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(resource, &d, handle);
}

void Uav(ID3D12Device* device, ID3D12Resource* resource, DXGI_FORMAT view,
         D3D12_CPU_DESCRIPTOR_HANDLE handle) {
    D3D12_UNORDERED_ACCESS_VIEW_DESC d{};
    d.Format = view;
    d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    device->CreateUnorderedAccessView(resource, nullptr, &d, handle);
}

struct DiagnosticConstants { std::uint32_t outlines, gainBits, invGammaBits, transfer; };
} // namespace

bool TransferEligible(DXGI_FORMAT colorView, DXGI_FORMAT answerView) {
    return SrgbView(colorView) == SrgbView(answerView);
}

struct ComputePath::Impl {
    WarpResources resources;
    gpu::ComputePipeline pipeline;
    sdk::LayoutV2 layout{};
    std::vector<sdk::ShaderInputConstantsV2> inputs;
    std::vector<bool> packedValid;
    // What each slot's last Pack read for colour and depth: the view its validation chose, the resting
    // state and subresource the host gave, and the evaluate it happened in (the pointers are valid only then).
    struct PackInput {
        ID3D12Resource* resource = nullptr;
        DXGI_FORMAT view = DXGI_FORMAT_UNKNOWN;
        D3D12_RESOURCE_STATES rest = D3D12_RESOURCE_STATE_COMMON;
        UINT subresource = 0;
    };
    struct PackSource { PackInput color, depth; std::uint64_t evaluate = 0; };
    std::vector<PackSource> packSources;
    bool transferApplied = false;
    DXGI_FORMAT colorView = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT outputView = DXGI_FORMAT_UNKNOWN;

    bool BindConstants(std::uint32_t set, const sdk::ShaderInputConstantsV2& input,
                       DiagnosticConstants diagnostic) {
        const std::uint32_t first = set * 3;
        const auto mapping = sdk::BuildShaderConstants(layout);
        if (!resources.constants.Write(first, &mapping, sizeof(mapping)) ||
            !resources.constants.Write(first + 1, &input, sizeof(input)) ||
            !resources.constants.Write(first + 2, &diagnostic, sizeof(diagnostic))) return false;
        for (std::uint32_t i = 0; i < 3; ++i)
            if (!resources.constants.CreateView(resources.device, first + i,
                                                CpuHandle(resources, set, 10 + i))) return false;
        return true;
    }
};

ComputePath::~ComputePath() = default;

std::unique_ptr<ComputePath> ComputePath::Create(
    ID3D12Device* commandListDevice, const sdk::LayoutV2& layout,
    DXGI_FORMAT colorView, DXGI_FORMAT outputView,
    std::uint32_t frameSlots, std::string& reason) {
    return Create(commandListDevice, layout, colorView, outputView, frameSlots, true, reason);
}

std::unique_ptr<ComputePath> ComputePath::Create(
    ID3D12Device* commandListDevice, const sdk::LayoutV2& layout,
    DXGI_FORMAT colorView, DXGI_FORMAT outputView,
    std::uint32_t frameSlots, bool copyIntermediates, std::string& reason) {
    reason.clear();
    if (!commandListDevice || !frameSlots || sdk::ValidateLayout(layout) != sdk::Status::Ok) {
        reason = "compute warp: invalid device, layout or frame slots";
        return nullptr;
    }
    for (const auto format : {colorView, DXGI_FORMAT_R32_FLOAT,
                              DXGI_FORMAT_R16G16_FLOAT, outputView}) {
        if (!TypedStore(commandListDevice, format)) {
            reason = "typed UAV store unsupported for " + FormatName(format);
            return nullptr;
        }
    }
    const auto& shaders = Ctx().shaders;
    if (!shaders.WarpLoaded()) { reason = "compute warp shader missing"; return nullptr; }
    auto path = std::unique_ptr<ComputePath>(new ComputePath);
    path->impl_ = std::make_unique<Impl>();
    auto& p = *path->impl_;
    p.layout = layout;
    p.inputs.resize(frameSlots);
    p.packedValid.resize(frameSlots, false);
    p.packSources.resize(frameSlots);
    p.colorView = colorView;
    p.outputView = outputView;
    if (!p.resources.Create(commandListDevice, layout.workWidth, layout.workHeight,
                            layout.nativeWidth, layout.nativeHeight, colorView, outputView,
                            frameSlots, copyIntermediates, 64)) {
        reason = "compute warp: texture, descriptor heap or upload allocation failed";
        return nullptr;
    }
    if (!p.pipeline.Create(commandListDevice, shaders.warpPack.data(), shaders.warpPack.size(),
                           shaders.warpUnpack.data(), shaders.warpUnpack.size())) {
        reason = p.pipeline.Reason();
        return nullptr;
    }
    return path;
}

Packed ComputePath::PackedFor(std::uint32_t frameSlot) const noexcept {
    if (!impl_ || frameSlot >= impl_->resources.slots.size()) return {};
    const auto& slot = impl_->resources.slots[frameSlot];
    return {slot.color.resource, slot.depth.resource, slot.motion.resource};
}

bool ComputePath::Pack(ID3D12GraphicsCommandList* cmd, std::uint32_t frameSlot,
                        const sdk::D3D12SourceResources& sources,
                        const sdk::InputDescriptionV2& input,
                        const D3D12_RESOURCE_STATES sourceRest[3],
                        const UINT sourceSubresources[3],
                        const OfpsFencePoint* privateUsePoint, Packed* out, std::string& reason) {
    if (!impl_ || !cmd || !out || !sourceRest || !sourceSubresources ||
        frameSlot >= impl_->resources.slots.size()) {
        reason = "Pack: invalid command, slot or output"; return false;
    }
    auto& p = *impl_;
    p.packedValid[frameSlot] = false;
    sdk::D3D12SourceValidation validation{};
    const auto status = sdk::ValidateD3D12Sources(p.resources.device, p.layout, sources,
                                                   input, &validation);
    if (status != sdk::AdapterStatus::Ok || !validation.nativeExtent) {
        reason = std::string("Pack source validation failed: ") + sdk::AdapterStatusString(status);
        return false;
    }
    const auto set = p.resources.AcquireSet(privateUsePoint ? *privateUsePoint : HostUsePoint(cmd),
                                            Ctx().evalCounter);
    if (set == gpu::DescriptorPool::kNone) { reason = "Pack descriptor ring exhausted"; return false; }
    const std::array<sdk::D3D12SourceResource, 4> entries{
        sources.color, sources.depth, sources.motion, sources.confidence};
    for (std::uint32_t i = 0; i < entries.size(); ++i)
        Srv(p.resources.device, entries[i].resource, validation.viewFormats[i],
            CpuHandle(p.resources, set, i));
    Srv(p.resources.device, nullptr, DXGI_FORMAT_R16G16B16A16_FLOAT, CpuHandle(p.resources, set, 4));
    Srv(p.resources.device, nullptr, DXGI_FORMAT_R16G16B16A16_FLOAT, CpuHandle(p.resources, set, 5));
    Srv(p.resources.device, nullptr, DXGI_FORMAT_R32_FLOAT, CpuHandle(p.resources, set, 6));
    auto& slot = p.resources.slots[frameSlot];
    Uav(p.resources.device, slot.color.resource, slot.color.view, CpuHandle(p.resources, set, 7));
    Uav(p.resources.device, slot.depth.resource, slot.depth.view, CpuHandle(p.resources, set, 8));
    Uav(p.resources.device, slot.motion.resource, slot.motion.view, CpuHandle(p.resources, set, 9));
    if (!p.BindConstants(set, validation.constants, {})) {
        reason = "Pack constant upload failed"; return false;
    }
    const std::array<ID3D12Resource*, 3> sourceResources{
        sources.color.resource, sources.depth.resource, sources.motion.resource};
    for (std::uint32_t i = 0; i < 3; ++i)
        gpu::BarrierExternal(cmd, sourceResources[i], sourceRest[i], kRead, sourceSubresources[i]);
    gpu::Barrier(cmd, slot.color.resource, slot.color.state, kWrite);
    gpu::Barrier(cmd, slot.depth.resource, slot.depth.state, kWrite);
    gpu::Barrier(cmd, slot.motion.resource, slot.motion.state, kWrite);
    const std::uint32_t rect[4]{0, 0, p.layout.workWidth, p.layout.workHeight};
    p.pipeline.Record(cmd, p.resources.heap, GpuHandle(p.resources, set), rect, false,
                      p.layout.workWidth, p.layout.workHeight);
    gpu::Barrier(cmd, slot.color.resource, slot.color.state, kRead);
    gpu::Barrier(cmd, slot.depth.resource, slot.depth.state, kRead);
    gpu::Barrier(cmd, slot.motion.resource, slot.motion.state, kRead);
    for (std::uint32_t i = 0; i < 3; ++i)
        gpu::BarrierExternal(cmd, sourceResources[i], kRead, sourceRest[i], sourceSubresources[i]);
    p.inputs[frameSlot] = validation.constants;
    p.packSources[frameSlot] = {
        {sources.color.resource, validation.viewFormats[0], sourceRest[0], sourceSubresources[0]},
        {sources.depth.resource, validation.viewFormats[1], sourceRest[1], sourceSubresources[1]},
        Ctx().evalCounter};
    p.packedValid[frameSlot] = true;
    *out = PackedFor(frameSlot);
    return true;
}

bool ComputePath::Unpack(ID3D12GraphicsCommandList* cmd, std::uint32_t frameSlot,
                         ID3D12Resource* answer, DXGI_FORMAT answerView,
                         const UnpackTarget& target, std::uint32_t outlines,
                         float gain, float gamma, const OfpsFencePoint* privateUsePoint,
                         std::string& reason) {
    return Unpack(cmd, frameSlot, answer, answerView, target, outlines, gain, gamma,
                  privateUsePoint, nullptr, reason);
}

bool ComputePath::Unpack(ID3D12GraphicsCommandList* cmd, std::uint32_t frameSlot,
                         ID3D12Resource* answer, DXGI_FORMAT answerView,
                         const UnpackTarget& target, std::uint32_t outlines,
                         float gain, float gamma, const OfpsFencePoint* privateUsePoint,
                         const TransferRequest* transferRequest, std::string& reason) {
    if (impl_) impl_->transferApplied = false;
    if (!impl_ || !cmd || frameSlot >= impl_->resources.slots.size() ||
        !impl_->packedValid[frameSlot] || !target.resource || !answer ||
        answerView == DXGI_FORMAT_UNKNOWN || !std::isfinite(gain) ||
        !std::isfinite(gamma) || gamma <= 0.0f) {
        reason = "Unpack: invalid answer, target or arguments"; return false;
    }
    auto& p = *impl_;
    const auto ad = answer->GetDesc();
    const auto td = target.resource->GetDesc();
    const bool answerValid = answer != target.resource && ShaderReadable(p.resources.device, answerView) &&
        ViewCompatible(ad.Format, answerView) &&
        ad.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
        ad.DepthOrArraySize == 1 && ad.MipLevels >= 1 &&
        ad.SampleDesc.Count == 1 && ad.Width >= p.layout.workWidth &&
        ad.Height >= p.layout.workHeight;
    const bool validSubresource = td.MipLevels != 0 &&
        target.subresource < static_cast<std::uint32_t>(td.MipLevels) * td.DepthOrArraySize;
    const auto mip = validSubresource ? target.subresource % td.MipLevels : 0;
    const auto mipWidth = std::max<UINT64>(1, td.Width >> mip);
    const auto mipHeight = std::max<UINT>(1, td.Height >> mip);
    const bool regionFits = validSubresource && target.width == p.layout.nativeWidth &&
        target.height == p.layout.nativeHeight && target.x <= mipWidth &&
        target.y <= mipHeight && target.width <= mipWidth - target.x &&
        target.height <= mipHeight - target.y;
    UnpackSupport support = TargetUnpackSupport(td, target.view, p.outputView, target.subresource,
        TypedStore(p.resources.device, target.view), TypedStore(p.resources.device, p.outputView));
    support.answerValid = answerValid;
    support.outputRegionFits = regionFits;
    support.temporalNeedsNativeRead = target.temporalNeedsNativeRead;
    const auto decision = DecideUnpack(support);
    if (decision.path == UnpackPath::None) { reason = ReasonText(decision.reason); return false; }
    if (decision.path == UnpackPath::CopyFromUav && !p.resources.slots[frameSlot].intermediate.resource) {
        reason = "Unpack: the target needs the copy fallback, which this texture set did not prepare";
        return false;
    }
    const auto& src = p.packSources[frameSlot];
    // Colour-space contract (TransferEligible): the model's input and answer share one numeric space (the
    // temporal residual relies on the same contract), and the frame is read through the same kind of
    // view as the model input; an sRGB view on one side and not on the other does not transfer.
    const bool sameSpace = TransferEligible(src.color.view, answerView) &&
                           TransferEligible(p.colorView, answerView);
    const bool packedThisEvaluate = src.evaluate == Ctx().evalCounter;
    const bool distinct = src.color.resource && src.depth.resource &&
        src.color.resource != target.resource && src.depth.resource != target.resource &&
        src.color.resource != answer && src.depth.resource != answer;
    const bool transfer = transferRequest != nullptr && sameSpace && packedThisEvaluate && distinct &&
        static_cast<std::uint32_t>(p.layout.colorFilter) >= static_cast<std::uint32_t>(sdk::ColorFilter::DetailTransfer);
    const auto set = p.resources.AcquireSet(privateUsePoint ? *privateUsePoint : HostUsePoint(cmd),
                                            Ctx().evalCounter);
    if (set == gpu::DescriptorPool::kNone) { reason = "Unpack descriptor ring exhausted"; return false; }
    auto& slot = p.resources.slots[frameSlot];
    Srv(p.resources.device, answer, answerView, CpuHandle(p.resources, set, 0));
    Srv(p.resources.device, slot.depth.resource, slot.depth.view, CpuHandle(p.resources, set, 1));
    Srv(p.resources.device, slot.motion.resource, slot.motion.view, CpuHandle(p.resources, set, 2));
    Srv(p.resources.device, nullptr, DXGI_FORMAT_R16_FLOAT, CpuHandle(p.resources, set, 3));
    if (transfer) {
        Srv(p.resources.device, slot.color.resource, slot.color.view, CpuHandle(p.resources, set, 4));
        Srv(p.resources.device, src.color.resource, src.color.view, CpuHandle(p.resources, set, 5));
        Srv(p.resources.device, src.depth.resource, src.depth.view, CpuHandle(p.resources, set, 6));
    } else {
        Srv(p.resources.device, nullptr, DXGI_FORMAT_R16G16B16A16_FLOAT, CpuHandle(p.resources, set, 4));
        Srv(p.resources.device, nullptr, DXGI_FORMAT_R16G16B16A16_FLOAT, CpuHandle(p.resources, set, 5));
        Srv(p.resources.device, nullptr, DXGI_FORMAT_R32_FLOAT, CpuHandle(p.resources, set, 6));
    }
    ID3D12Resource* destination = decision.path == UnpackPath::DirectUav ?
        target.resource : slot.intermediate.resource;
    Uav(p.resources.device, destination, p.outputView, CpuHandle(p.resources, set, 7));
    Uav(p.resources.device, nullptr, DXGI_FORMAT_R32_FLOAT, CpuHandle(p.resources, set, 8));
    Uav(p.resources.device, nullptr, DXGI_FORMAT_R16G16_FLOAT, CpuHandle(p.resources, set, 9));
    // Transfer word (warp_cs.hlsl): bit 0 depth reversed, bit 1 transfer bound, bit 2 the Peripheral
    // band is exact 1:1 (work extent == raw work extent, GlobalScale 100); below that the band is shrunk too.
    const bool bandExact = p.layout.workWidth == p.layout.rawWorkWidth &&
                           p.layout.workHeight == p.layout.rawWorkHeight;
    const DiagnosticConstants diagnostic{outlines, std::bit_cast<std::uint32_t>(gain),
        std::bit_cast<std::uint32_t>(1.0f / gamma),
        transfer ? (2u | (transferRequest->depthReversed ? 1u : 0u) | (bandExact ? 4u : 0u)) : 0u};
    if (!p.BindConstants(set, p.inputs[frameSlot], diagnostic)) {
        reason = "Unpack constant upload failed"; return false;
    }
    auto* state = decision.path == UnpackPath::DirectUav ? nullptr : &slot.intermediate.state;
    if (state) gpu::Barrier(cmd, destination, *state, kWrite);
    else gpu::BarrierExternal(cmd, destination, target.restState, kWrite, target.subresource);
    const std::uint32_t rect[4]{decision.path == UnpackPath::DirectUav ? target.x : 0,
        decision.path == UnpackPath::DirectUav ? target.y : 0, target.width, target.height};
    if (transfer)
        for (const auto* in : {&src.color, &src.depth})
            gpu::BarrierExternal(cmd, in->resource, in->rest, kRead, in->subresource);
    p.pipeline.Record(cmd, p.resources.heap, GpuHandle(p.resources, set), rect, true,
                      target.width, target.height);
    if (transfer)
        for (const auto* in : {&src.color, &src.depth})
            gpu::BarrierExternal(cmd, in->resource, kRead, in->rest, in->subresource);
    if (state) {
        gpu::Barrier(cmd, destination, *state, D3D12_RESOURCE_STATE_COPY_SOURCE);
        gpu::BarrierExternal(cmd, target.resource, target.restState,
                             D3D12_RESOURCE_STATE_COPY_DEST, target.subresource);
        D3D12_TEXTURE_COPY_LOCATION from{destination, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        D3D12_TEXTURE_COPY_LOCATION to{target.resource, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        to.SubresourceIndex = target.subresource;
        const D3D12_BOX box{0, 0, 0, target.width, target.height, 1};
        cmd->CopyTextureRegion(&to, target.x, target.y, 0, &from, &box);
        gpu::BarrierExternal(cmd, target.resource, D3D12_RESOURCE_STATE_COPY_DEST,
                             target.restState, target.subresource);
        gpu::Barrier(cmd, destination, *state, D3D12_RESOURCE_STATE_COMMON);
    } else {
        if (target.restState == kWrite) {
            D3D12_RESOURCE_BARRIER sync{};
            sync.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            sync.UAV.pResource = destination;
            cmd->ResourceBarrier(1, &sync);
        }
        gpu::BarrierExternal(cmd, destination, kWrite, target.restState, target.subresource);
    }
    p.transferApplied = transfer;
    return true;
}

bool ComputePath::TransferApplied() const noexcept { return impl_ && impl_->transferApplied; }

bool ComputePath::BaseUnpack(ID3D12GraphicsCommandList* cmd, std::uint32_t frameSlot,
                             const UnpackTarget& target, const OfpsFencePoint* privateUsePoint,
                             std::string& reason) {
    if (!impl_ || frameSlot >= impl_->resources.slots.size() || !impl_->packedValid[frameSlot]) {
        reason = "base Unpack: no packed frame"; return false;
    }
    return Unpack(cmd, frameSlot, impl_->resources.slots[frameSlot].color.resource,
                  impl_->colorView, target, 0, 1.0f, 1.0f, privateUsePoint, reason);
}

} // namespace ofps::core::warp
