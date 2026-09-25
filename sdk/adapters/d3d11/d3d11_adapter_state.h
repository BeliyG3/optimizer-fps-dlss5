#pragma once
#include "d3d11_adapter_private.h"

namespace ofps::sdk {
namespace {
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
} // namespace
} // namespace ofps::sdk
