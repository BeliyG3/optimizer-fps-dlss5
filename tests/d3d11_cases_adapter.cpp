#include "d3d11_cases.h"

namespace d3d11_cases {

void RunAdapterCases(const std::vector<char> &vertex, const std::vector<char> &pack,
                     const std::vector<char> &unpack)
{
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
    if (!device || !context) return;

    const ofps::sdk::ShaderSet shaders{
        {vertex.data(), vertex.size()}, {pack.data(), pack.size()}, {unpack.data(), unpack.size()}};
    ofps::sdk::LayoutV1 layout{};
    ofps::sdk::ConfigV1 config = ofps::sdk::DefaultConfigV1();
    config.flags |= ofps::sdk::ConfigFlagInputConfidenceValid;
    Check(ofps::sdk::BuildLayout(config, 320, 180, &layout) == ofps::sdk::Status::Ok,
          "smoke-test layout builds");

    ofps::sdk::D3D11Adapter adapter;
    const ofps::sdk::D3D11SourceViews emptySources{};
    Check(adapter.Pack(context.Get(), emptySources) == ofps::sdk::AdapterStatus::NotInitialized,
          "Pack rejects use before initialization");
    ofps::sdk::LayoutV1 invalidLayout = layout;
    invalidLayout.edgeSlopeX = 0.75f;
    Check(adapter.Initialize(device.Get(), invalidLayout, DXGI_FORMAT_R8G8B8A8_UNORM, shaders) ==
              ofps::sdk::AdapterStatus::InvalidArgument,
          "adapter rejects a noncanonical layout");
    Check(adapter.Initialize(device.Get(), layout, DXGI_FORMAT_R32_UINT, shaders) ==
              ofps::sdk::AdapterStatus::UnsupportedFormat,
          "adapter rejects an unsupported color format");
    ofps::sdk::ShaderSet missingShader = shaders;
    missingShader.packPixel = {};
    Check(adapter.Initialize(device.Get(), layout, DXGI_FORMAT_R8G8B8A8_UNORM, missingShader) ==
              ofps::sdk::AdapterStatus::ShaderBytecodeMissing,
          "adapter rejects missing shader bytecode");
    Check(adapter.Initialize(device.Get(), layout, DXGI_FORMAT_R8G8B8A8_UNORM, shaders) ==
              ofps::sdk::AdapterStatus::Ok,
          "adapter initializes on D3D11 WARP");

    const ofps::sdk::D3D11PackedViews packed = adapter.PackedViews();
    Check(packed.color != nullptr && packed.depth != nullptr && packed.motion != nullptr &&
              packed.confidence != nullptr,
          "adapter owns all four packed views");
    D3D11_TEXTURE2D_DESC packedColorDesc{};
    Check(DescribeTexture(packed.color, &packedColorDesc), "packed color view owns a 2D texture");
    Check(packedColorDesc.Width == layout.workWidth && packedColorDesc.Height == layout.workHeight &&
              packedColorDesc.Format == DXGI_FORMAT_R8G8B8A8_UNORM,
          "owned color texture matches work dimensions and target format");

    ofps::sdk::ConfigV2 scaledConfig = ofps::sdk::DefaultConfigV2();
    scaledConfig.globalScalePercent = 50.0f;
    ofps::sdk::LayoutV2 scaledLayout{};
    Check(ofps::sdk::BuildLayout(scaledConfig, 320, 180, &scaledLayout) == ofps::sdk::Status::Ok,
          "scaled adapter layout builds");
    ofps::sdk::D3D11Adapter scaledAdapter;
    Check(scaledAdapter.Initialize(device.Get(), scaledLayout,
                                   DXGI_FORMAT_R8G8B8A8_UNORM, shaders) ==
              ofps::sdk::AdapterStatus::Ok,
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
    if (failures != 0) return;

    const TextureViews sourceDepth16 = CreateTexture(
        device.Get(), 160, 90, DXGI_FORMAT_R16_UNORM);
    const TextureViews sourceMotion32 = CreateTexture(
        device.Get(), 160, 90, DXGI_FORMAT_R32G32_FLOAT);
    ofps::sdk::InputDescriptionV2 inputV2 = ofps::sdk::DefaultInputDescriptionV2(
        layout.nativeWidth, layout.nativeHeight);
    inputV2.depthRect = {0, 0, 160, 90};
    inputV2.motionRect = {0, 0, 160, 90};
    inputV2.confidenceRect = {0, 0, 1, 1};
    inputV2.motionScaleX = 320.0f;
    inputV2.motionScaleY = 180.0f;
    inputV2.motionDirection = ofps::sdk::MotionDirection::PreviousToCurrent;
    inputV2.depthConvention = ofps::sdk::DepthConvention::Reversed;
    inputV2.colorEncoding = ofps::sdk::ColorEncoding::Srgb;
    const ofps::sdk::D3D11SourceViews universalSources{
        sourceColor.srv.Get(), sourceDepth16.srv.Get(), sourceMotion32.srv.Get(), nullptr};
    Check(adapter.PackV2(context.Get(), universalSources, inputV2) == ofps::sdk::AdapterStatus::Ok,
          "ABI-v2 accepts R16 depth, RG32 motion, and half-resolution guides");
    inputV2.motionScaleX = 0.0f;
    Check(adapter.PackV2(context.Get(), universalSources, inputV2) ==
              ofps::sdk::AdapterStatus::InvalidArgument,
          "ABI-v2 rejects an ambiguous motion scale before recording GPU work");

    const TextureViews wrongExtent = CreateTexture(
        device.Get(), layout.nativeWidth - 2u, layout.nativeHeight, DXGI_FORMAT_R8G8B8A8_UNORM);
    ofps::sdk::D3D11SourceViews mismatchedSources{
        wrongExtent.srv.Get(), sourceDepth.srv.Get(), sourceMotion.srv.Get(), sourceConfidence.srv.Get()};
    Check(adapter.Pack(context.Get(), mismatchedSources) == ofps::sdk::AdapterStatus::ResourceMismatch,
          "Pack rejects a source with the wrong native extent");
    mismatchedSources.color = sourceColor.srv.Get();
    mismatchedSources.motion = sourceDepth.srv.Get();
    Check(adapter.Pack(context.Get(), mismatchedSources) == ofps::sdk::AdapterStatus::UnsupportedFormat,
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

    const ofps::sdk::D3D11SourceViews nativeSources{
        sourceColor.srv.Get(), sourceDepth.srv.Get(), sourceMotion.srv.Get(),
        sourceConfidence.srv.Get()};
    Check(adapter.Pack(nullptr, nativeSources) == ofps::sdk::AdapterStatus::InvalidArgument,
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
    Check(adapter.Pack(context.Get(), nativeSources) == ofps::sdk::AdapterStatus::Ok,
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
    Check(adapter.Pack(context.Get(), nativeSources) == ofps::sdk::AdapterStatus::Ok,
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

    const ofps::sdk::D3D11SourceViews packedSources{
        packed.color, packed.depth, packed.motion, packed.confidence};
    const ofps::sdk::D3D11TargetViews nativeTargets{
        outputColor.rtv.Get(), outputDepth.rtv.Get(), outputMotion.rtv.Get(),
        outputConfidence.rtv.Get()};
    ofps::sdk::D3D11TargetViews missingColor = nativeTargets;
    missingColor.color = nullptr;
    Check(adapter.Unpack(context.Get(), packedSources, missingColor) ==
              ofps::sdk::AdapterStatus::InvalidArgument,
          "Unpack rejects a missing color target");

    constexpr UINT unpackPixelSlot = 37;
    constexpr UINT unpackComputeSlot = 103;
    ID3D11ShaderResourceView *unpackPixelResource = outputColor.srv.Get();
    ID3D11ShaderResourceView *unpackComputeResource = outputDepth.srv.Get();
    context->PSSetShaderResources(unpackPixelSlot, 1, &unpackPixelResource);
    context->CSSetShaderResources(unpackComputeSlot, 1, &unpackComputeResource);
    Check(adapter.Unpack(context.Get(), packedSources, nativeTargets) == ofps::sdk::AdapterStatus::Ok,
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
    Check(adapter.Pack(context.Get(), nativeSources) == ofps::sdk::AdapterStatus::Ok &&
              adapter.Unpack(context.Get(), packedSources, nativeTargets) == ofps::sdk::AdapterStatus::Ok,
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

    const ofps::sdk::DiagnosticOutlineFlags bothOutlines =
        ofps::sdk::DiagnosticOutlineCenter | ofps::sdk::DiagnosticOutlineRawWork;
    Check(adapter.Unpack(context.Get(), packedSources, nativeTargets, bothOutlines) ==
              ofps::sdk::AdapterStatus::Ok,
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
              static_cast<ofps::sdk::DiagnosticOutlineFlags>(1u << 7)) ==
              ofps::sdk::AdapterStatus::InvalidArgument,
          "Unpack rejects unknown diagnostic outline bits");
}

} // namespace d3d11_cases
