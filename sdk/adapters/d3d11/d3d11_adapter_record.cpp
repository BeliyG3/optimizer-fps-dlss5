#include "d3d11_adapter_state.h"

namespace ofps::sdk {

AdapterStatus D3D11Adapter::Impl::Draw(
    ID3D11DeviceContext *context, ID3D11PixelShader *shader,
    const D3D11SourceViews &sources, const D3D11TargetViews &targets,
    std::uint32_t width, std::uint32_t height,
    DiagnosticOutlineFlags diagnosticOutlines) noexcept
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
} // namespace ofps::sdk
