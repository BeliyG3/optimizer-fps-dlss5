#include "d3d11_adapter_private.h"

#include <cmath>
#include <algorithm>
#include <array>
#include <d3d11_1.h>
#include <utility>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace ofps::sdk {
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
namespace {

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
    const AdapterStatus resourceStatus = next->CreateResources(shaders);
    if (resourceStatus != AdapterStatus::Ok) return resourceStatus;

    impl_ = std::move(next);
    return AdapterStatus::Ok;
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

} // namespace ofps::sdk
