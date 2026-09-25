#include "d3d11_adapter_private.h"

namespace ofps::sdk {

AdapterStatus D3D11Adapter::Impl::CreateResources(const ShaderSet &shaders) noexcept
{
    auto *next = this;
    ID3D11Device *resourceDevice = next->device.Get();
    const LayoutV2 &targetLayout = next->layout;
    DXGI_FORMAT targetColorFormat = next->colorFormat;
    if (FAILED(resourceDevice->CreateVertexShader(shaders.fullscreenVertex.data, shaders.fullscreenVertex.size, nullptr,
                                          &next->fullscreenVertex)) ||
        FAILED(resourceDevice->CreatePixelShader(shaders.packPixel.data, shaders.packPixel.size, nullptr, &next->packPixel)) ||
        FAILED(resourceDevice->CreatePixelShader(shaders.unpackPixel.data, shaders.unpackPixel.size, nullptr, &next->unpackPixel)))
        return AdapterStatus::DeviceError;
    if (IsValid(shaders.outlinePixel) &&
        FAILED(resourceDevice->CreatePixelShader(shaders.outlinePixel.data, shaders.outlinePixel.size,
                                         nullptr, &next->outlinePixel)))
        return AdapterStatus::DeviceError;

    const ShaderConstantsV2 shaderConstants = BuildShaderConstants(targetLayout);
    D3D11_BUFFER_DESC cbDesc{};
    cbDesc.ByteWidth = sizeof(shaderConstants);
    cbDesc.Usage = D3D11_USAGE_IMMUTABLE;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA cbData{&shaderConstants, 0, 0};
    if (FAILED(resourceDevice->CreateBuffer(&cbDesc, &cbData, &next->constants))) return AdapterStatus::DeviceError;

    const InputDescriptionV2 defaultInput = DefaultInputDescriptionV2(
        targetLayout.nativeWidth, targetLayout.nativeHeight);
    const InputResourceExtentsV2 defaultExtents{
        targetLayout.nativeWidth, targetLayout.nativeHeight, targetLayout.nativeWidth, targetLayout.nativeHeight,
        targetLayout.nativeWidth, targetLayout.nativeHeight, targetLayout.nativeWidth, targetLayout.nativeHeight};
    ShaderInputConstantsV2 defaultInputConstants{};
    if (BuildShaderInputConstantsV2(defaultInput, defaultExtents, &defaultInputConstants) != Status::Ok)
        return AdapterStatus::InvalidArgument;
    cbDesc.ByteWidth = sizeof(defaultInputConstants);
    cbDesc.Usage = D3D11_USAGE_DEFAULT;
    cbData.pSysMem = &defaultInputConstants;
    if (FAILED(resourceDevice->CreateBuffer(&cbDesc, &cbData, &next->inputConstants)))
        return AdapterStatus::DeviceError;

    const DiagnosticConstants defaultDiagnostics{};
    cbDesc.ByteWidth = sizeof(defaultDiagnostics);
    cbDesc.Usage = D3D11_USAGE_DEFAULT;
    cbData.pSysMem = &defaultDiagnostics;
    if (FAILED(resourceDevice->CreateBuffer(&cbDesc, &cbData, &next->diagnosticConstants)))
        return AdapterStatus::DeviceError;

    D3D11_SAMPLER_DESC sampler{};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(resourceDevice->CreateSamplerState(&sampler, &next->linearSampler))) return AdapterStatus::DeviceError;
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    if (FAILED(resourceDevice->CreateSamplerState(&sampler, &next->pointSampler))) return AdapterStatus::DeviceError;

    D3D11_RASTERIZER_DESC rasterizerDesc{};
    rasterizerDesc.FillMode = D3D11_FILL_SOLID;
    rasterizerDesc.CullMode = D3D11_CULL_NONE;
    rasterizerDesc.ScissorEnable = TRUE;
    rasterizerDesc.DepthClipEnable = TRUE;
    if (FAILED(resourceDevice->CreateRasterizerState(&rasterizerDesc, &next->rasterizer))) return AdapterStatus::DeviceError;
    D3D11_BLEND_DESC blendDesc{};
    for (auto &target : blendDesc.RenderTarget) target.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(resourceDevice->CreateBlendState(&blendDesc, &next->blend))) return AdapterStatus::DeviceError;
    D3D11_DEPTH_STENCIL_DESC depthStencilDesc{};
    depthStencilDesc.DepthEnable = FALSE;
    depthStencilDesc.StencilEnable = FALSE;
    if (FAILED(resourceDevice->CreateDepthStencilState(&depthStencilDesc, &next->depthStencil))) return AdapterStatus::DeviceError;

    const DXGI_FORMAT formats[] = {targetColorFormat, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R16_FLOAT};
    for (std::size_t i = 0; i < next->packedTextures.size(); ++i) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = targetLayout.workWidth;
        desc.Height = targetLayout.workHeight;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = formats[i];
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        if (FAILED(resourceDevice->CreateTexture2D(&desc, nullptr, &next->packedTextures[i])) ||
            FAILED(resourceDevice->CreateShaderResourceView(next->packedTextures[i].Get(), nullptr, &next->packedSrvs[i])) ||
            FAILED(resourceDevice->CreateRenderTargetView(next->packedTextures[i].Get(), nullptr, &next->packedRtvs[i])))
            return AdapterStatus::DeviceError;
    }
    return AdapterStatus::Ok;
}

void D3D11Adapter::Shutdown() noexcept
{
    if (impl_) *impl_ = Impl{};
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
} // namespace ofps::sdk
