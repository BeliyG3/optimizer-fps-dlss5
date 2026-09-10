#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "d3d11_adapter.h"

#include <DirectXPackedVector.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <wrl/client.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

int failures = 0;

void Check(bool condition, const char *message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

std::vector<char> ReadBinary(const char *path)
{
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) return {};
    const std::streamsize size = stream.tellg();
    if (size <= 0) return {};
    std::vector<char> bytes(static_cast<std::size_t>(size));
    stream.seekg(0);
    stream.read(bytes.data(), size);
    return stream ? bytes : std::vector<char>{};
}

struct TextureViews {
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11RenderTargetView> rtv;
};

struct UavTexture {
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11UnorderedAccessView> uav;
};

TextureViews CreateTexture(ID3D11Device *device, std::uint32_t width,
                           std::uint32_t height, DXGI_FORMAT format)
{
    TextureViews result;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &result.texture)) ||
        FAILED(device->CreateShaderResourceView(result.texture.Get(), nullptr, &result.srv)) ||
        FAILED(device->CreateRenderTargetView(result.texture.Get(), nullptr, &result.rtv))) {
        return {};
    }
    return result;
}

UavTexture CreateUavTexture(ID3D11Device *device, std::uint32_t width, std::uint32_t height)
{
    UavTexture result;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R32_UINT;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &result.texture)) ||
        FAILED(device->CreateUnorderedAccessView(result.texture.Get(), nullptr, &result.uav)))
        return {};
    return result;
}

bool DescribeTexture(ID3D11ShaderResourceView *view, D3D11_TEXTURE2D_DESC *description)
{
    if (view == nullptr || description == nullptr) return false;
    ComPtr<ID3D11Resource> resource;
    view->GetResource(&resource);
    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(resource.As(&texture))) return false;
    texture->GetDesc(description);
    return true;
}

bool ReadColorPixel(ID3D11Device *device, ID3D11DeviceContext *context,
                    ID3D11Texture2D *source, std::uint32_t x, std::uint32_t y,
                    std::array<std::uint8_t, 4> *pixel)
{
    if (device == nullptr || context == nullptr || source == nullptr || pixel == nullptr)
        return false;
    D3D11_TEXTURE2D_DESC sourceDesc{};
    source->GetDesc(&sourceDesc);
    if (sourceDesc.Format != DXGI_FORMAT_R8G8B8A8_UNORM ||
        x >= sourceDesc.Width || y >= sourceDesc.Height)
        return false;

    D3D11_TEXTURE2D_DESC stagingDesc = sourceDesc;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDesc.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device->CreateTexture2D(&stagingDesc, nullptr, &staging))) return false;
    context->CopyResource(staging.Get(), source);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return false;
    const auto *bytes = static_cast<const std::uint8_t *>(mapped.pData) +
                        static_cast<std::size_t>(y) * mapped.RowPitch +
                        static_cast<std::size_t>(x) * 4u;
    *pixel = {bytes[0], bytes[1], bytes[2], bytes[3]};
    context->Unmap(staging.Get(), 0);
    return true;
}

bool ReadRawPixel(ID3D11Device *device, ID3D11DeviceContext *context,
                  ID3D11Texture2D *source, std::uint32_t x, std::uint32_t y,
                  void *pixel, std::size_t pixelSize)
{
    if (device == nullptr || context == nullptr || source == nullptr || pixel == nullptr)
        return false;
    D3D11_TEXTURE2D_DESC sourceDesc{};
    source->GetDesc(&sourceDesc);
    if (x >= sourceDesc.Width || y >= sourceDesc.Height) return false;

    D3D11_TEXTURE2D_DESC stagingDesc = sourceDesc;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDesc.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device->CreateTexture2D(&stagingDesc, nullptr, &staging))) return false;
    context->CopyResource(staging.Get(), source);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return false;
    const auto *bytes = static_cast<const std::uint8_t *>(mapped.pData) +
                        static_cast<std::size_t>(y) * mapped.RowPitch +
                        static_cast<std::size_t>(x) * pixelSize;
    std::memcpy(pixel, bytes, pixelSize);
    context->Unmap(staging.Get(), 0);
    return true;
}

bool ReadFloatPixel(ID3D11Device *device, ID3D11DeviceContext *context,
                    ID3D11Texture2D *source, std::uint32_t x, std::uint32_t y,
                    float *pixel)
{
    return ReadRawPixel(device, context, source, x, y, pixel, sizeof(*pixel));
}

bool ReadHalfPixel(ID3D11Device *device, ID3D11DeviceContext *context,
                   ID3D11Texture2D *source, std::uint32_t x, std::uint32_t y,
                   float *pixel)
{
    std::uint16_t encoded = 0;
    if (!ReadRawPixel(device, context, source, x, y, &encoded, sizeof(encoded))) return false;
    *pixel = DirectX::PackedVector::XMConvertHalfToFloat(encoded);
    return true;
}

bool ReadHalf2Pixel(ID3D11Device *device, ID3D11DeviceContext *context,
                    ID3D11Texture2D *source, std::uint32_t x, std::uint32_t y,
                    std::array<float, 2> *pixel)
{
    std::array<std::uint16_t, 2> encoded{};
    if (!ReadRawPixel(device, context, source, x, y, encoded.data(), sizeof(encoded))) return false;
    (*pixel)[0] = DirectX::PackedVector::XMConvertHalfToFloat(encoded[0]);
    (*pixel)[1] = DirectX::PackedVector::XMConvertHalfToFloat(encoded[1]);
    return true;
}

bool NearByte(std::uint8_t value, std::uint8_t expected)
{
    const int delta = static_cast<int>(value) - static_cast<int>(expected);
    return delta >= -2 && delta <= 2;
}

} // namespace

int main(int argc, char **argv)
{
    if (argc != 4) {
        std::cerr << "Expected fullscreen, pack, and unpack DXBC paths\n";
        return EXIT_FAILURE;
    }
    const std::vector<char> vertex = ReadBinary(argv[1]);
    const std::vector<char> pack = ReadBinary(argv[2]);
    const std::vector<char> unpack = ReadBinary(argv[3]);
    Check(!vertex.empty() && !pack.empty() && !unpack.empty(),
          "compiled DXBC fixtures are readable");
    if (failures != 0) return EXIT_FAILURE;

    constexpr D3D_FEATURE_LEVEL requestedLevels[] = {D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL actualLevel{};
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    Check(SUCCEEDED(D3D11CreateDevice(
              nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, requestedLevels,
              static_cast<UINT>(std::size(requestedLevels)), D3D11_SDK_VERSION,
              &device, &actualLevel, &context)),
          "D3D11 WARP device is available");
    Check(actualLevel == D3D_FEATURE_LEVEL_11_0, "D3D11 WARP exposes feature level 11_0");
    if (!device || !context) return EXIT_FAILURE;

    const pw::ShaderSet shaders{
        {vertex.data(), vertex.size()}, {pack.data(), pack.size()}, {unpack.data(), unpack.size()}};
    pw::LayoutV1 layout{};
    pw::ConfigV1 config = pw::DefaultConfigV1();
    config.flags |= pw::ConfigFlagInputConfidenceValid;
    Check(pw::BuildLayout(config, 320, 180, &layout) == pw::Status::Ok,
          "smoke-test layout builds");

    pw::D3D11Adapter adapter;
    const pw::D3D11SourceViews emptySources{};
    Check(adapter.Pack(context.Get(), emptySources) == pw::AdapterStatus::NotInitialized,
          "Pack rejects use before initialization");
    pw::LayoutV1 invalidLayout = layout;
    invalidLayout.edgeSlopeX = 0.75f;
    Check(adapter.Initialize(device.Get(), invalidLayout, DXGI_FORMAT_R8G8B8A8_UNORM, shaders) ==
              pw::AdapterStatus::InvalidArgument,
          "adapter rejects a noncanonical layout");
    Check(adapter.Initialize(device.Get(), layout, DXGI_FORMAT_R32_UINT, shaders) ==
              pw::AdapterStatus::UnsupportedFormat,
          "adapter rejects an unsupported color format");
    pw::ShaderSet missingShader = shaders;
    missingShader.packPixel = {};
    Check(adapter.Initialize(device.Get(), layout, DXGI_FORMAT_R8G8B8A8_UNORM, missingShader) ==
              pw::AdapterStatus::ShaderBytecodeMissing,
          "adapter rejects missing shader bytecode");
    Check(adapter.Initialize(device.Get(), layout, DXGI_FORMAT_R8G8B8A8_UNORM, shaders) ==
              pw::AdapterStatus::Ok,
          "adapter initializes on D3D11 WARP");

    const pw::D3D11PackedViews packed = adapter.PackedViews();
    Check(packed.color != nullptr && packed.depth != nullptr && packed.motion != nullptr &&
              packed.confidence != nullptr,
          "adapter owns all four packed views");
    D3D11_TEXTURE2D_DESC packedColorDesc{};
    Check(DescribeTexture(packed.color, &packedColorDesc), "packed color view owns a 2D texture");
    Check(packedColorDesc.Width == layout.workWidth && packedColorDesc.Height == layout.workHeight &&
              packedColorDesc.Format == DXGI_FORMAT_R8G8B8A8_UNORM,
          "owned color texture matches work dimensions and target format");

    pw::ConfigV2 scaledConfig = pw::DefaultConfigV2();
    scaledConfig.globalScalePercent = 50.0f;
    pw::LayoutV2 scaledLayout{};
    Check(pw::BuildLayout(scaledConfig, 320, 180, &scaledLayout) == pw::Status::Ok,
          "scaled adapter layout builds");
    pw::D3D11Adapter scaledAdapter;
    Check(scaledAdapter.Initialize(device.Get(), scaledLayout,
                                   DXGI_FORMAT_R8G8B8A8_UNORM, shaders) ==
              pw::AdapterStatus::Ok,
          "adapter initializes directly from LayoutV2");
    D3D11_TEXTURE2D_DESC scaledPackedDesc{};
    Check(DescribeTexture(scaledAdapter.PackedViews().color, &scaledPackedDesc) &&
              scaledPackedDesc.Width == scaledLayout.workWidth &&
              scaledPackedDesc.Height == scaledLayout.workHeight,
          "LayoutV2 allocates packed resources at the actual NR extent");
    Check(scaledAdapter.Layout() == nullptr &&
              scaledAdapter.LayoutV2Description() != nullptr &&
              scaledAdapter.LayoutV2Description()->rawWorkWidth == scaledLayout.rawWorkWidth,
          "LayoutV2 initialization does not expose a misleading legacy layout");

    const TextureViews sourceColor = CreateTexture(
        device.Get(), layout.nativeWidth, layout.nativeHeight, DXGI_FORMAT_R8G8B8A8_UNORM);
    const TextureViews sourceDepth = CreateTexture(
        device.Get(), layout.nativeWidth, layout.nativeHeight, DXGI_FORMAT_R32_FLOAT);
    const TextureViews sourceMotion = CreateTexture(
        device.Get(), layout.nativeWidth, layout.nativeHeight, DXGI_FORMAT_R16G16_FLOAT);
    const TextureViews sourceConfidence = CreateTexture(
        device.Get(), layout.nativeWidth, layout.nativeHeight, DXGI_FORMAT_R16_FLOAT);
    const TextureViews outputColor = CreateTexture(
        device.Get(), layout.nativeWidth, layout.nativeHeight, DXGI_FORMAT_R8G8B8A8_UNORM);
    const TextureViews outputDepth = CreateTexture(
        device.Get(), layout.nativeWidth, layout.nativeHeight, DXGI_FORMAT_R32_FLOAT);
    const TextureViews outputMotion = CreateTexture(
        device.Get(), layout.nativeWidth, layout.nativeHeight, DXGI_FORMAT_R16G16_FLOAT);
    const TextureViews outputConfidence = CreateTexture(
        device.Get(), layout.nativeWidth, layout.nativeHeight, DXGI_FORMAT_R16_FLOAT);
    Check(sourceColor.texture && sourceDepth.texture && sourceMotion.texture &&
              sourceConfidence.texture && outputColor.texture && outputDepth.texture &&
              outputMotion.texture && outputConfidence.texture,
          "native source and target fixtures allocate");
    if (failures != 0) return EXIT_FAILURE;

    const TextureViews sourceDepth16 = CreateTexture(
        device.Get(), 160, 90, DXGI_FORMAT_R16_UNORM);
    const TextureViews sourceMotion32 = CreateTexture(
        device.Get(), 160, 90, DXGI_FORMAT_R32G32_FLOAT);
    pw::InputDescriptionV2 inputV2 = pw::DefaultInputDescriptionV2(
        layout.nativeWidth, layout.nativeHeight);
    inputV2.depthRect = {0, 0, 160, 90};
    inputV2.motionRect = {0, 0, 160, 90};
    inputV2.confidenceRect = {0, 0, 1, 1};
    inputV2.motionScaleX = 320.0f;
    inputV2.motionScaleY = 180.0f;
    inputV2.motionDirection = pw::MotionDirection::PreviousToCurrent;
    inputV2.depthConvention = pw::DepthConvention::Reversed;
    inputV2.colorEncoding = pw::ColorEncoding::Srgb;
    const pw::D3D11SourceViews universalSources{
        sourceColor.srv.Get(), sourceDepth16.srv.Get(), sourceMotion32.srv.Get(), nullptr};
    Check(adapter.PackV2(context.Get(), universalSources, inputV2) == pw::AdapterStatus::Ok,
          "ABI-v2 accepts R16 depth, RG32 motion, and half-resolution guides");
    inputV2.motionScaleX = 0.0f;
    Check(adapter.PackV2(context.Get(), universalSources, inputV2) ==
              pw::AdapterStatus::InvalidArgument,
          "ABI-v2 rejects an ambiguous motion scale before recording GPU work");

    const TextureViews wrongExtent = CreateTexture(
        device.Get(), layout.nativeWidth - 2u, layout.nativeHeight, DXGI_FORMAT_R8G8B8A8_UNORM);
    pw::D3D11SourceViews mismatchedSources{
        wrongExtent.srv.Get(), sourceDepth.srv.Get(), sourceMotion.srv.Get(), sourceConfidence.srv.Get()};
    Check(adapter.Pack(context.Get(), mismatchedSources) == pw::AdapterStatus::ResourceMismatch,
          "Pack rejects a source with the wrong native extent");
    mismatchedSources.color = sourceColor.srv.Get();
    mismatchedSources.motion = sourceDepth.srv.Get();
    Check(adapter.Pack(context.Get(), mismatchedSources) == pw::AdapterStatus::UnsupportedFormat,
          "Pack rejects a motion view with the wrong format");

    std::array<TextureViews, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> savedTargets{};
    std::array<ID3D11RenderTargetView *, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> savedTargetViews{};
    for (std::size_t i = 0; i < savedTargets.size(); ++i) {
        savedTargets[i] = CreateTexture(
            device.Get(), layout.nativeWidth, layout.nativeHeight, DXGI_FORMAT_R8G8B8A8_UNORM);
        savedTargetViews[i] = savedTargets[i].rtv.Get();
    }
    context->OMSetRenderTargets(static_cast<UINT>(savedTargetViews.size()), savedTargetViews.data(), nullptr);

    ComPtr<ID3D11DeviceContext1> context1;
    ComPtr<ID3D11Buffer> rangedConstantBuffer;
    UINT expectedFirstConstant = 16;
    UINT expectedConstantCount = 16;
    if (SUCCEEDED(context.As(&context1))) {
        D3D11_BUFFER_DESC constantDesc{};
        constantDesc.ByteWidth = 1024;
        constantDesc.Usage = D3D11_USAGE_DEFAULT;
        constantDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        Check(SUCCEEDED(device->CreateBuffer(&constantDesc, nullptr, &rangedConstantBuffer)),
              "D3D11.1 ranged constant-buffer fixture allocates");
        ID3D11Buffer *constant = rangedConstantBuffer.Get();
        context1->VSSetConstantBuffers1(0, 1, &constant, &expectedFirstConstant, &expectedConstantCount);
        context1->PSSetConstantBuffers1(0, 1, &constant, &expectedFirstConstant, &expectedConstantCount);
    }

    constexpr FLOAT sourceColorValue[4] = {0.25f, 0.5f, 0.75f, 1.0f};
    constexpr FLOAT sourceDepthValue[4] = {0.625f, 0.0f, 0.0f, 0.0f};
    constexpr FLOAT sourceMotionValue[4] = {4.0f, -2.0f, 0.0f, 0.0f};
    constexpr FLOAT sourceConfidenceValue[4] = {0.875f, 0.0f, 0.0f, 0.0f};
    context->ClearRenderTargetView(sourceColor.rtv.Get(), sourceColorValue);
    context->ClearRenderTargetView(sourceDepth.rtv.Get(), sourceDepthValue);
    context->ClearRenderTargetView(sourceMotion.rtv.Get(), sourceMotionValue);
    context->ClearRenderTargetView(sourceConfidence.rtv.Get(), sourceConfidenceValue);

    const pw::D3D11SourceViews nativeSources{
        sourceColor.srv.Get(), sourceDepth.srv.Get(), sourceMotion.srv.Get(),
        sourceConfidence.srv.Get()};
    Check(adapter.Pack(nullptr, nativeSources) == pw::AdapterStatus::InvalidArgument,
          "Pack rejects a null context");

    constexpr UINT packVertexSlot = 17;
    constexpr UINT packHullSlot = 29;
    constexpr UINT packDomainSlot = 43;
    constexpr UINT packGeometrySlot = 71;
    ID3D11ShaderResourceView *packVertexResource = packed.color;
    ID3D11ShaderResourceView *packHullResource = packed.depth;
    ID3D11ShaderResourceView *packDomainResource = packed.motion;
    ID3D11ShaderResourceView *packGeometryResource = packed.confidence;
    context->VSSetShaderResources(packVertexSlot, 1, &packVertexResource);
    context->HSSetShaderResources(packHullSlot, 1, &packHullResource);
    context->DSSetShaderResources(packDomainSlot, 1, &packDomainResource);
    context->GSSetShaderResources(packGeometrySlot, 1, &packGeometryResource);
    Check(adapter.Pack(context.Get(), nativeSources) == pw::AdapterStatus::Ok,
          "Pack executes with color, depth, motion, and confidence inputs");
    std::array<ID3D11RenderTargetView *, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> restoredTargets{};
    context->OMGetRenderTargets(static_cast<UINT>(restoredTargets.size()), restoredTargets.data(), nullptr);
    for (std::size_t i = 0; i < restoredTargets.size(); ++i) {
        Check(restoredTargets[i] == savedTargetViews[i], "Pack restores every D3D11 render-target slot");
        if (restoredTargets[i] != nullptr) restoredTargets[i]->Release();
    }
    ID3D11ShaderResourceView *restoredVertexResource = nullptr;
    ID3D11ShaderResourceView *restoredHullResource = nullptr;
    ID3D11ShaderResourceView *restoredDomainResource = nullptr;
    ID3D11ShaderResourceView *restoredGeometryResource = nullptr;
    context->VSGetShaderResources(packVertexSlot, 1, &restoredVertexResource);
    context->HSGetShaderResources(packHullSlot, 1, &restoredHullResource);
    context->DSGetShaderResources(packDomainSlot, 1, &restoredDomainResource);
    context->GSGetShaderResources(packGeometrySlot, 1, &restoredGeometryResource);
    Check(restoredVertexResource == packed.color && restoredHullResource == packed.depth &&
              restoredDomainResource == packed.motion &&
              restoredGeometryResource == packed.confidence,
          "Pack restores RTV-aliased SRVs across VS, HS, DS, and GS slots outside PS t0-t3");
    if (restoredVertexResource != nullptr) restoredVertexResource->Release();
    if (restoredHullResource != nullptr) restoredHullResource->Release();
    if (restoredDomainResource != nullptr) restoredDomainResource->Release();
    if (restoredGeometryResource != nullptr) restoredGeometryResource->Release();
    ID3D11ShaderResourceView *nullResource = nullptr;
    context->VSSetShaderResources(packVertexSlot, 1, &nullResource);
    context->HSSetShaderResources(packHullSlot, 1, &nullResource);
    context->DSSetShaderResources(packDomainSlot, 1, &nullResource);
    context->GSSetShaderResources(packGeometrySlot, 1, &nullResource);
    if (context1 && rangedConstantBuffer) {
        ID3D11Buffer *restoredVsConstant = nullptr;
        ID3D11Buffer *restoredPsConstant = nullptr;
        UINT restoredVsFirst = 0;
        UINT restoredVsCount = 0;
        UINT restoredPsFirst = 0;
        UINT restoredPsCount = 0;
        context1->VSGetConstantBuffers1(
            0, 1, &restoredVsConstant, &restoredVsFirst, &restoredVsCount);
        context1->PSGetConstantBuffers1(
            0, 1, &restoredPsConstant, &restoredPsFirst, &restoredPsCount);
        Check(restoredVsConstant == rangedConstantBuffer.Get() &&
                  restoredPsConstant == rangedConstantBuffer.Get() &&
                  restoredVsFirst == expectedFirstConstant && restoredPsFirst == expectedFirstConstant &&
                  restoredVsCount == expectedConstantCount && restoredPsCount == expectedConstantCount,
              "Pack restores D3D11.1 VS/PS constant-buffer ranges");
        if (restoredVsConstant != nullptr) restoredVsConstant->Release();
        if (restoredPsConstant != nullptr) restoredPsConstant->Release();
    }
    context->OMSetRenderTargets(0, nullptr, nullptr);

    const UavTexture omUav = CreateUavTexture(device.Get(), layout.nativeWidth, layout.nativeHeight);
    const UavTexture csUav = CreateUavTexture(device.Get(), layout.nativeWidth, layout.nativeHeight);
    Check(omUav.uav && csUav.uav, "OM and CS UAV state fixtures allocate");
    ID3D11RenderTargetView *oneTarget = savedTargetViews[0];
    ID3D11UnorderedAccessView *omUavView = omUav.uav.Get();
    ID3D11UnorderedAccessView *csUavView = csUav.uav.Get();
    UINT keepCounter = D3D11_KEEP_UNORDERED_ACCESS_VIEWS;
    context->OMSetRenderTargetsAndUnorderedAccessViews(
        1, &oneTarget, nullptr, 1, 1, &omUavView, &keepCounter);
    context->CSSetUnorderedAccessViews(0, 1, &csUavView, &keepCounter);
    Check(adapter.Pack(context.Get(), nativeSources) == pw::AdapterStatus::Ok,
          "Pack executes while unrelated OM and CS UAVs are bound");
    ID3D11UnorderedAccessView *restoredOmUav = nullptr;
    ID3D11UnorderedAccessView *restoredCsUav = nullptr;
    context->OMGetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 1, 1, &restoredOmUav);
    context->CSGetUnorderedAccessViews(0, 1, &restoredCsUav);
    Check(restoredOmUav == omUav.uav.Get() && restoredCsUav == csUav.uav.Get(),
          "Pack restores unrelated OM and CS UAV bindings");
    if (restoredOmUav != nullptr) restoredOmUav->Release();
    if (restoredCsUav != nullptr) restoredCsUav->Release();
    ID3D11UnorderedAccessView *nullUav = nullptr;
    context->OMSetRenderTargetsAndUnorderedAccessViews(
        0, nullptr, nullptr, 0, 1, &nullUav, &keepCounter);
    context->CSSetUnorderedAccessViews(0, 1, &nullUav, &keepCounter);

    const pw::D3D11SourceViews packedSources{
        packed.color, packed.depth, packed.motion, packed.confidence};
    const pw::D3D11TargetViews nativeTargets{
        outputColor.rtv.Get(), outputDepth.rtv.Get(), outputMotion.rtv.Get(),
        outputConfidence.rtv.Get()};
    pw::D3D11TargetViews missingColor = nativeTargets;
    missingColor.color = nullptr;
    Check(adapter.Unpack(context.Get(), packedSources, missingColor) ==
              pw::AdapterStatus::InvalidArgument,
          "Unpack rejects a missing color target");

    constexpr UINT unpackPixelSlot = 37;
    constexpr UINT unpackComputeSlot = 103;
    ID3D11ShaderResourceView *unpackPixelResource = outputColor.srv.Get();
    ID3D11ShaderResourceView *unpackComputeResource = outputDepth.srv.Get();
    context->PSSetShaderResources(unpackPixelSlot, 1, &unpackPixelResource);
    context->CSSetShaderResources(unpackComputeSlot, 1, &unpackComputeResource);
    Check(adapter.Unpack(context.Get(), packedSources, nativeTargets) == pw::AdapterStatus::Ok,
          "Unpack executes to native color, depth, motion, and confidence targets");
    ID3D11ShaderResourceView *restoredPixelResource = nullptr;
    ID3D11ShaderResourceView *restoredComputeResource = nullptr;
    context->PSGetShaderResources(unpackPixelSlot, 1, &restoredPixelResource);
    context->CSGetShaderResources(unpackComputeSlot, 1, &restoredComputeResource);
    Check(restoredPixelResource == outputColor.srv.Get() &&
              restoredComputeResource == outputDepth.srv.Get(),
          "Unpack restores RTV-aliased SRVs across PS and CS slots outside PS t0-t3");
    if (restoredPixelResource != nullptr) restoredPixelResource->Release();
    if (restoredComputeResource != nullptr) restoredComputeResource->Release();
    context->PSSetShaderResources(unpackPixelSlot, 1, &nullResource);
    context->CSSetShaderResources(unpackComputeSlot, 1, &nullResource);
    context->Flush();
    Check(device->GetDeviceRemovedReason() == S_OK, "Pack and Unpack keep the WARP device healthy");

    std::array<std::uint8_t, 4> resultPixel{};
    Check(ReadColorPixel(device.Get(), context.Get(), outputColor.texture.Get(),
                         layout.nativeWidth / 2u, layout.nativeHeight / 2u, &resultPixel),
          "reconstructed color can be read back");
    Check(NearByte(resultPixel[0], 64) && NearByte(resultPixel[1], 128) &&
              NearByte(resultPixel[2], 191) && NearByte(resultPixel[3], 255),
          "constant color survives Pack and Unpack");

    float resultDepth = 0.0f;
    std::array<float, 2> resultMotion{};
    float resultConfidence = 0.0f;
    Check(ReadFloatPixel(device.Get(), context.Get(), outputDepth.texture.Get(),
                         layout.nativeWidth / 2u, layout.nativeHeight / 2u, &resultDepth) &&
              std::abs(resultDepth - sourceDepthValue[0]) < 0.0001f,
          "constant depth survives Pack and Unpack");
    Check(ReadHalf2Pixel(device.Get(), context.Get(), outputMotion.texture.Get(),
                        layout.nativeWidth / 2u, layout.nativeHeight / 2u, &resultMotion) &&
              std::abs(resultMotion[0] - sourceMotionValue[0]) < 0.05f &&
              std::abs(resultMotion[1] - sourceMotionValue[1]) < 0.05f,
          "constant motion survives endpoint Pack and Unpack");
    Check(ReadHalfPixel(device.Get(), context.Get(), outputConfidence.texture.Get(),
                       layout.nativeWidth / 2u, layout.nativeHeight / 2u, &resultConfidence) &&
              std::abs(resultConfidence - sourceConfidenceValue[0]) < 0.005f,
          "constant confidence survives conservative Pack and Unpack");

    std::vector<std::uint8_t> gradient(
        static_cast<std::size_t>(layout.nativeWidth) * layout.nativeHeight * 4u);
    for (std::uint32_t y = 0; y < layout.nativeHeight; ++y) {
        for (std::uint32_t x = 0; x < layout.nativeWidth; ++x) {
            const std::size_t offset =
                (static_cast<std::size_t>(y) * layout.nativeWidth + x) * 4u;
            gradient[offset + 0] = static_cast<std::uint8_t>(x * 255u / (layout.nativeWidth - 1u));
            gradient[offset + 1] = static_cast<std::uint8_t>(y * 255u / (layout.nativeHeight - 1u));
            gradient[offset + 2] = 97u;
            gradient[offset + 3] = 255u;
        }
    }
    context->UpdateSubresource(sourceColor.texture.Get(), 0, nullptr, gradient.data(),
                               layout.nativeWidth * 4u, 0);
    Check(adapter.Pack(context.Get(), nativeSources) == pw::AdapterStatus::Ok &&
              adapter.Unpack(context.Get(), packedSources, nativeTargets) == pw::AdapterStatus::Ok,
          "nonconstant fixture executes through the nonlinear path");
    for (const std::uint32_t x : {layout.nativeWidth / 2u, layout.nativeWidth * 15u / 16u}) {
        const std::uint32_t y = layout.nativeHeight / 2u;
        const std::uint8_t expectedRed = static_cast<std::uint8_t>(x * 255u / (layout.nativeWidth - 1u));
        const std::uint8_t expectedGreen = static_cast<std::uint8_t>(y * 255u / (layout.nativeHeight - 1u));
        Check(ReadColorPixel(device.Get(), context.Get(), outputColor.texture.Get(), x, y, &resultPixel) &&
                  std::abs(static_cast<int>(resultPixel[0]) - expectedRed) <= 4 &&
                  std::abs(static_cast<int>(resultPixel[1]) - expectedGreen) <= 4 &&
                  std::abs(static_cast<int>(resultPixel[2]) - 97) <= 2,
              "gradient reconstructs at center and across the nonlinear peripheral boundary");
    }

    const pw::DiagnosticOutlineFlags bothOutlines =
        pw::DiagnosticOutlineCenter | pw::DiagnosticOutlineRawWork;
    Check(adapter.Unpack(context.Get(), packedSources, nativeTargets, bothOutlines) ==
              pw::AdapterStatus::Ok,
          "one Unpack draw accepts independent Center and raw Work diagnostics");
    const std::uint32_t centerLeft = static_cast<std::uint32_t>(std::lround(
        0.5f * static_cast<float>(layout.nativeWidth) * (1.0f - layout.centerFractionX)));
    const std::uint32_t workLeft = (layout.nativeWidth - layout.workWidth) / 2u;
    Check(ReadColorPixel(device.Get(), context.Get(), outputColor.texture.Get(), centerLeft,
                         layout.nativeHeight / 2u, &resultPixel) &&
              NearByte(resultPixel[0], 0) && NearByte(resultPixel[1], 209) &&
              NearByte(resultPixel[2], 255),
          "integrated Unpack draws the cyan Center boundary at the exact pixel");
    Check(ReadColorPixel(device.Get(), context.Get(), outputColor.texture.Get(), workLeft,
                         layout.nativeHeight / 2u, &resultPixel) &&
              NearByte(resultPixel[0], 255) && NearByte(resultPixel[1], 115) &&
              NearByte(resultPixel[2], 0),
          "the same Unpack draw emits the orange raw Work boundary");
    Check(adapter.Unpack(
              context.Get(), packedSources, nativeTargets,
              static_cast<pw::DiagnosticOutlineFlags>(1u << 7)) ==
              pw::AdapterStatus::InvalidArgument,
          "Unpack rejects unknown diagnostic outline bits");

    if (failures != 0) {
        std::cerr << failures << " D3D11 smoke-test failure(s)\n";
        return EXIT_FAILURE;
    }
    std::cout << "PeripheralWarp D3D11 smoke test passed\n";
    return EXIT_SUCCESS;
}
