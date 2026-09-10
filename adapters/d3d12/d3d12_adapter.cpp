#include "d3d12_adapter.h"

#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace pw {
namespace {

bool ValidHandle(D3D12_CPU_DESCRIPTOR_HANDLE handle) noexcept { return handle.ptr != 0; }

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
    return format == DXGI_FORMAT_R16G16_FLOAT || format == DXGI_FORMAT_R32G32_FLOAT ||
           format == DXGI_FORMAT_R16G16_SNORM || format == DXGI_FORMAT_R16G16_UNORM;
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

bool TypedUavStoreSupported(ID3D12Device *device, DXGI_FORMAT format) noexcept
{
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support{format};
    return device != nullptr &&
           SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support))) &&
           (support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE) != 0;
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

struct D3D12Adapter::Impl {
    std::uint32_t outputGainBits = 0;     // float bits of the Unpack colour gain (0 = unset)
    std::uint32_t outputInvGammaBits = 0; // float bits of 1 / gamma (0 = unset)
    enum SourceExtent : std::uint8_t {
        SourceExtentNone = 0,
        SourceExtentNative = 1u << 0,
        SourceExtentWork = 1u << 1,
    };

    struct PackedFrame {
        std::array<ComPtr<ID3D12Resource>, 4> resources;
        D3D12TargetHandles rtvs{};
        std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 4> srvs{};
        D3D12_GPU_DESCRIPTOR_HANDLE shaderResourceTable{};
    };

    LayoutV2 layout{};
    LayoutV1 legacyLayout{};
    bool hasLegacyLayout = false;
    bool packedConfidenceAllocated = true;
    std::uint32_t framesInFlight = 0;
    std::uint32_t sourceSets = 0; // external source sets (>= framesInFlight); the owned SRV tables follow them in the heap
    UINT descriptorIncrement = 0;
    UINT rtvDescriptorIncrement = 0;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12RootSignature> rootSignature;
    ComPtr<ID3D12PipelineState> packPipeline;
    ComPtr<ID3D12PipelineState> unpackPipeline;
    ComPtr<ID3D12PipelineState> unpackColorPipeline;
    ComPtr<ID3D12PipelineState> outlinePipeline;
    ComPtr<ID3D12DescriptorHeap> sourceHeap;
    ComPtr<ID3D12DescriptorHeap> packedRtvHeap;
    ComPtr<ID3D12Resource> constants;
    ComPtr<ID3D12Resource> inputConstants;
    std::vector<std::uint8_t> sourceExtents;
    std::vector<PackedFrame> packedFrames;

    [[nodiscard]] D3D12_GPU_DESCRIPTOR_HANDLE ExternalSourceTable(std::uint32_t sourceSet) const noexcept
    {
        D3D12_GPU_DESCRIPTOR_HANDLE table = sourceHeap->GetGPUDescriptorHandleForHeapStart();
        table.ptr += static_cast<UINT64>(sourceSet) * 4u * descriptorIncrement;
        return table;
    }

    AdapterStatus CreatePipeline(const ShaderSet &shaders, ShaderBytecode pixel,
                                 const D3D12TargetFormats &formats, UINT targetCount,
                                 ID3D12PipelineState **output, bool confidenceTarget = true) noexcept
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

    // `constantSet` selects the input constants (an external source set, or the frame slot for the
    // owned-texture draws, whose constants are those of the set that packed the slot).
    AdapterStatus Record(ID3D12GraphicsCommandList *list, std::uint32_t constantSet,
                         ID3D12PipelineState *pipeline, const D3D12_CPU_DESCRIPTOR_HANDLE *targets,
                         UINT targetCount, std::uint32_t width, std::uint32_t height,
                         D3D12_GPU_DESCRIPTOR_HANDLE sourceTable,
                         DiagnosticOutlineFlags diagnosticOutlines = DiagnosticOutlineNone) noexcept
    {
        if (list == nullptr || pipeline == nullptr || constantSet >= sourceSets ||
            !ValidDiagnosticOutlineFlags(diagnosticOutlines))
            return AdapterStatus::InvalidArgument;
        ID3D12DescriptorHeap *heaps[] = {sourceHeap.Get()};
        list->SetDescriptorHeaps(1, heaps);
        list->SetGraphicsRootSignature(rootSignature.Get());
        list->SetPipelineState(pipeline);
        list->SetGraphicsRootConstantBufferView(0, constants->GetGPUVirtualAddress());
        list->SetGraphicsRootConstantBufferView(
            1, inputConstants->GetGPUVirtualAddress() + static_cast<UINT64>(constantSet) * 256u);
        list->SetGraphicsRootDescriptorTable(2, sourceTable);
        const std::uint32_t diagnosticConstants[4]{
            static_cast<std::uint32_t>(diagnosticOutlines), outputGainBits, outputInvGammaBits, 0};
        list->SetGraphicsRoot32BitConstants(3, 4, diagnosticConstants, 0);
        const D3D12_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f};
        const D3D12_RECT scissor{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
        list->RSSetViewports(1, &viewport);
        list->RSSetScissorRects(1, &scissor);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->OMSetRenderTargets(targetCount, targets, FALSE, nullptr);
        list->DrawInstanced(3, 1, 0, 0);
        return AdapterStatus::Ok;
    }
};

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

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.NumDescriptors = sourceSets * 4u + framesInFlight * 4u;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&next->sourceHeap))))
        return AdapterStatus::DeviceError;
    next->descriptorIncrement = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{};
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.NumDescriptors = framesInFlight * 4u;
    if (FAILED(device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&next->packedRtvHeap))))
        return AdapterStatus::DeviceError;
    next->rtvDescriptorIncrement = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    const DXGI_FORMAT packedFormats[] = {
        packFormats.color, packFormats.depth, packFormats.motion, packFormats.confidence};
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = next->packedRtvHeap->GetCPUDescriptorHandleForHeapStart();
    D3D12_CPU_DESCRIPTOR_HANDLE srv = next->sourceHeap->GetCPUDescriptorHandleForHeapStart();
    srv.ptr += static_cast<SIZE_T>(sourceSets) * 4u * next->descriptorIncrement;
    D3D12_GPU_DESCRIPTOR_HANDLE gpuSrv = next->sourceHeap->GetGPUDescriptorHandleForHeapStart();
    gpuSrv.ptr += static_cast<UINT64>(sourceSets) * 4u * next->descriptorIncrement;
    D3D12_HEAP_PROPERTIES defaultHeap{};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
    for (std::uint32_t frame = 0; frame < framesInFlight; ++frame) {
        Impl::PackedFrame &packed = next->packedFrames[frame];
        packed.shaderResourceTable = gpuSrv;
        D3D12_CPU_DESCRIPTOR_HANDLE *rtvHandles[] = {
            &packed.rtvs.color, &packed.rtvs.depth, &packed.rtvs.motion, &packed.rtvs.confidence};
        for (std::size_t i = 0; i < std::size(packedFormats); ++i) {
            const bool omittedConfidence = i == 3 && !allocateConfidence;
            if (!omittedConfidence) {
                D3D12_RESOURCE_DESC texture{};
                texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
                texture.Width = layout.workWidth;
                texture.Height = layout.workHeight;
                texture.DepthOrArraySize = 1;
                texture.MipLevels = 1;
                texture.Format = packedFormats[i];
                texture.SampleDesc.Count = 1;
                texture.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
                texture.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
                // OptiScaler can resolve NR directly into packed color when the
                // chosen typed view supports UAV stores.
                if (i == 0 && TypedUavStoreSupported(device, packedFormats[i]))
                    texture.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
                if (FAILED(device->CreateCommittedResource(
                        &defaultHeap, D3D12_HEAP_FLAG_NONE, &texture,
                        D3D12_RESOURCE_STATE_COMMON, nullptr,
                        IID_PPV_ARGS(&packed.resources[i]))))
                    return AdapterStatus::DeviceError;
            }

            D3D12_RENDER_TARGET_VIEW_DESC rtvDesc{};
            rtvDesc.Format = packedFormats[i];
            rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
            device->CreateRenderTargetView(packed.resources[i].Get(), &rtvDesc, rtv);
            *rtvHandles[i] = rtv;
            packed.srvs[i] = srv;
            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
            srvDesc.Format = packedFormats[i];
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.Texture2D.MipLevels = 1;
            device->CreateShaderResourceView(packed.resources[i].Get(), &srvDesc, srv);

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
    if (FAILED(device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &buffer,
                                               D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                               IID_PPV_ARGS(&next->constants))))
        return AdapterStatus::DeviceError;
    void *mapped = nullptr;
    const D3D12_RANGE noRead{0, 0};
    if (FAILED(next->constants->Map(0, &noRead, &mapped))) return AdapterStatus::DeviceError;
    const ShaderConstantsV2 constants = BuildShaderConstants(layout);
    std::memcpy(mapped, &constants, sizeof(constants));
    next->constants->Unmap(0, nullptr);

    buffer.Width = static_cast<UINT64>(sourceSets) * 256u;
    if (FAILED(device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &buffer,
                                               D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                               IID_PPV_ARGS(&next->inputConstants))))
        return AdapterStatus::DeviceError;
    if (FAILED(next->inputConstants->Map(0, &noRead, &mapped))) return AdapterStatus::DeviceError;
    const InputDescriptionV2 defaultInput = DefaultInputDescriptionV2(layout.nativeWidth, layout.nativeHeight);
    const InputResourceExtentsV2 defaultExtents{
        layout.nativeWidth, layout.nativeHeight, layout.nativeWidth, layout.nativeHeight,
        layout.nativeWidth, layout.nativeHeight, layout.nativeWidth, layout.nativeHeight};
    ShaderInputConstantsV2 defaultConstants{};
    if (BuildShaderInputConstantsV2(defaultInput, defaultExtents, &defaultConstants) != Status::Ok) {
        next->inputConstants->Unmap(0, nullptr);
        return AdapterStatus::InvalidArgument;
    }
    for (std::uint32_t frame = 0; frame < framesInFlight; ++frame)
        std::memcpy(static_cast<std::uint8_t *>(mapped) + static_cast<std::size_t>(frame) * 256u,
                    &defaultConstants, sizeof(defaultConstants));
    next->inputConstants->Unmap(0, nullptr);

    impl_ = std::move(next);
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

AdapterStatus D3D12Adapter::RecordPack(ID3D12GraphicsCommandList *commandList, std::uint32_t frameSlot,
                                       const D3D12TargetHandles &targets) noexcept
{
    if (!impl_->device) return AdapterStatus::NotInitialized;
    if (frameSlot >= impl_->framesInFlight) return AdapterStatus::InvalidArgument;
    return RecordPackFromSet(commandList, frameSlot, frameSlot, targets);
}

AdapterStatus D3D12Adapter::RecordPack(ID3D12GraphicsCommandList *commandList,
                                       std::uint32_t frameSlot) noexcept
{
    if (!impl_->device) return AdapterStatus::NotInitialized;
    if (frameSlot >= impl_->framesInFlight) return AdapterStatus::InvalidArgument;
    return RecordPackFromSet(commandList, frameSlot, frameSlot, impl_->packedFrames[frameSlot].rtvs);
}

AdapterStatus D3D12Adapter::RecordPackFromSet(ID3D12GraphicsCommandList *commandList, std::uint32_t frameSlot,
                                              std::uint32_t sourceSet, const D3D12TargetHandles &targets) noexcept
{
    if (!impl_->device) return AdapterStatus::NotInitialized;
    if (!ValidHandle(targets.color) || !ValidHandle(targets.depth) ||
        !ValidHandle(targets.motion) || !ValidHandle(targets.confidence)) return AdapterStatus::InvalidArgument;
    if (frameSlot >= impl_->framesInFlight || sourceSet >= impl_->sourceSets) return AdapterStatus::InvalidArgument;
    if ((impl_->sourceExtents[sourceSet] & Impl::SourceExtentNative) == 0)
        return AdapterStatus::ResourceMismatch;
    const D3D12_CPU_DESCRIPTOR_HANDLE handles[] = {targets.color, targets.depth, targets.motion, targets.confidence};
    (void) frameSlot; // the targets already name the slot's textures
    return impl_->Record(commandList, sourceSet, impl_->packPipeline.Get(), handles, 4,
                         impl_->layout.workWidth, impl_->layout.workHeight,
                         impl_->ExternalSourceTable(sourceSet));
}

AdapterStatus D3D12Adapter::RecordPackFromSet(ID3D12GraphicsCommandList *commandList, std::uint32_t frameSlot,
                                              std::uint32_t sourceSet) noexcept
{
    if (!impl_->device) return AdapterStatus::NotInitialized;
    if (frameSlot >= impl_->framesInFlight) return AdapterStatus::InvalidArgument;
    return RecordPackFromSet(commandList, frameSlot, sourceSet, impl_->packedFrames[frameSlot].rtvs);
}

AdapterStatus D3D12Adapter::RecordUnpack(ID3D12GraphicsCommandList *commandList, std::uint32_t frameSlot,
                                         const D3D12TargetHandles &targets) noexcept
{
    return RecordUnpack(commandList, frameSlot, targets, DiagnosticOutlineNone);
}

AdapterStatus D3D12Adapter::RecordUnpack(
    ID3D12GraphicsCommandList *commandList, std::uint32_t frameSlot,
    const D3D12TargetHandles &targets, DiagnosticOutlineFlags diagnosticOutlines) noexcept
{
    if (!impl_->device) return AdapterStatus::NotInitialized;
    if (!ValidDiagnosticOutlineFlags(diagnosticOutlines)) return AdapterStatus::InvalidArgument;
    if (!ValidHandle(targets.color) || !ValidHandle(targets.depth) ||
        !ValidHandle(targets.motion) || !ValidHandle(targets.confidence)) return AdapterStatus::InvalidArgument;
    if (frameSlot >= impl_->framesInFlight ||
        (impl_->sourceExtents[frameSlot] & Impl::SourceExtentWork) == 0)
        return AdapterStatus::ResourceMismatch;
    const D3D12_CPU_DESCRIPTOR_HANDLE handles[] = {targets.color, targets.depth, targets.motion, targets.confidence};
    return impl_->Record(commandList, frameSlot, impl_->unpackPipeline.Get(), handles, 4,
                         impl_->layout.nativeWidth, impl_->layout.nativeHeight,
                         impl_->ExternalSourceTable(frameSlot), diagnosticOutlines);
}

AdapterStatus D3D12Adapter::RecordUnpackColor(ID3D12GraphicsCommandList *commandList, std::uint32_t frameSlot,
                                              D3D12_CPU_DESCRIPTOR_HANDLE target) noexcept
{
    return RecordUnpackColor(commandList, frameSlot, target, DiagnosticOutlineNone);
}

AdapterStatus D3D12Adapter::RecordUnpackColor(
    ID3D12GraphicsCommandList *commandList, std::uint32_t frameSlot,
    D3D12_CPU_DESCRIPTOR_HANDLE target, DiagnosticOutlineFlags diagnosticOutlines) noexcept
{
    if (!impl_->device) return AdapterStatus::NotInitialized;
    if (frameSlot >= impl_->framesInFlight) return AdapterStatus::InvalidArgument;
    return RecordUnpackColorFromSet(commandList, frameSlot, frameSlot, target, diagnosticOutlines);
}

AdapterStatus D3D12Adapter::RecordUnpackColorFromSet(
    ID3D12GraphicsCommandList *commandList, std::uint32_t frameSlot, std::uint32_t sourceSet,
    D3D12_CPU_DESCRIPTOR_HANDLE target, DiagnosticOutlineFlags diagnosticOutlines) noexcept
{
    if (!impl_->device) return AdapterStatus::NotInitialized;
    if (!ValidDiagnosticOutlineFlags(diagnosticOutlines)) return AdapterStatus::InvalidArgument;
    if (!ValidHandle(target)) return AdapterStatus::InvalidArgument;
    if (frameSlot >= impl_->framesInFlight || sourceSet >= impl_->sourceSets) return AdapterStatus::InvalidArgument;
    if ((impl_->sourceExtents[sourceSet] & Impl::SourceExtentWork) == 0)
        return AdapterStatus::ResourceMismatch;
    (void) frameSlot;
    return impl_->Record(commandList, sourceSet, impl_->unpackColorPipeline.Get(), &target, 1,
                         impl_->layout.nativeWidth, impl_->layout.nativeHeight,
                         impl_->ExternalSourceTable(sourceSet), diagnosticOutlines);
}

AdapterStatus D3D12Adapter::RecordUnpackOwned(ID3D12GraphicsCommandList *commandList,
                                              std::uint32_t frameSlot,
                                              const D3D12TargetHandles &targets) noexcept
{
    return RecordUnpackOwned(commandList, frameSlot, targets, DiagnosticOutlineNone);
}

AdapterStatus D3D12Adapter::RecordUnpackOwned(
    ID3D12GraphicsCommandList *commandList, std::uint32_t frameSlot,
    const D3D12TargetHandles &targets, DiagnosticOutlineFlags diagnosticOutlines) noexcept
{
    if (!impl_->device) return AdapterStatus::NotInitialized;
    if (!impl_->packedConfidenceAllocated) return AdapterStatus::ResourceMismatch;
    if (!ValidDiagnosticOutlineFlags(diagnosticOutlines)) return AdapterStatus::InvalidArgument;
    if (frameSlot >= impl_->framesInFlight || !ValidHandle(targets.color) ||
        !ValidHandle(targets.depth) || !ValidHandle(targets.motion) ||
        !ValidHandle(targets.confidence))
        return AdapterStatus::InvalidArgument;
    const D3D12_CPU_DESCRIPTOR_HANDLE handles[] = {
        targets.color, targets.depth, targets.motion, targets.confidence};
    return impl_->Record(commandList, frameSlot, impl_->unpackPipeline.Get(), handles, 4,
                         impl_->layout.nativeWidth, impl_->layout.nativeHeight,
                         impl_->packedFrames[frameSlot].shaderResourceTable,
                         diagnosticOutlines);
}

AdapterStatus D3D12Adapter::RecordUnpackOwnedColor(
    ID3D12GraphicsCommandList *commandList, std::uint32_t frameSlot,
    D3D12_CPU_DESCRIPTOR_HANDLE target) noexcept
{
    return RecordUnpackOwnedColor(commandList, frameSlot, target, DiagnosticOutlineNone);
}

AdapterStatus D3D12Adapter::RecordUnpackOwnedColor(
    ID3D12GraphicsCommandList *commandList, std::uint32_t frameSlot,
    D3D12_CPU_DESCRIPTOR_HANDLE target, DiagnosticOutlineFlags diagnosticOutlines) noexcept
{
    if (!impl_->device) return AdapterStatus::NotInitialized;
    if (frameSlot >= impl_->framesInFlight) return AdapterStatus::InvalidArgument;
    return RecordUnpackOwnedColorFromSet(commandList, frameSlot, frameSlot, target, diagnosticOutlines);
}

AdapterStatus D3D12Adapter::RecordUnpackOwnedColorFromSet(
    ID3D12GraphicsCommandList *commandList, std::uint32_t frameSlot, std::uint32_t sourceSet,
    D3D12_CPU_DESCRIPTOR_HANDLE target, DiagnosticOutlineFlags diagnosticOutlines) noexcept
{
    if (!impl_->device) return AdapterStatus::NotInitialized;
    if (!ValidDiagnosticOutlineFlags(diagnosticOutlines)) return AdapterStatus::InvalidArgument;
    if (frameSlot >= impl_->framesInFlight || sourceSet >= impl_->sourceSets || !ValidHandle(target))
        return AdapterStatus::InvalidArgument;
    return impl_->Record(commandList, sourceSet, impl_->unpackColorPipeline.Get(), &target, 1,
                         impl_->layout.nativeWidth, impl_->layout.nativeHeight,
                         impl_->packedFrames[frameSlot].shaderResourceTable,
                         diagnosticOutlines);
}

AdapterStatus D3D12Adapter::RecordOutlines(
    ID3D12GraphicsCommandList *commandList, std::uint32_t frameSlot,
    D3D12_CPU_DESCRIPTOR_HANDLE target,
    DiagnosticOutlineFlags diagnosticOutlines) noexcept
{
    if (!impl_->device) return AdapterStatus::NotInitialized;
    if (!ValidDiagnosticOutlineFlags(diagnosticOutlines)) return AdapterStatus::InvalidArgument;
    if (!impl_->outlinePipeline) return AdapterStatus::ShaderBytecodeMissing;
    if (frameSlot >= impl_->framesInFlight || !ValidHandle(target))
        return AdapterStatus::InvalidArgument;
    if (diagnosticOutlines == DiagnosticOutlineNone) return AdapterStatus::Ok;
    return impl_->Record(commandList, frameSlot, impl_->outlinePipeline.Get(), &target, 1,
                         impl_->layout.nativeWidth, impl_->layout.nativeHeight,
                         impl_->ExternalSourceTable(frameSlot), diagnosticOutlines);
}

AdapterStatus D3D12Adapter::RecordCenterOutline(
    ID3D12GraphicsCommandList *commandList, std::uint32_t frameSlot,
    D3D12_CPU_DESCRIPTOR_HANDLE target) noexcept
{
    return RecordOutlines(commandList, frameSlot, target, DiagnosticOutlineCenter);
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

} // namespace pw
