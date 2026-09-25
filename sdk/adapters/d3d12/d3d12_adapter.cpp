#include "d3d12_adapter_private.h"

#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace ofps::sdk {
namespace {

bool SupportedColorFormat(DXGI_FORMAT format) noexcept
{
    return format == DXGI_FORMAT_R8G8B8A8_UNORM || format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
           format == DXGI_FORMAT_B8G8R8A8_UNORM || format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
           format == DXGI_FORMAT_R10G10B10A2_UNORM || format == DXGI_FORMAT_R11G11B10_FLOAT ||
           format == DXGI_FORMAT_R16G16B16A16_FLOAT;
}

bool SupportedDepthView(DXGI_FORMAT format) noexcept
{
    return format == DXGI_FORMAT_R16_FLOAT || format == DXGI_FORMAT_R16_UNORM ||
           format == DXGI_FORMAT_R24_UNORM_X8_TYPELESS || format == DXGI_FORMAT_R32_FLOAT ||
           format == DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
}

bool SupportedMotionView(DXGI_FORMAT format) noexcept
{
    // Four-channel vectors (Cyberpunk 2077 with Ray Reconstruction hands them over as RGBA16F) are read as
    // .xy, as the model reads them: every shader declares its motion input Texture2D<float2>.
    return format == DXGI_FORMAT_R16G16_FLOAT || format == DXGI_FORMAT_R32G32_FLOAT ||
           format == DXGI_FORMAT_R16G16_SNORM || format == DXGI_FORMAT_R16G16_UNORM ||
           format == DXGI_FORMAT_R16G16B16A16_FLOAT || format == DXGI_FORMAT_R32G32B32A32_FLOAT;
}

bool SupportedConfidenceView(DXGI_FORMAT format) noexcept
{
    return format == DXGI_FORMAT_R8_UNORM || format == DXGI_FORMAT_R16_FLOAT ||
           format == DXGI_FORMAT_R32_FLOAT;
}

bool ShaderLoadSupported(ID3D12Device *device, DXGI_FORMAT format) noexcept
{
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support{format};
    return device != nullptr &&
           SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support))) &&
           (support.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_LOAD) != 0;
}

bool ValidTargetFormats(const D3D12TargetFormats &formats) noexcept
{
    return SupportedColorFormat(formats.color) && formats.depth == DXGI_FORMAT_R32_FLOAT &&
           formats.motion == DXGI_FORMAT_R16G16_FLOAT && formats.confidence == DXGI_FORMAT_R16_FLOAT;
}

bool ResourceFormatSupportsView(DXGI_FORMAT resourceFormat, DXGI_FORMAT viewFormat) noexcept
{
    if (resourceFormat == viewFormat) return true;
    switch (viewFormat) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return resourceFormat == DXGI_FORMAT_R8G8B8A8_TYPELESS;
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return resourceFormat == DXGI_FORMAT_B8G8R8A8_TYPELESS;
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        return resourceFormat == DXGI_FORMAT_R16G16B16A16_TYPELESS;
    case DXGI_FORMAT_R10G10B10A2_UNORM:
        return resourceFormat == DXGI_FORMAT_R10G10B10A2_TYPELESS;
    case DXGI_FORMAT_R32_FLOAT:
        return resourceFormat == DXGI_FORMAT_R32_TYPELESS;
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
        return resourceFormat == DXGI_FORMAT_R32G8X24_TYPELESS;
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
        return resourceFormat == DXGI_FORMAT_R24G8_TYPELESS;
    case DXGI_FORMAT_R16_FLOAT:
    case DXGI_FORMAT_R16_UNORM:
        return resourceFormat == DXGI_FORMAT_R16_TYPELESS;
    case DXGI_FORMAT_R16G16_FLOAT:
    case DXGI_FORMAT_R16G16_SNORM:
    case DXGI_FORMAT_R16G16_UNORM:
        return resourceFormat == DXGI_FORMAT_R16G16_TYPELESS;
    case DXGI_FORMAT_R32G32_FLOAT:
        return resourceFormat == DXGI_FORMAT_R32G32_TYPELESS;
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
        return resourceFormat == DXGI_FORMAT_R32G32B32A32_TYPELESS;
    case DXGI_FORMAT_R8_UNORM:
        return resourceFormat == DXGI_FORMAT_R8_TYPELESS;
    default:
        return false;
    }
}

bool ValidTexture2D(const D3D12_RESOURCE_DESC &desc) noexcept
{
    return desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
           desc.Width != 0 && desc.Height != 0 && desc.DepthOrArraySize == 1 &&
           desc.MipLevels != 0 && desc.SampleDesc.Count == 1 &&
           (desc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) == 0;
}

D3D12_STATIC_SAMPLER_DESC StaticSampler(UINT shaderRegister, D3D12_FILTER filter) noexcept
{
    D3D12_STATIC_SAMPLER_DESC result{};
    result.Filter = filter;
    result.AddressU = result.AddressV = result.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    result.MipLODBias = 0.0f;
    result.MaxAnisotropy = 1;
    result.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    result.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
    result.MinLOD = 0.0f;
    result.MaxLOD = D3D12_FLOAT32_MAX;
    result.ShaderRegister = shaderRegister;
    result.RegisterSpace = 0;
    result.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    return result;
}

LayoutV2 AdapterLayoutFromV1(const LayoutV1 &layout) noexcept
{
    LayoutV2 result{};
    result.structSize = sizeof(result);
    result.version = kAbiVersionV2;
    result.mode = layout.mode;
    result.colorFilter = layout.colorFilter;
    result.nativeWidth = layout.nativeWidth;
    result.nativeHeight = layout.nativeHeight;
    result.rawWorkWidth = layout.workWidth;
    result.rawWorkHeight = layout.workHeight;
    result.workWidth = layout.workWidth;
    result.workHeight = layout.workHeight;
    result.centerFractionX = layout.centerFractionX;
    result.centerFractionY = layout.centerFractionY;
    result.configuredWorkFractionX = layout.workFractionX;
    result.configuredWorkFractionY = layout.workFractionY;
    result.rawWorkFractionX = layout.workFractionX;
    result.rawWorkFractionY = layout.workFractionY;
    result.globalScalePercent = 100.0f;
    result.effectiveScaleX = 1.0f;
    result.effectiveScaleY = 1.0f;
    result.compressionX = layout.compressionX;
    result.compressionY = layout.compressionY;
    result.edgeSlopeX = layout.edgeSlopeX;
    result.edgeSlopeY = layout.edgeSlopeY;
    // A v1 layout is symmetric: both sides carry the same curve (the v2 per-side fields are the
    // layout's source of truth for the mapping).
    result.compressionXNeg = result.compressionXPos = layout.compressionX;
    result.compressionYNeg = result.compressionYPos = layout.compressionY;
    result.edgeSlopeXNeg = result.edgeSlopeXPos = layout.edgeSlopeX;
    result.edgeSlopeYNeg = result.edgeSlopeYPos = layout.edgeSlopeY;
    result.minimumLocalScaleX = layout.mode == WarpMode::Peripheral
        ? layout.edgeSlopeX : layout.workFractionX;
    result.minimumLocalScaleY = layout.mode == WarpMode::Peripheral
        ? layout.edgeSlopeY : layout.workFractionY;
    result.maximumSourceFootprintX = result.minimumLocalScaleX > 0.0f
        ? 1.0f / result.minimumLocalScaleX : 0.0f;
    result.maximumSourceFootprintY = result.minimumLocalScaleY > 0.0f
        ? 1.0f / result.minimumLocalScaleY : 0.0f;
    result.pixelPercent = 100.0f * layout.workFractionX * layout.workFractionY;
    result.flags = layout.flags;
    return result;
}

} // namespace

D3D12Adapter::D3D12Adapter() : impl_(std::make_unique<Impl>()) {}
D3D12Adapter::~D3D12Adapter() = default;
D3D12Adapter::D3D12Adapter(D3D12Adapter &&) noexcept = default;
D3D12Adapter &D3D12Adapter::operator=(D3D12Adapter &&) noexcept = default;

AdapterStatus D3D12Adapter::Initialize(ID3D12Device *device, const LayoutV1 &layout,
                                       const D3D12TargetFormats &packFormats,
                                       const D3D12TargetFormats &unpackFormats,
                                       const ShaderSet &shaders, std::uint32_t framesInFlight, std::uint32_t sourceSets)
{
    if (ValidateLayout(layout) != Status::Ok) {
        Shutdown();
        return AdapterStatus::InvalidArgument;
    }
    return InitializeInternal(device, AdapterLayoutFromV1(layout), &layout,
                              packFormats, unpackFormats, shaders, framesInFlight, true, sourceSets);
}

AdapterStatus D3D12Adapter::Initialize(ID3D12Device *device, const LayoutV2 &layout,
                                       const D3D12TargetFormats &packFormats,
                                       const D3D12TargetFormats &unpackFormats,
                                       const ShaderSet &shaders, std::uint32_t framesInFlight,
                                       bool allocateConfidence, std::uint32_t sourceSets)
{
    if (ValidateLayout(layout) != Status::Ok) {
        Shutdown();
        return AdapterStatus::InvalidArgument;
    }
    return InitializeInternal(device, layout, nullptr, packFormats, unpackFormats,
                              shaders, framesInFlight, allocateConfidence, sourceSets);
}

std::uint32_t D3D12Adapter::SourceSetCount() const noexcept { return impl_->device ? impl_->sourceSets : 0u; }

AdapterStatus D3D12Adapter::InitializeInternal(
    ID3D12Device *device, const LayoutV2 &layout, const LayoutV1 *legacyLayout,
    const D3D12TargetFormats &packFormats, const D3D12TargetFormats &unpackFormats,
    const ShaderSet &shaders, std::uint32_t framesInFlight, bool allocateConfidence, std::uint32_t sourceSets)
{
    Shutdown();
    if (sourceSets == 0) sourceSets = framesInFlight;
    if (device == nullptr || framesInFlight == 0 || sourceSets < framesInFlight ||
        sourceSets > (std::numeric_limits<UINT>::max)() / 8u ||
        layout.workWidth == 0 || layout.workHeight == 0)
        return AdapterStatus::InvalidArgument;
    if (!ValidTargetFormats(packFormats) || !ValidTargetFormats(unpackFormats))
        return AdapterStatus::UnsupportedFormat;
    if (!IsValid(shaders.fullscreenVertex) || !IsValid(shaders.packPixel) || !IsValid(shaders.unpackPixel))
        return AdapterStatus::ShaderBytecodeMissing;
    // A layout that promises a valid confidence guide needs somewhere to pack it; a null
    // RTV would silently publish zeros to every consumer.
    if (!allocateConfidence && (layout.flags & ConfigFlagInputConfidenceValid) != 0)
        return AdapterStatus::InvalidArgument;

    auto next = std::make_unique<Impl>();
    next->device = device;
    next->layout = layout;
    if (legacyLayout != nullptr) {
        next->legacyLayout = *legacyLayout;
        next->hasLegacyLayout = true;
    }
    next->framesInFlight = framesInFlight;
    next->sourceSets = sourceSets;
    next->packedConfidenceAllocated = allocateConfidence;
    next->sourceExtents.resize(sourceSets, Impl::SourceExtentNone);
    next->packedFrames.resize(framesInFlight);

    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 4;
    range.BaseShaderRegister = 0;
    range.RegisterSpace = 0;
    range.OffsetInDescriptorsFromTableStart = 0;
    D3D12_ROOT_PARAMETER parameters[4]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameters[0].Descriptor.ShaderRegister = 0;
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameters[1].Descriptor.ShaderRegister = 1;
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[2].DescriptorTable.NumDescriptorRanges = 1;
    parameters[2].DescriptorTable.pDescriptorRanges = &range;
    parameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    parameters[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[3].Constants.ShaderRegister = 2;
    parameters[3].Constants.RegisterSpace = 0;
    parameters[3].Constants.Num32BitValues = 4;
    parameters[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    const D3D12_STATIC_SAMPLER_DESC samplers[] = {
        StaticSampler(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR),
        StaticSampler(1, D3D12_FILTER_MIN_MAG_MIP_POINT)};
    D3D12_ROOT_SIGNATURE_DESC rootDesc{};
    rootDesc.NumParameters = 4;
    rootDesc.pParameters = parameters;
    rootDesc.NumStaticSamplers = 2;
    rootDesc.pStaticSamplers = samplers;
    rootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
                     D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
                     D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
                     D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;
    ComPtr<ID3DBlob> serialized;
    ComPtr<ID3DBlob> errors;
    if (FAILED(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1,
                                           &serialized, &errors)) ||
        FAILED(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                           IID_PPV_ARGS(&next->rootSignature))))
        return AdapterStatus::DeviceError;

    if (next->CreatePipeline(shaders, shaders.packPixel, packFormats, 4, &next->packPipeline, allocateConfidence) != AdapterStatus::Ok ||
        next->CreatePipeline(shaders, shaders.unpackPixel, unpackFormats, 4, &next->unpackPipeline) != AdapterStatus::Ok ||
        next->CreatePipeline(shaders, shaders.unpackPixel, unpackFormats, 1, &next->unpackColorPipeline) != AdapterStatus::Ok)
        return AdapterStatus::DeviceError;
    if (IsValid(shaders.outlinePixel) &&
        next->CreatePipeline(shaders, shaders.outlinePixel, unpackFormats, 1,
                             &next->outlinePipeline) != AdapterStatus::Ok)
        return AdapterStatus::DeviceError;

    const AdapterStatus resourceStatus = next->CreateResources(
        packFormats, layout, framesInFlight, sourceSets, allocateConfidence);
    if (resourceStatus != AdapterStatus::Ok) return resourceStatus;

    impl_ = std::move(next);
    return AdapterStatus::Ok;
}

AdapterStatus ValidateD3D12Sources(ID3D12Device *device, const LayoutV2 &layout,
                                   const D3D12SourceResources &sources,
                                   const InputDescriptionV2 &description,
                                   D3D12SourceValidation *validation) noexcept
{
    if (device == nullptr || validation == nullptr) return AdapterStatus::InvalidArgument;
    if (ValidateInputDescriptionV2(description) != Status::Ok)
        return AdapterStatus::InvalidArgument;
    const bool confidenceRequired = (description.flags & InputFlagConfidenceValid) != 0;
    if (sources.color.resource == nullptr || sources.depth.resource == nullptr ||
        sources.motion.resource == nullptr ||
        (confidenceRequired && sources.confidence.resource == nullptr))
        return AdapterStatus::InvalidArgument;
    if (sources.confidence.resource == nullptr && sources.confidence.format != DXGI_FORMAT_UNKNOWN)
        return AdapterStatus::UnsupportedFormat;

    const D3D12SourceResource entries[] = {
        sources.color, sources.depth, sources.motion, sources.confidence};
    const DXGI_FORMAT viewFormats[] = {
        sources.color.format, sources.depth.format, sources.motion.format,
        sources.confidence.resource != nullptr ? sources.confidence.format : DXGI_FORMAT_R16_FLOAT};
    if (!ShaderLoadSupported(device, viewFormats[0]) ||
        !SupportedDepthView(viewFormats[1]) || !ShaderLoadSupported(device, viewFormats[1]) ||
        !SupportedMotionView(viewFormats[2]) || !ShaderLoadSupported(device, viewFormats[2]) ||
        (sources.confidence.resource != nullptr &&
         (!SupportedConfidenceView(viewFormats[3]) || !ShaderLoadSupported(device, viewFormats[3]))))
        return AdapterStatus::UnsupportedFormat;

    std::array<D3D12_RESOURCE_DESC, 4> resourceDescs{};
    for (std::size_t i = 0; i < std::size(entries); ++i) {
        if (entries[i].resource == nullptr) continue;
        // Same adapter rather than the same device object: a host may hand the adapter a device
        // proxy (ReShade wraps devices and command lists) while its resources answer with the real
        // device. Cross-adapter resources are still refused.
        ComPtr<ID3D12Device> resourceDevice;
        if (FAILED(entries[i].resource->GetDevice(IID_PPV_ARGS(&resourceDevice))))
            return AdapterStatus::ResourceMismatch;
        if (resourceDevice.Get() != device) {
            const LUID a = device->GetAdapterLuid();
            const LUID b = resourceDevice->GetAdapterLuid();
            if (a.LowPart != b.LowPart || a.HighPart != b.HighPart) return AdapterStatus::ResourceMismatch;
        }
        resourceDescs[i] = entries[i].resource->GetDesc();
        if (!ValidTexture2D(resourceDescs[i])) return AdapterStatus::ResourceMismatch;
        if (!ResourceFormatSupportsView(resourceDescs[i].Format, viewFormats[i]))
            return AdapterStatus::UnsupportedFormat;
    }

    const InputResourceExtentsV2 extents{
        static_cast<std::uint32_t>(resourceDescs[0].Width), resourceDescs[0].Height,
        static_cast<std::uint32_t>(resourceDescs[1].Width), resourceDescs[1].Height,
        static_cast<std::uint32_t>(resourceDescs[2].Width), resourceDescs[2].Height,
        sources.confidence.resource != nullptr ? static_cast<std::uint32_t>(resourceDescs[3].Width) : 1u,
        sources.confidence.resource != nullptr ? resourceDescs[3].Height : 1u};
    D3D12SourceValidation result{};
    if (BuildShaderInputConstantsV2(description, extents, &result.constants) != Status::Ok)
        return AdapterStatus::ResourceMismatch;
    for (std::size_t i = 0; i < std::size(viewFormats); ++i) result.viewFormats[i] = viewFormats[i];
    result.nativeExtent = description.colorRect.width == layout.nativeWidth &&
                          description.colorRect.height == layout.nativeHeight;
    result.workExtent = description.colorRect.width == layout.workWidth &&
                        description.colorRect.height == layout.workHeight;
    if (!result.nativeExtent && !result.workExtent) return AdapterStatus::ResourceMismatch;
    *validation = result;
    return AdapterStatus::Ok;
}

AdapterStatus D3D12Adapter::SetOutputColorAdjust(float gain, float gamma) noexcept
{
    if (!impl_->device) return AdapterStatus::NotInitialized;
    if (!std::isfinite(gain) || !std::isfinite(gamma) || gain <= 0.0f || gamma <= 0.0f)
        return AdapterStatus::InvalidArgument;
    const float invGamma = 1.0f / gamma;
    std::memcpy(&impl_->outputGainBits, &gain, sizeof(gain));
    std::memcpy(&impl_->outputInvGammaBits, &invGamma, sizeof(invGamma));
    return AdapterStatus::Ok;
}

} // namespace ofps::sdk
