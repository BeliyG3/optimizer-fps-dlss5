#include "d3d12_adapter_private.h"
#include <cstring>
#include <iterator>
#include <utility>

namespace ofps::sdk {
namespace {
bool TypedUavStoreSupported(ID3D12Device *device, DXGI_FORMAT format) noexcept
{
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support{format};
    return device != nullptr &&
           SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support))) &&
           (support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE) != 0;
}
D3D12_BLEND_DESC OpaqueBlend() noexcept
{
    D3D12_BLEND_DESC result{};
    result.AlphaToCoverageEnable = FALSE;
    result.IndependentBlendEnable = FALSE;
    const D3D12_RENDER_TARGET_BLEND_DESC target{
        FALSE, FALSE,
        D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD,
        D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD,
        D3D12_LOGIC_OP_NOOP, D3D12_COLOR_WRITE_ENABLE_ALL};
    for (auto &entry : result.RenderTarget) entry = target;
    return result;
}

D3D12_RASTERIZER_DESC Rasterizer() noexcept
{
    D3D12_RASTERIZER_DESC result{};
    result.FillMode = D3D12_FILL_MODE_SOLID;
    result.CullMode = D3D12_CULL_MODE_NONE;
    result.FrontCounterClockwise = FALSE;
    result.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
    result.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
    result.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
    result.DepthClipEnable = TRUE;
    result.MultisampleEnable = FALSE;
    result.AntialiasedLineEnable = FALSE;
    result.ForcedSampleCount = 0;
    result.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
    return result;
}

D3D12_DEPTH_STENCIL_DESC DisabledDepth() noexcept
{
    D3D12_DEPTH_STENCIL_DESC result{};
    result.DepthEnable = FALSE;
    result.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    result.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    result.StencilEnable = FALSE;
    return result;
}
} // namespace

AdapterStatus D3D12Adapter::Impl::CreatePipeline(
    const ShaderSet &shaders, ShaderBytecode pixel, const D3D12TargetFormats &formats,
    UINT targetCount, ID3D12PipelineState **output, bool confidenceTarget) noexcept
{
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = rootSignature.Get();
        desc.VS = {shaders.fullscreenVertex.data, shaders.fullscreenVertex.size};
        desc.PS = {pixel.data, pixel.size};
        desc.BlendState = OpaqueBlend();
        desc.SampleMask = UINT_MAX;
        desc.RasterizerState = Rasterizer();
        desc.DepthStencilState = DisabledDepth();
        desc.InputLayout = {nullptr, 0};
        desc.IBStripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
        desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        desc.NumRenderTargets = targetCount;
        desc.RTVFormats[0] = formats.color;
        if (targetCount > 1) {
            desc.RTVFormats[1] = formats.depth;
            desc.RTVFormats[2] = formats.motion;
            // Without a confidence allocation slot 3 is bound as a null view, and a null view is
            // only legal when the pipeline declares no format there (writes are discarded).
            desc.RTVFormats[3] = confidenceTarget ? formats.confidence : DXGI_FORMAT_UNKNOWN;
        }
        desc.DSVFormat = DXGI_FORMAT_UNKNOWN;
        desc.SampleDesc.Count = 1;
        return SUCCEEDED(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(output)))
            ? AdapterStatus::Ok : AdapterStatus::DeviceError;
}

AdapterStatus D3D12Adapter::Impl::CreateResources(
    const D3D12TargetFormats &packFormats, const LayoutV2 &targetLayout,
    std::uint32_t frameCount, std::uint32_t sourceSetCount, bool allocateConfidence) noexcept
{
    auto *next = this;
    ID3D12Device *resourceDevice = next->device.Get();
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.NumDescriptors = sourceSetCount * 4u + frameCount * 4u;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(resourceDevice->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&next->sourceHeap))))
        return AdapterStatus::DeviceError;
    next->descriptorIncrement = resourceDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{};
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.NumDescriptors = frameCount * 4u;
    if (FAILED(resourceDevice->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&next->packedRtvHeap))))
        return AdapterStatus::DeviceError;
    next->rtvDescriptorIncrement = resourceDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    const DXGI_FORMAT packedFormats[] = {
        packFormats.color, packFormats.depth, packFormats.motion, packFormats.confidence};
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = next->packedRtvHeap->GetCPUDescriptorHandleForHeapStart();
    D3D12_CPU_DESCRIPTOR_HANDLE srv = next->sourceHeap->GetCPUDescriptorHandleForHeapStart();
    srv.ptr += static_cast<SIZE_T>(sourceSetCount) * 4u * next->descriptorIncrement;
    D3D12_GPU_DESCRIPTOR_HANDLE gpuSrv = next->sourceHeap->GetGPUDescriptorHandleForHeapStart();
    gpuSrv.ptr += static_cast<UINT64>(sourceSetCount) * 4u * next->descriptorIncrement;
    D3D12_HEAP_PROPERTIES defaultHeap{};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
    for (std::uint32_t frame = 0; frame < frameCount; ++frame) {
        Impl::PackedFrame &packed = next->packedFrames[frame];
        packed.shaderResourceTable = gpuSrv;
        D3D12_CPU_DESCRIPTOR_HANDLE *rtvHandles[] = {
            &packed.rtvs.color, &packed.rtvs.depth, &packed.rtvs.motion, &packed.rtvs.confidence};
        for (std::size_t i = 0; i < std::size(packedFormats); ++i) {
            const bool omittedConfidence = i == 3 && !allocateConfidence;
            if (!omittedConfidence) {
                D3D12_RESOURCE_DESC texture{};
                texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
                texture.Width = targetLayout.workWidth;
                texture.Height = targetLayout.workHeight;
                texture.DepthOrArraySize = 1;
                texture.MipLevels = 1;
                texture.Format = packedFormats[i];
                texture.SampleDesc.Count = 1;
                texture.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
                texture.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
                // OptiScaler can resolve NR directly into packed color when the
                // chosen typed view supports UAV stores.
                if (i == 0 && TypedUavStoreSupported(resourceDevice, packedFormats[i]))
                    texture.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
                if (FAILED(resourceDevice->CreateCommittedResource(
                        &defaultHeap, D3D12_HEAP_FLAG_NONE, &texture,
                        D3D12_RESOURCE_STATE_COMMON, nullptr,
                        IID_PPV_ARGS(&packed.resources[i]))))
                    return AdapterStatus::DeviceError;
            }

            D3D12_RENDER_TARGET_VIEW_DESC rtvDesc{};
            rtvDesc.Format = packedFormats[i];
            rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
            resourceDevice->CreateRenderTargetView(packed.resources[i].Get(), &rtvDesc, rtv);
            *rtvHandles[i] = rtv;
            packed.srvs[i] = srv;
            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
            srvDesc.Format = packedFormats[i];
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.Texture2D.MipLevels = 1;
            resourceDevice->CreateShaderResourceView(packed.resources[i].Get(), &srvDesc, srv);

            rtv.ptr += next->rtvDescriptorIncrement;
            srv.ptr += next->descriptorIncrement;
            gpuSrv.ptr += next->descriptorIncrement;
        }
    }

    D3D12_HEAP_PROPERTIES uploadHeap{};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = 256;
    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;
    buffer.Format = DXGI_FORMAT_UNKNOWN;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(resourceDevice->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &buffer,
                                               D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                               IID_PPV_ARGS(&next->constants))))
        return AdapterStatus::DeviceError;
    void *mapped = nullptr;
    const D3D12_RANGE noRead{0, 0};
    if (FAILED(next->constants->Map(0, &noRead, &mapped))) return AdapterStatus::DeviceError;
    const ShaderConstantsV2 shaderConstants = BuildShaderConstants(targetLayout);
    std::memcpy(mapped, &shaderConstants, sizeof(shaderConstants));
    next->constants->Unmap(0, nullptr);

    buffer.Width = static_cast<UINT64>(sourceSetCount) * 256u;
    if (FAILED(resourceDevice->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &buffer,
                                               D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                               IID_PPV_ARGS(&next->inputConstants))))
        return AdapterStatus::DeviceError;
    if (FAILED(next->inputConstants->Map(0, &noRead, &mapped))) return AdapterStatus::DeviceError;
    const InputDescriptionV2 defaultInput = DefaultInputDescriptionV2(targetLayout.nativeWidth, targetLayout.nativeHeight);
    const InputResourceExtentsV2 defaultExtents{
        targetLayout.nativeWidth, targetLayout.nativeHeight, targetLayout.nativeWidth, targetLayout.nativeHeight,
        targetLayout.nativeWidth, targetLayout.nativeHeight, targetLayout.nativeWidth, targetLayout.nativeHeight};
    ShaderInputConstantsV2 defaultConstants{};
    if (BuildShaderInputConstantsV2(defaultInput, defaultExtents, &defaultConstants) != Status::Ok) {
        next->inputConstants->Unmap(0, nullptr);
        return AdapterStatus::InvalidArgument;
    }
    for (std::uint32_t frame = 0; frame < frameCount; ++frame)
        std::memcpy(static_cast<std::uint8_t *>(mapped) + static_cast<std::size_t>(frame) * 256u,
                    &defaultConstants, sizeof(defaultConstants));
    next->inputConstants->Unmap(0, nullptr);
    return AdapterStatus::Ok;
}

void D3D12Adapter::Shutdown() noexcept
{
    if (impl_) *impl_ = Impl{};
}

AdapterStatus D3D12Adapter::WriteSourceDescriptors(std::uint32_t frameSlot,
                                                   const D3D12SourceResources &sources) noexcept
{
    if (sources.color.resource == nullptr) return AdapterStatus::InvalidArgument;
    const auto colorDesc = sources.color.resource->GetDesc();
    InputDescriptionV2 description = DefaultInputDescriptionV2(
        static_cast<std::uint32_t>(colorDesc.Width), colorDesc.Height);
    if ((impl_->layout.flags & ConfigFlagInputConfidenceValid) != 0)
        description.flags |= InputFlagConfidenceValid;
    return WriteSourceDescriptorsV2(frameSlot, sources, description);
}

AdapterStatus D3D12Adapter::WriteSourceDescriptorsV2(
    std::uint32_t frameSlot, const D3D12SourceResources &sources,
    const InputDescriptionV2 &description) noexcept
{
    if (!impl_->device) return AdapterStatus::NotInitialized;
    if (frameSlot >= impl_->framesInFlight) return AdapterStatus::InvalidArgument;
    return WriteSourceSetV2(frameSlot, sources, description);
}

AdapterStatus D3D12Adapter::WriteSourceSetV2(
    std::uint32_t frameSlot, const D3D12SourceResources &sources,
    const InputDescriptionV2 &description) noexcept
{
    if (!impl_->device) return AdapterStatus::NotInitialized;
    if (frameSlot >= impl_->sourceSets) return AdapterStatus::InvalidArgument;
    impl_->sourceExtents[frameSlot] = Impl::SourceExtentNone;

    D3D12SourceValidation validation{};
    const AdapterStatus status =
        ValidateD3D12Sources(impl_->device.Get(), impl_->layout, sources, description, &validation);
    if (status != AdapterStatus::Ok) return status;

    const D3D12SourceResource entries[] = {
        sources.color, sources.depth, sources.motion, sources.confidence};
    std::uint8_t sourceExtent = Impl::SourceExtentNone;
    if (validation.nativeExtent) sourceExtent |= Impl::SourceExtentNative;
    if (validation.workExtent) sourceExtent |= Impl::SourceExtentWork;

    D3D12_CPU_DESCRIPTOR_HANDLE handle = impl_->sourceHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(frameSlot) * 4u * impl_->descriptorIncrement;
    for (std::size_t i = 0; i < std::size(entries); ++i) {
        D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
        desc.Format = validation.viewFormats[i];
        desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        desc.Texture2D.MipLevels = 1;
        impl_->device->CreateShaderResourceView(entries[i].resource, &desc, handle);
        handle.ptr += impl_->descriptorIncrement;
    }
    void *mapped = nullptr;
    const D3D12_RANGE noRead{0, 0};
    if (FAILED(impl_->inputConstants->Map(0, &noRead, &mapped)))
        return AdapterStatus::DeviceError;
    std::memcpy(static_cast<std::uint8_t *>(mapped) + static_cast<std::size_t>(frameSlot) * 256u,
                &validation.constants, sizeof(validation.constants));
    impl_->inputConstants->Unmap(0, nullptr);
    impl_->sourceExtents[frameSlot] = sourceExtent;
    return AdapterStatus::Ok;
}

ID3D12DescriptorHeap *D3D12Adapter::SourceDescriptorHeap() const noexcept { return impl_->sourceHeap.Get(); }
D3D12PackedViews D3D12Adapter::PackedViews(std::uint32_t frameSlot) const noexcept
{
    if (!impl_->device || frameSlot >= impl_->framesInFlight) return {};
    const Impl::PackedFrame &packed = impl_->packedFrames[frameSlot];
    D3D12PackedViews result{};
    result.resources = {
        {packed.resources[0].Get(), packed.resources[0]->GetDesc().Format},
        {packed.resources[1].Get(), packed.resources[1]->GetDesc().Format},
        {packed.resources[2].Get(), packed.resources[2]->GetDesc().Format},
        {packed.resources[3].Get(), DXGI_FORMAT_R16_FLOAT}};
    result.colorSrv = packed.srvs[0];
    result.depthSrv = packed.srvs[1];
    result.motionSrv = packed.srvs[2];
    result.confidenceSrv = packed.srvs[3];
    result.shaderResourceTable = packed.shaderResourceTable;
    return result;
}
const LayoutV1 *D3D12Adapter::Layout() const noexcept
{
    return impl_->device && impl_->hasLegacyLayout ? &impl_->legacyLayout : nullptr;
}

const LayoutV2 *D3D12Adapter::LayoutV2Description() const noexcept
{
    return impl_->device ? &impl_->layout : nullptr;
}

} // namespace ofps::sdk
