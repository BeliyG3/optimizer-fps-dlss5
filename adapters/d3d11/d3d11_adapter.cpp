#include "d3d11_adapter.h"

#include <cmath>
#include <algorithm>
#include <array>
#include <d3d11_1.h>
#include <utility>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace pw {
namespace {

struct alignas(16) DiagnosticConstants {
    std::uint32_t outlineFlags;
    std::uint32_t outputGainBits;     // float bits, 0 = unset
    std::uint32_t outputInvGammaBits; // float bits, 0 = unset
    std::uint32_t reserved;
};

static_assert(sizeof(DiagnosticConstants) == 16);

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

bool ShaderSampleSupported(ID3D11Device *device, DXGI_FORMAT format) noexcept
{
    UINT support = 0;
    return device != nullptr && SUCCEEDED(device->CheckFormatSupport(format, &support)) &&
           (support & D3D11_FORMAT_SUPPORT_SHADER_SAMPLE) != 0;
}

AdapterStatus ValidateContext(ID3D11DeviceContext *context, ID3D11Device *expectedDevice) noexcept
{
    if (context == nullptr) return AdapterStatus::InvalidArgument;
    ComPtr<ID3D11Device> contextDevice;
    context->GetDevice(&contextDevice);
    return contextDevice.Get() == expectedDevice ? AdapterStatus::Ok : AdapterStatus::ResourceMismatch;
}

AdapterStatus DescribeSourceView(ID3D11ShaderResourceView *view, ID3D11Device *expectedDevice,
                                 bool required, DXGI_FORMAT *format,
                                 std::uint32_t *width, std::uint32_t *height) noexcept
{
    if (view == nullptr) return required ? AdapterStatus::InvalidArgument : AdapterStatus::Ok;
    if (format == nullptr || width == nullptr || height == nullptr)
        return AdapterStatus::InvalidArgument;

    D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc{};
    view->GetDesc(&viewDesc);
    if (viewDesc.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D)
        return AdapterStatus::ResourceMismatch;
    if (!ShaderSampleSupported(expectedDevice, viewDesc.Format))
        return AdapterStatus::UnsupportedFormat;

    ComPtr<ID3D11Resource> resource;
    view->GetResource(&resource);
    ComPtr<ID3D11Device> resourceDevice;
    resource->GetDevice(&resourceDevice);
    if (resourceDevice.Get() != expectedDevice) return AdapterStatus::ResourceMismatch;

    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(resource.As(&texture))) return AdapterStatus::ResourceMismatch;
    D3D11_TEXTURE2D_DESC textureDesc{};
    texture->GetDesc(&textureDesc);
    if (textureDesc.ArraySize != 1 || textureDesc.SampleDesc.Count != 1 ||
        viewDesc.Texture2D.MostDetailedMip >= textureDesc.MipLevels)
        return AdapterStatus::ResourceMismatch;
    *format = viewDesc.Format;
    *width = (std::max)(1u, textureDesc.Width >> viewDesc.Texture2D.MostDetailedMip);
    *height = (std::max)(1u, textureDesc.Height >> viewDesc.Texture2D.MostDetailedMip);
    return AdapterStatus::Ok;
}

AdapterStatus ValidateSourceViewExact(ID3D11ShaderResourceView *view, ID3D11Device *expectedDevice,
                                      std::uint32_t expectedWidth, std::uint32_t expectedHeight,
                                      DXGI_FORMAT expectedFormat, bool required) noexcept
{
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    const AdapterStatus status = DescribeSourceView(
        view, expectedDevice, required, &format, &width, &height);
    if (status != AdapterStatus::Ok || view == nullptr) return status;
    if (format != expectedFormat) return AdapterStatus::UnsupportedFormat;
    return width == expectedWidth && height == expectedHeight
        ? AdapterStatus::Ok : AdapterStatus::ResourceMismatch;
}

AdapterStatus ValidateTargetView(ID3D11RenderTargetView *view, ID3D11Device *expectedDevice,
                                 std::uint32_t expectedWidth, std::uint32_t expectedHeight,
                                 DXGI_FORMAT expectedFormat, bool required) noexcept
{
    if (view == nullptr) return required ? AdapterStatus::InvalidArgument : AdapterStatus::Ok;

    D3D11_RENDER_TARGET_VIEW_DESC viewDesc{};
    view->GetDesc(&viewDesc);
    if (viewDesc.ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2D)
        return AdapterStatus::ResourceMismatch;
    if (viewDesc.Format != expectedFormat) return AdapterStatus::UnsupportedFormat;

    ComPtr<ID3D11Resource> resource;
    view->GetResource(&resource);
    ComPtr<ID3D11Device> resourceDevice;
    resource->GetDevice(&resourceDevice);
    if (resourceDevice.Get() != expectedDevice) return AdapterStatus::ResourceMismatch;

    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(resource.As(&texture))) return AdapterStatus::ResourceMismatch;
    D3D11_TEXTURE2D_DESC textureDesc{};
    texture->GetDesc(&textureDesc);
    if (textureDesc.ArraySize != 1 || textureDesc.SampleDesc.Count != 1 ||
        viewDesc.Texture2D.MipSlice >= textureDesc.MipLevels)
        return AdapterStatus::ResourceMismatch;
    const std::uint32_t width = (std::max)(1u, textureDesc.Width >> viewDesc.Texture2D.MipSlice);
    const std::uint32_t height = (std::max)(1u, textureDesc.Height >> viewDesc.Texture2D.MipSlice);
    return width == expectedWidth && height == expectedHeight
        ? AdapterStatus::Ok : AdapterStatus::ResourceMismatch;
}

template <typename T>
struct ShaderStageState {
    ComPtr<T> shader;
    std::array<ComPtr<ID3D11ClassInstance>, D3D11_SHADER_MAX_INTERFACES> classInstances;
    UINT classInstanceCount = 0;
};

struct ContextState {
    using ShaderResourceState = std::array<
        ComPtr<ID3D11ShaderResourceView>, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT>;

    ID3D11DeviceContext *context = nullptr;
    ComPtr<ID3D11DeviceContext1> context1;
    std::array<ComPtr<ID3D11RenderTargetView>, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> renderTargets;
    ComPtr<ID3D11DepthStencilView> depthStencilView;
    ComPtr<ID3D11BlendState> blendState;
    FLOAT blendFactor[4]{};
    UINT sampleMask = 0;
    ComPtr<ID3D11DepthStencilState> depthStencilState;
    UINT stencilReference = 0;
    ComPtr<ID3D11RasterizerState> rasterizerState;
    D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    UINT viewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    D3D11_RECT scissors[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    UINT scissorCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ComPtr<ID3D11InputLayout> inputLayout;
    ShaderStageState<ID3D11VertexShader> vertex;
    ShaderStageState<ID3D11HullShader> hull;
    ShaderStageState<ID3D11DomainShader> domain;
    ShaderStageState<ID3D11GeometryShader> geometry;
    ShaderStageState<ID3D11PixelShader> pixel;
    static constexpr UINT kConstantSlots = 3; // b0 warp, b1 input description, b2 diagnostics
    std::array<ComPtr<ID3D11Buffer>, kConstantSlots> vsConstants;
    std::array<ComPtr<ID3D11Buffer>, kConstantSlots> psConstants;
    UINT vsFirstConstants[kConstantSlots]{};
    UINT vsConstantCounts[kConstantSlots]{};
    UINT psFirstConstants[kConstantSlots]{};
    UINT psConstantCounts[kConstantSlots]{};
    ShaderResourceState vertexResources;
    ShaderResourceState hullResources;
    ShaderResourceState domainResources;
    ShaderResourceState geometryResources;
    ShaderResourceState pixelResources;
    ShaderResourceState computeResources;
    std::array<ComPtr<ID3D11SamplerState>, 2> psSamplers;
    std::array<ComPtr<ID3D11UnorderedAccessView>, D3D11_1_UAV_SLOT_COUNT> omUnorderedAccessViews;
    std::array<ComPtr<ID3D11UnorderedAccessView>, D3D11_1_UAV_SLOT_COUNT> csUnorderedAccessViews;
    UINT unorderedAccessViewCount = D3D11_PS_CS_UAV_REGISTER_COUNT;
    ComPtr<ID3D11Predicate> predicate;
    BOOL predicateValue = FALSE;

    explicit ContextState(ID3D11DeviceContext *value) : context(value)
    {
        context->QueryInterface(IID_PPV_ARGS(&context1));
        ID3D11RenderTargetView *rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
        ID3D11DepthStencilView *dsv = nullptr;
        context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, &dsv);
        for (std::size_t i = 0; i < renderTargets.size(); ++i) renderTargets[i].Attach(rtvs[i]);
        depthStencilView.Attach(dsv);
        ID3D11BlendState *blend = nullptr;
        context->OMGetBlendState(&blend, blendFactor, &sampleMask);
        blendState.Attach(blend);
        ID3D11DepthStencilState *depth = nullptr;
        context->OMGetDepthStencilState(&depth, &stencilReference);
        depthStencilState.Attach(depth);
        ID3D11RasterizerState *rasterizer = nullptr;
        context->RSGetState(&rasterizer);
        rasterizerState.Attach(rasterizer);
        context->RSGetViewports(&viewportCount, viewports);
        context->RSGetScissorRects(&scissorCount, scissors);
        context->IAGetPrimitiveTopology(&topology);
        ID3D11InputLayout *layout = nullptr;
        context->IAGetInputLayout(&layout);
        inputLayout.Attach(layout);
        CaptureVertexShader();
        CaptureHullShader();
        CaptureDomainShader();
        CaptureGeometryShader();
        CapturePixelShader();
        ID3D11Buffer *buffers[kConstantSlots]{};
        if (context1)
            context1->VSGetConstantBuffers1(0, kConstantSlots, buffers, vsFirstConstants, vsConstantCounts);
        else
            context->VSGetConstantBuffers(0, kConstantSlots, buffers);
        for (std::size_t i = 0; i < vsConstants.size(); ++i) vsConstants[i].Attach(buffers[i]);
        for (auto &b : buffers) b = nullptr;
        if (context1)
            context1->PSGetConstantBuffers1(0, kConstantSlots, buffers, psFirstConstants, psConstantCounts);
        else
            context->PSGetConstantBuffers(0, kConstantSlots, buffers);
        for (std::size_t i = 0; i < psConstants.size(); ++i) psConstants[i].Attach(buffers[i]);
        CaptureShaderResources(vertexResources, [this](ID3D11ShaderResourceView **resources) {
            context->VSGetShaderResources(
                0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, resources);
        });
        CaptureShaderResources(hullResources, [this](ID3D11ShaderResourceView **resources) {
            context->HSGetShaderResources(
                0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, resources);
        });
        CaptureShaderResources(domainResources, [this](ID3D11ShaderResourceView **resources) {
            context->DSGetShaderResources(
                0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, resources);
        });
        CaptureShaderResources(geometryResources, [this](ID3D11ShaderResourceView **resources) {
            context->GSGetShaderResources(
                0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, resources);
        });
        CaptureShaderResources(pixelResources, [this](ID3D11ShaderResourceView **resources) {
            context->PSGetShaderResources(
                0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, resources);
        });
        CaptureShaderResources(computeResources, [this](ID3D11ShaderResourceView **resources) {
            context->CSGetShaderResources(
                0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, resources);
        });
        ID3D11SamplerState *samplers[2]{};
        context->PSGetSamplers(0, 2, samplers);
        for (std::size_t i = 0; i < psSamplers.size(); ++i) psSamplers[i].Attach(samplers[i]);
        ComPtr<ID3D11Device> device;
        context->GetDevice(&device);
        if (context1 && device->GetFeatureLevel() >= D3D_FEATURE_LEVEL_11_1)
            unorderedAccessViewCount = D3D11_1_UAV_SLOT_COUNT;
        std::array<ID3D11UnorderedAccessView *, D3D11_1_UAV_SLOT_COUNT> omUavs{};
        context->OMGetRenderTargetsAndUnorderedAccessViews(
            0, nullptr, nullptr, 0, unorderedAccessViewCount, omUavs.data());
        for (UINT i = 0; i < unorderedAccessViewCount; ++i)
            omUnorderedAccessViews[i].Attach(omUavs[i]);
        std::array<ID3D11UnorderedAccessView *, D3D11_1_UAV_SLOT_COUNT> csUavs{};
        context->CSGetUnorderedAccessViews(0, unorderedAccessViewCount, csUavs.data());
        for (UINT i = 0; i < unorderedAccessViewCount; ++i)
            csUnorderedAccessViews[i].Attach(csUavs[i]);
        ID3D11Predicate *savedPredicate = nullptr;
        context->GetPredication(&savedPredicate, &predicateValue);
        predicate.Attach(savedPredicate);
    }

    ~ContextState()
    {
        ID3D11RenderTargetView *rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
        UINT renderTargetCount = 0;
        for (UINT i = 0; i < static_cast<UINT>(renderTargets.size()); ++i) {
            rtvs[i] = renderTargets[i].Get();
            if (rtvs[i] != nullptr) renderTargetCount = i + 1;
        }
        std::array<ID3D11UnorderedAccessView *, D3D11_1_UAV_SLOT_COUNT> omUavs{};
        std::array<UINT, D3D11_1_UAV_SLOT_COUNT> initialCounts{};
        initialCounts.fill(D3D11_KEEP_UNORDERED_ACCESS_VIEWS);
        for (UINT i = 0; i < unorderedAccessViewCount; ++i)
            omUavs[i] = omUnorderedAccessViews[i].Get();
        context->OMSetRenderTargetsAndUnorderedAccessViews(
            renderTargetCount, renderTargetCount != 0 ? rtvs : nullptr,
            depthStencilView.Get(), renderTargetCount,
            unorderedAccessViewCount - renderTargetCount,
            omUavs.data() + renderTargetCount,
            initialCounts.data() + renderTargetCount);
        context->OMSetBlendState(blendState.Get(), blendFactor, sampleMask);
        context->OMSetDepthStencilState(depthStencilState.Get(), stencilReference);
        context->RSSetState(rasterizerState.Get());
        context->RSSetViewports(viewportCount, viewportCount != 0 ? viewports : nullptr);
        context->RSSetScissorRects(scissorCount, scissorCount != 0 ? scissors : nullptr);
        context->IASetPrimitiveTopology(topology);
        context->IASetInputLayout(inputLayout.Get());
        RestoreVertexShader();
        RestoreHullShader();
        RestoreDomainShader();
        RestoreGeometryShader();
        RestorePixelShader();
        ID3D11Buffer *vsCbs[kConstantSlots]{};
        ID3D11Buffer *psCbs[kConstantSlots]{};
        for (std::size_t i = 0; i < kConstantSlots; ++i) {
            vsCbs[i] = vsConstants[i].Get();
            psCbs[i] = psConstants[i].Get();
        }
        if (context1) {
            context1->VSSetConstantBuffers1(0, kConstantSlots, vsCbs, vsFirstConstants, vsConstantCounts);
            context1->PSSetConstantBuffers1(0, kConstantSlots, psCbs, psFirstConstants, psConstantCounts);
        } else {
            context->VSSetConstantBuffers(0, kConstantSlots, vsCbs);
            context->PSSetConstantBuffers(0, kConstantSlots, psCbs);
        }
        ID3D11SamplerState *samplers[2]{};
        for (std::size_t i = 0; i < psSamplers.size(); ++i) samplers[i] = psSamplers[i].Get();
        context->PSSetSamplers(0, 2, samplers);
        std::array<ID3D11UnorderedAccessView *, D3D11_1_UAV_SLOT_COUNT> csUavs{};
        for (UINT i = 0; i < unorderedAccessViewCount; ++i)
            csUavs[i] = csUnorderedAccessViews[i].Get();
        context->CSSetUnorderedAccessViews(
            0, unorderedAccessViewCount, csUavs.data(), initialCounts.data());
        // OMSetRenderTargets may implicitly unbind an aliasing SRV from any shader stage and
        // slot. Restore all SRVs only after the original render targets and UAVs are back.
        RestoreShaderResources(vertexResources, [this](ID3D11ShaderResourceView *const *resources) {
            context->VSSetShaderResources(
                0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, resources);
        });
        RestoreShaderResources(hullResources, [this](ID3D11ShaderResourceView *const *resources) {
            context->HSSetShaderResources(
                0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, resources);
        });
        RestoreShaderResources(domainResources, [this](ID3D11ShaderResourceView *const *resources) {
            context->DSSetShaderResources(
                0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, resources);
        });
        RestoreShaderResources(geometryResources, [this](ID3D11ShaderResourceView *const *resources) {
            context->GSSetShaderResources(
                0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, resources);
        });
        RestoreShaderResources(pixelResources, [this](ID3D11ShaderResourceView *const *resources) {
            context->PSSetShaderResources(
                0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, resources);
        });
        RestoreShaderResources(computeResources, [this](ID3D11ShaderResourceView *const *resources) {
            context->CSSetShaderResources(
                0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, resources);
        });
        context->SetPredication(predicate.Get(), predicateValue);
    }

    void PrepareForDraw()
    {
        std::array<ID3D11UnorderedAccessView *, D3D11_1_UAV_SLOT_COUNT> nullUavs{};
        std::array<UINT, D3D11_1_UAV_SLOT_COUNT> keepCounts{};
        keepCounts.fill(D3D11_KEEP_UNORDERED_ACCESS_VIEWS);
        context->CSSetUnorderedAccessViews(
            0, unorderedAccessViewCount, nullUavs.data(), keepCounts.data());
    }

private:
    template <typename Getter>
    static void CaptureShaderResources(ShaderResourceState &state, Getter &&getter)
    {
        std::array<
            ID3D11ShaderResourceView *, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT> resources{};
        getter(resources.data());
        for (std::size_t i = 0; i < state.size(); ++i)
            state[i].Attach(resources[i]);
    }

    template <typename Setter>
    static void RestoreShaderResources(const ShaderResourceState &state, Setter &&setter)
    {
        std::array<
            ID3D11ShaderResourceView *, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT> resources{};
        for (std::size_t i = 0; i < state.size(); ++i)
            resources[i] = state[i].Get();
        setter(resources.data());
    }

    template <typename T>
    static std::array<ID3D11ClassInstance *, D3D11_SHADER_MAX_INTERFACES>
    RawClasses(const ShaderStageState<T> &stage) noexcept
    {
        std::array<ID3D11ClassInstance *, D3D11_SHADER_MAX_INTERFACES> result{};
        for (UINT i = 0; i < stage.classInstanceCount; ++i)
            result[i] = stage.classInstances[i].Get();
        return result;
    }

    void CaptureVertexShader()
    {
        ID3D11VertexShader *shader = nullptr;
        std::array<ID3D11ClassInstance *, D3D11_SHADER_MAX_INTERFACES> classes{};
        vertex.classInstanceCount = static_cast<UINT>(classes.size());
        context->VSGetShader(&shader, classes.data(), &vertex.classInstanceCount);
        vertex.shader.Attach(shader);
        for (UINT i = 0; i < vertex.classInstanceCount; ++i) vertex.classInstances[i].Attach(classes[i]);
    }
    void CaptureHullShader()
    {
        ID3D11HullShader *shader = nullptr;
        std::array<ID3D11ClassInstance *, D3D11_SHADER_MAX_INTERFACES> classes{};
        hull.classInstanceCount = static_cast<UINT>(classes.size());
        context->HSGetShader(&shader, classes.data(), &hull.classInstanceCount);
        hull.shader.Attach(shader);
        for (UINT i = 0; i < hull.classInstanceCount; ++i) hull.classInstances[i].Attach(classes[i]);
    }
    void CaptureDomainShader()
    {
        ID3D11DomainShader *shader = nullptr;
        std::array<ID3D11ClassInstance *, D3D11_SHADER_MAX_INTERFACES> classes{};
        domain.classInstanceCount = static_cast<UINT>(classes.size());
        context->DSGetShader(&shader, classes.data(), &domain.classInstanceCount);
        domain.shader.Attach(shader);
        for (UINT i = 0; i < domain.classInstanceCount; ++i) domain.classInstances[i].Attach(classes[i]);
    }
    void CaptureGeometryShader()
    {
        ID3D11GeometryShader *shader = nullptr;
        std::array<ID3D11ClassInstance *, D3D11_SHADER_MAX_INTERFACES> classes{};
        geometry.classInstanceCount = static_cast<UINT>(classes.size());
        context->GSGetShader(&shader, classes.data(), &geometry.classInstanceCount);
        geometry.shader.Attach(shader);
        for (UINT i = 0; i < geometry.classInstanceCount; ++i) geometry.classInstances[i].Attach(classes[i]);
    }
    void CapturePixelShader()
    {
        ID3D11PixelShader *shader = nullptr;
        std::array<ID3D11ClassInstance *, D3D11_SHADER_MAX_INTERFACES> classes{};
        pixel.classInstanceCount = static_cast<UINT>(classes.size());
        context->PSGetShader(&shader, classes.data(), &pixel.classInstanceCount);
        pixel.shader.Attach(shader);
        for (UINT i = 0; i < pixel.classInstanceCount; ++i) pixel.classInstances[i].Attach(classes[i]);
    }

    void RestoreVertexShader()
    {
        auto classes = RawClasses(vertex);
        context->VSSetShader(vertex.shader.Get(), classes.data(), vertex.classInstanceCount);
    }
    void RestoreHullShader()
    {
        auto classes = RawClasses(hull);
        context->HSSetShader(hull.shader.Get(), classes.data(), hull.classInstanceCount);
    }
    void RestoreDomainShader()
    {
        auto classes = RawClasses(domain);
        context->DSSetShader(domain.shader.Get(), classes.data(), domain.classInstanceCount);
    }
    void RestoreGeometryShader()
    {
        auto classes = RawClasses(geometry);
        context->GSSetShader(geometry.shader.Get(), classes.data(), geometry.classInstanceCount);
    }
    void RestorePixelShader()
    {
        auto classes = RawClasses(pixel);
        context->PSSetShader(pixel.shader.Get(), classes.data(), pixel.classInstanceCount);
    }
};

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

struct D3D11Adapter::Impl {
    std::uint32_t outputGainBits = 0;
    std::uint32_t outputInvGammaBits = 0;
    LayoutV2 layout{};
    LayoutV1 legacyLayout{};
    bool hasLegacyLayout = false;
    DXGI_FORMAT colorFormat = DXGI_FORMAT_UNKNOWN;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11VertexShader> fullscreenVertex;
    ComPtr<ID3D11PixelShader> packPixel;
    ComPtr<ID3D11PixelShader> unpackPixel;
    ComPtr<ID3D11PixelShader> outlinePixel;
    ComPtr<ID3D11Buffer> constants;
    ComPtr<ID3D11Buffer> inputConstants;
    ComPtr<ID3D11Buffer> diagnosticConstants;
    ComPtr<ID3D11SamplerState> linearSampler;
    ComPtr<ID3D11SamplerState> pointSampler;
    ComPtr<ID3D11RasterizerState> rasterizer;
    ComPtr<ID3D11BlendState> blend;
    ComPtr<ID3D11DepthStencilState> depthStencil;
    std::array<ComPtr<ID3D11Texture2D>, 4> packedTextures;
    std::array<ComPtr<ID3D11ShaderResourceView>, 4> packedSrvs;
    std::array<ComPtr<ID3D11RenderTargetView>, 4> packedRtvs;

    AdapterStatus Draw(ID3D11DeviceContext *context, ID3D11PixelShader *shader,
                       const D3D11SourceViews &sources, const D3D11TargetViews &targets,
                       std::uint32_t width, std::uint32_t height,
                       DiagnosticOutlineFlags diagnosticOutlines = DiagnosticOutlineNone) noexcept
    {
        if (context == nullptr || shader == nullptr || targets.color == nullptr ||
            !ValidDiagnosticOutlineFlags(diagnosticOutlines))
            return AdapterStatus::InvalidArgument;
        const DiagnosticConstants diagnostics{
            static_cast<std::uint32_t>(diagnosticOutlines), outputGainBits, outputInvGammaBits, 0};
        context->UpdateSubresource(diagnosticConstants.Get(), 0, nullptr, &diagnostics, 0, 0);
        ContextState restore(context);
        restore.PrepareForDraw();
        ID3D11RenderTargetView *rtvs[] = {targets.color, targets.depth, targets.motion, targets.confidence};
        ID3D11ShaderResourceView *srvs[] = {sources.color, sources.depth, sources.motion, sources.confidence};
        ID3D11SamplerState *samplers[] = {linearSampler.Get(), pointSampler.Get()};
        ID3D11Buffer *constantBuffers[] = {
            constants.Get(), inputConstants.Get(), diagnosticConstants.Get()};
        const D3D11_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f};
        const D3D11_RECT scissor{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};

        context->OMSetRenderTargets(4, rtvs, nullptr);
        const FLOAT blendFactor[4]{};
        context->OMSetBlendState(blend.Get(), blendFactor, 0xffffffffu);
        context->OMSetDepthStencilState(depthStencil.Get(), 0);
        context->RSSetState(rasterizer.Get());
        context->RSSetViewports(1, &viewport);
        context->RSSetScissorRects(1, &scissor);
        context->IASetInputLayout(nullptr);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(fullscreenVertex.Get(), nullptr, 0);
        context->HSSetShader(nullptr, nullptr, 0);
        context->DSSetShader(nullptr, nullptr, 0);
        context->GSSetShader(nullptr, nullptr, 0);
        context->VSSetConstantBuffers(0, 3, constantBuffers);
        context->PSSetShader(shader, nullptr, 0);
        context->PSSetConstantBuffers(0, 3, constantBuffers);
        context->PSSetShaderResources(0, 4, srvs);
        context->PSSetSamplers(0, 2, samplers);
        context->SetPredication(nullptr, FALSE);
        context->Draw(3, 0);
        ID3D11ShaderResourceView *nullSrvs[4]{};
        context->PSSetShaderResources(0, 4, nullSrvs);
        return AdapterStatus::Ok;
    }
};

D3D11Adapter::D3D11Adapter() : impl_(std::make_unique<Impl>()) {}
D3D11Adapter::~D3D11Adapter() = default;
D3D11Adapter::D3D11Adapter(D3D11Adapter &&) noexcept = default;
D3D11Adapter &D3D11Adapter::operator=(D3D11Adapter &&) noexcept = default;

AdapterStatus D3D11Adapter::Initialize(ID3D11Device *device, const LayoutV1 &layout,
                                       DXGI_FORMAT colorFormat, const ShaderSet &shaders)
{
    if (ValidateLayout(layout) != Status::Ok) {
        Shutdown();
        return AdapterStatus::InvalidArgument;
    }
    return InitializeInternal(device, AdapterLayoutFromV1(layout), &layout,
                              colorFormat, shaders);
}

AdapterStatus D3D11Adapter::Initialize(ID3D11Device *device, const LayoutV2 &layout,
                                       DXGI_FORMAT colorFormat, const ShaderSet &shaders)
{
    if (ValidateLayout(layout) != Status::Ok) {
        Shutdown();
        return AdapterStatus::InvalidArgument;
    }
    return InitializeInternal(device, layout, nullptr, colorFormat, shaders);
}

AdapterStatus D3D11Adapter::InitializeInternal(
    ID3D11Device *device, const LayoutV2 &layout, const LayoutV1 *legacyLayout,
    DXGI_FORMAT colorFormat, const ShaderSet &shaders)
{
    Shutdown();
    if (device == nullptr) return AdapterStatus::InvalidArgument;
    if (!SupportedColorFormat(colorFormat)) return AdapterStatus::UnsupportedFormat;
    if (!IsValid(shaders.fullscreenVertex) || !IsValid(shaders.packPixel) || !IsValid(shaders.unpackPixel))
        return AdapterStatus::ShaderBytecodeMissing;

    auto next = std::make_unique<Impl>();
    next->device = device;
    next->layout = layout;
    if (legacyLayout != nullptr) {
        next->legacyLayout = *legacyLayout;
        next->hasLegacyLayout = true;
    }
    next->colorFormat = colorFormat;
    if (FAILED(device->CreateVertexShader(shaders.fullscreenVertex.data, shaders.fullscreenVertex.size, nullptr,
                                          &next->fullscreenVertex)) ||
        FAILED(device->CreatePixelShader(shaders.packPixel.data, shaders.packPixel.size, nullptr, &next->packPixel)) ||
        FAILED(device->CreatePixelShader(shaders.unpackPixel.data, shaders.unpackPixel.size, nullptr, &next->unpackPixel)))
        return AdapterStatus::DeviceError;
    if (IsValid(shaders.outlinePixel) &&
        FAILED(device->CreatePixelShader(shaders.outlinePixel.data, shaders.outlinePixel.size,
                                         nullptr, &next->outlinePixel)))
        return AdapterStatus::DeviceError;

    const ShaderConstantsV2 constants = BuildShaderConstants(layout);
    D3D11_BUFFER_DESC cbDesc{};
    cbDesc.ByteWidth = sizeof(constants);
    cbDesc.Usage = D3D11_USAGE_IMMUTABLE;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA cbData{&constants, 0, 0};
    if (FAILED(device->CreateBuffer(&cbDesc, &cbData, &next->constants))) return AdapterStatus::DeviceError;

    const InputDescriptionV2 defaultInput = DefaultInputDescriptionV2(
        layout.nativeWidth, layout.nativeHeight);
    const InputResourceExtentsV2 defaultExtents{
        layout.nativeWidth, layout.nativeHeight, layout.nativeWidth, layout.nativeHeight,
        layout.nativeWidth, layout.nativeHeight, layout.nativeWidth, layout.nativeHeight};
    ShaderInputConstantsV2 defaultInputConstants{};
    if (BuildShaderInputConstantsV2(defaultInput, defaultExtents, &defaultInputConstants) != Status::Ok)
        return AdapterStatus::InvalidArgument;
    cbDesc.ByteWidth = sizeof(defaultInputConstants);
    cbDesc.Usage = D3D11_USAGE_DEFAULT;
    cbData.pSysMem = &defaultInputConstants;
    if (FAILED(device->CreateBuffer(&cbDesc, &cbData, &next->inputConstants)))
        return AdapterStatus::DeviceError;

    const DiagnosticConstants defaultDiagnostics{};
    cbDesc.ByteWidth = sizeof(defaultDiagnostics);
    cbDesc.Usage = D3D11_USAGE_DEFAULT;
    cbData.pSysMem = &defaultDiagnostics;
    if (FAILED(device->CreateBuffer(&cbDesc, &cbData, &next->diagnosticConstants)))
        return AdapterStatus::DeviceError;

    D3D11_SAMPLER_DESC sampler{};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(device->CreateSamplerState(&sampler, &next->linearSampler))) return AdapterStatus::DeviceError;
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    if (FAILED(device->CreateSamplerState(&sampler, &next->pointSampler))) return AdapterStatus::DeviceError;

    D3D11_RASTERIZER_DESC rasterizer{};
    rasterizer.FillMode = D3D11_FILL_SOLID;
    rasterizer.CullMode = D3D11_CULL_NONE;
    rasterizer.ScissorEnable = TRUE;
    rasterizer.DepthClipEnable = TRUE;
    if (FAILED(device->CreateRasterizerState(&rasterizer, &next->rasterizer))) return AdapterStatus::DeviceError;
    D3D11_BLEND_DESC blend{};
    for (auto &target : blend.RenderTarget) target.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(device->CreateBlendState(&blend, &next->blend))) return AdapterStatus::DeviceError;
    D3D11_DEPTH_STENCIL_DESC depthStencil{};
    depthStencil.DepthEnable = FALSE;
    depthStencil.StencilEnable = FALSE;
    if (FAILED(device->CreateDepthStencilState(&depthStencil, &next->depthStencil))) return AdapterStatus::DeviceError;

    const DXGI_FORMAT formats[] = {colorFormat, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R16_FLOAT};
    for (std::size_t i = 0; i < next->packedTextures.size(); ++i) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = layout.workWidth;
        desc.Height = layout.workHeight;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = formats[i];
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        if (FAILED(device->CreateTexture2D(&desc, nullptr, &next->packedTextures[i])) ||
            FAILED(device->CreateShaderResourceView(next->packedTextures[i].Get(), nullptr, &next->packedSrvs[i])) ||
            FAILED(device->CreateRenderTargetView(next->packedTextures[i].Get(), nullptr, &next->packedRtvs[i])))
            return AdapterStatus::DeviceError;
    }
    impl_ = std::move(next);
    return AdapterStatus::Ok;
}

void D3D11Adapter::Shutdown() noexcept
{
    if (impl_) *impl_ = Impl{};
}

AdapterStatus D3D11Adapter::Pack(ID3D11DeviceContext *context, const D3D11SourceViews &sources) noexcept
{
    InputDescriptionV2 description = DefaultInputDescriptionV2(
        impl_->layout.nativeWidth, impl_->layout.nativeHeight);
    if ((impl_->layout.flags & ConfigFlagInputConfidenceValid) != 0)
        description.flags |= InputFlagConfidenceValid;
    return PackV2(context, sources, description);
}

AdapterStatus D3D11Adapter::PackV2(ID3D11DeviceContext *context,
                                   const D3D11SourceViews &sources,
                                   const InputDescriptionV2 &description) noexcept
{
    if (!impl_->device) return AdapterStatus::NotInitialized;
    AdapterStatus status = ValidateContext(context, impl_->device.Get());
    if (status != AdapterStatus::Ok) return status;
    if (ValidateInputDescriptionV2(description) != Status::Ok)
        return AdapterStatus::InvalidArgument;
    if (description.colorRect.width != impl_->layout.nativeWidth ||
        description.colorRect.height != impl_->layout.nativeHeight)
        return AdapterStatus::ResourceMismatch;

    const bool confidenceRequired = (description.flags & InputFlagConfidenceValid) != 0;
    ID3D11ShaderResourceView *views[] = {
        sources.color, sources.depth, sources.motion, sources.confidence};
    DXGI_FORMAT formats[4]{};
    std::uint32_t widths[4]{};
    std::uint32_t heights[4]{};
    for (std::size_t i = 0; i < std::size(views); ++i) {
        status = DescribeSourceView(views[i], impl_->device.Get(), i < 3 || confidenceRequired,
                                    &formats[i], &widths[i], &heights[i]);
        if (status != AdapterStatus::Ok) return status;
    }
    if (!SupportedColorFormat(formats[0]) || !SupportedDepthView(formats[1]) ||
        !SupportedMotionView(formats[2]) ||
        (sources.confidence != nullptr && !SupportedConfidenceView(formats[3])))
        return AdapterStatus::UnsupportedFormat;

    const InputResourceExtentsV2 extents{
        widths[0], heights[0], widths[1], heights[1], widths[2], heights[2],
        sources.confidence != nullptr ? widths[3] : 1u,
        sources.confidence != nullptr ? heights[3] : 1u};
    ShaderInputConstantsV2 inputConstants{};
    if (BuildShaderInputConstantsV2(description, extents, &inputConstants) != Status::Ok)
        return AdapterStatus::ResourceMismatch;
    context->UpdateSubresource(impl_->inputConstants.Get(), 0, nullptr, &inputConstants, 0, 0);

    const D3D11TargetViews targets{impl_->packedRtvs[0].Get(), impl_->packedRtvs[1].Get(),
                                   impl_->packedRtvs[2].Get(), impl_->packedRtvs[3].Get()};
    return impl_->Draw(context, impl_->packPixel.Get(), sources, targets, impl_->layout.workWidth, impl_->layout.workHeight);
}

AdapterStatus D3D11Adapter::Unpack(ID3D11DeviceContext *context, const D3D11SourceViews &packedSources,
                                   const D3D11TargetViews &nativeTargets) noexcept
{
    return Unpack(context, packedSources, nativeTargets, DiagnosticOutlineNone);
}

AdapterStatus D3D11Adapter::Unpack(
    ID3D11DeviceContext *context, const D3D11SourceViews &packedSources,
    const D3D11TargetViews &nativeTargets,
    DiagnosticOutlineFlags diagnosticOutlines) noexcept
{
    if (!impl_->device) return AdapterStatus::NotInitialized;
    if (!ValidDiagnosticOutlineFlags(diagnosticOutlines)) return AdapterStatus::InvalidArgument;
    AdapterStatus status = ValidateContext(context, impl_->device.Get());
    if (status != AdapterStatus::Ok) return status;
    const struct { ID3D11ShaderResourceView *view; DXGI_FORMAT format; bool required; } sources[] = {
        {packedSources.color, impl_->colorFormat, true},
        {packedSources.depth, DXGI_FORMAT_R32_FLOAT, true},
        {packedSources.motion, DXGI_FORMAT_R16G16_FLOAT, true},
        {packedSources.confidence, DXGI_FORMAT_R16_FLOAT, nativeTargets.confidence != nullptr},
    };
    for (const auto &entry : sources) {
        status = ValidateSourceViewExact(entry.view, impl_->device.Get(), impl_->layout.workWidth,
                                         impl_->layout.workHeight, entry.format, entry.required);
        if (status != AdapterStatus::Ok) return status;
    }
    const struct { ID3D11RenderTargetView *view; DXGI_FORMAT format; bool required; } targets[] = {
        {nativeTargets.color, impl_->colorFormat, true},
        {nativeTargets.depth, DXGI_FORMAT_R32_FLOAT, false},
        {nativeTargets.motion, DXGI_FORMAT_R16G16_FLOAT, false},
        {nativeTargets.confidence, DXGI_FORMAT_R16_FLOAT, false},
    };
    for (const auto &entry : targets) {
        status = ValidateTargetView(entry.view, impl_->device.Get(), impl_->layout.nativeWidth,
                                    impl_->layout.nativeHeight, entry.format, entry.required);
        if (status != AdapterStatus::Ok) return status;
    }
    return impl_->Draw(context, impl_->unpackPixel.Get(), packedSources, nativeTargets,
                       impl_->layout.nativeWidth, impl_->layout.nativeHeight,
                       diagnosticOutlines);
}

AdapterStatus D3D11Adapter::DrawOutlines(
    ID3D11DeviceContext *context, ID3D11RenderTargetView *nativeColorTarget,
    DiagnosticOutlineFlags diagnosticOutlines) noexcept
{
    if (!impl_->device) return AdapterStatus::NotInitialized;
    if (!ValidDiagnosticOutlineFlags(diagnosticOutlines)) return AdapterStatus::InvalidArgument;
    if (!impl_->outlinePixel) return AdapterStatus::ShaderBytecodeMissing;
    AdapterStatus status = ValidateContext(context, impl_->device.Get());
    if (status != AdapterStatus::Ok) return status;
    status = ValidateTargetView(nativeColorTarget, impl_->device.Get(),
                                impl_->layout.nativeWidth, impl_->layout.nativeHeight,
                                impl_->colorFormat, true);
    if (status != AdapterStatus::Ok) return status;
    if (diagnosticOutlines == DiagnosticOutlineNone) return AdapterStatus::Ok;
    const D3D11SourceViews sources{};
    const D3D11TargetViews targets{nativeColorTarget, nullptr, nullptr, nullptr};
    return impl_->Draw(context, impl_->outlinePixel.Get(), sources, targets,
                       impl_->layout.nativeWidth, impl_->layout.nativeHeight,
                       diagnosticOutlines);
}

AdapterStatus D3D11Adapter::DrawCenterOutline(
    ID3D11DeviceContext *context, ID3D11RenderTargetView *nativeColorTarget) noexcept
{
    return DrawOutlines(context, nativeColorTarget, DiagnosticOutlineCenter);
}

D3D11PackedViews D3D11Adapter::PackedViews() const noexcept
{
    return {impl_->packedSrvs[0].Get(), impl_->packedSrvs[1].Get(),
            impl_->packedSrvs[2].Get(), impl_->packedSrvs[3].Get()};
}

const LayoutV1 *D3D11Adapter::Layout() const noexcept
{
    return impl_->device && impl_->hasLegacyLayout ? &impl_->legacyLayout : nullptr;
}

const LayoutV2 *D3D11Adapter::LayoutV2Description() const noexcept
{
    return impl_->device ? &impl_->layout : nullptr;
}

AdapterStatus D3D11Adapter::SetOutputColorAdjust(float gain, float gamma) noexcept
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
