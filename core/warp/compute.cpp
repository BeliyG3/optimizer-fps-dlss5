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
constexpr std::uint32_t kDescriptorsPerSet = 10;

D3D12_CPU_DESCRIPTOR_HANDLE CpuHandle(const WarpResources& r, std::uint32_t set,
                                       std::uint32_t entry) {
    auto h = r.heap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(set * kDescriptorsPerSet + entry) * r.descriptorStride;
    return h;
}

D3D12_GPU_DESCRIPTOR_HANDLE GpuHandle(const WarpResources& r, std::uint32_t set) {
    auto h = r.heap->GetGPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<UINT64>(set) * kDescriptorsPerSet * r.descriptorStride;
    return h;
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

struct DiagnosticConstants { std::uint32_t outlines, gainBits, invGammaBits, unused; };
} // namespace

struct ComputePath::Impl {
    WarpResources resources;
    gpu::ComputePipeline pipeline;
    sdk::LayoutV2 layout{};
    std::vector<sdk::ShaderInputConstantsV2> inputs;
    std::vector<bool> packedValid;
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
                                                CpuHandle(resources, set, 7 + i))) return false;
        return true;
    }
};

ComputePath::~ComputePath() = default;

std::unique_ptr<ComputePath> ComputePath::Create(
    ID3D12Device* commandListDevice, const sdk::LayoutV2& layout,
    DXGI_FORMAT colorView, DXGI_FORMAT outputView,
    std::uint32_t frameSlots, std::string& reason) {
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
    p.colorView = colorView;
    p.outputView = outputView;
    if (!p.resources.Create(commandListDevice, layout.workWidth, layout.workHeight,
                            layout.nativeWidth, layout.nativeHeight, colorView, outputView,
                            frameSlots, 64)) {
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
    auto& slot = p.resources.slots[frameSlot];
    Uav(p.resources.device, slot.color.resource, slot.color.view, CpuHandle(p.resources, set, 4));
    Uav(p.resources.device, slot.depth.resource, slot.depth.view, CpuHandle(p.resources, set, 5));
    Uav(p.resources.device, slot.motion.resource, slot.motion.view, CpuHandle(p.resources, set, 6));
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
    p.packedValid[frameSlot] = true;
    *out = PackedFor(frameSlot);
    return true;
}

bool ComputePath::Unpack(ID3D12GraphicsCommandList* cmd, std::uint32_t frameSlot,
                         ID3D12Resource* answer, DXGI_FORMAT answerView,
                         const UnpackTarget& target, std::uint32_t outlines,
                         float gain, float gamma, const OfpsFencePoint* privateUsePoint,
                         std::string& reason) {
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
    const UnpackSupport support{answerValid, TypedStore(p.resources.device, target.view),
        (td.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0,
        target.subresource == 0, regionFits,
        td.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && td.SampleDesc.Count == 1 &&
            td.DepthOrArraySize == 1 && ViewCompatible(td.Format, target.view),
        target.view == p.outputView && ViewCompatible(td.Format, p.outputView),
        TypedStore(p.resources.device, p.outputView),
        target.temporalNeedsNativeRead};
    const auto decision = DecideUnpack(support);
    if (decision.path == UnpackPath::None) { reason = ReasonText(decision.reason); return false; }
    const auto set = p.resources.AcquireSet(privateUsePoint ? *privateUsePoint : HostUsePoint(cmd),
                                            Ctx().evalCounter);
    if (set == gpu::DescriptorPool::kNone) { reason = "Unpack descriptor ring exhausted"; return false; }
    auto& slot = p.resources.slots[frameSlot];
    Srv(p.resources.device, answer, answerView, CpuHandle(p.resources, set, 0));
    Srv(p.resources.device, slot.depth.resource, slot.depth.view, CpuHandle(p.resources, set, 1));
    Srv(p.resources.device, slot.motion.resource, slot.motion.view, CpuHandle(p.resources, set, 2));
    Srv(p.resources.device, nullptr, DXGI_FORMAT_R16_FLOAT, CpuHandle(p.resources, set, 3));
    ID3D12Resource* destination = decision.path == UnpackPath::DirectUav ?
        target.resource : slot.intermediate.resource;
    Uav(p.resources.device, destination, p.outputView, CpuHandle(p.resources, set, 4));
    Uav(p.resources.device, nullptr, DXGI_FORMAT_R32_FLOAT, CpuHandle(p.resources, set, 5));
    Uav(p.resources.device, nullptr, DXGI_FORMAT_R16G16_FLOAT, CpuHandle(p.resources, set, 6));
    const DiagnosticConstants diagnostic{outlines, std::bit_cast<std::uint32_t>(gain),
        std::bit_cast<std::uint32_t>(1.0f / gamma), 0};
    if (!p.BindConstants(set, p.inputs[frameSlot], diagnostic)) {
        reason = "Unpack constant upload failed"; return false;
    }
    auto* state = decision.path == UnpackPath::DirectUav ? nullptr : &slot.intermediate.state;
    if (state) gpu::Barrier(cmd, destination, *state, kWrite);
    else gpu::BarrierExternal(cmd, destination, target.restState, kWrite, target.subresource);
    const std::uint32_t rect[4]{decision.path == UnpackPath::DirectUav ? target.x : 0,
        decision.path == UnpackPath::DirectUav ? target.y : 0, target.width, target.height};
    p.pipeline.Record(cmd, p.resources.heap, GpuHandle(p.resources, set), rect, true,
                      target.width, target.height);
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
    return true;
}

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
