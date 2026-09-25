#pragma once
#include "d3d11_adapter.h"
#include <algorithm>
#include <array>
#include <d3d11_1.h>
#include <wrl/client.h>

namespace ofps::sdk {
using Microsoft::WRL::ComPtr;

struct alignas(16) DiagnosticConstants {
    std::uint32_t outlineFlags;
    std::uint32_t outputGainBits;     // float bits, 0 = unset
    std::uint32_t outputInvGammaBits; // float bits, 0 = unset
    std::uint32_t reserved;
};

static_assert(sizeof(DiagnosticConstants) == 16);

bool SupportedColorFormat(DXGI_FORMAT format) noexcept;
bool SupportedDepthView(DXGI_FORMAT format) noexcept;
bool SupportedMotionView(DXGI_FORMAT format) noexcept;
bool SupportedConfidenceView(DXGI_FORMAT format) noexcept;
bool ShaderSampleSupported(ID3D11Device *device, DXGI_FORMAT format) noexcept;
AdapterStatus ValidateContext(ID3D11DeviceContext *context, ID3D11Device *expectedDevice) noexcept;
AdapterStatus DescribeSourceView(ID3D11ShaderResourceView *view, ID3D11Device *expectedDevice,
                                 bool required, DXGI_FORMAT *format,
                                 std::uint32_t *width, std::uint32_t *height) noexcept;
AdapterStatus ValidateSourceViewExact(ID3D11ShaderResourceView *view, ID3D11Device *expectedDevice,
                                      std::uint32_t expectedWidth, std::uint32_t expectedHeight,
                                      DXGI_FORMAT expectedFormat, bool required) noexcept;
AdapterStatus ValidateTargetView(ID3D11RenderTargetView *view, ID3D11Device *expectedDevice,
                                 std::uint32_t expectedWidth, std::uint32_t expectedHeight,
                                 DXGI_FORMAT expectedFormat, bool required) noexcept;

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

    AdapterStatus CreateResources(const ShaderSet &shaders) noexcept;
    AdapterStatus Draw(ID3D11DeviceContext *context, ID3D11PixelShader *shader,
                       const D3D11SourceViews &sources, const D3D11TargetViews &targets,
                       std::uint32_t width, std::uint32_t height,
                       DiagnosticOutlineFlags diagnosticOutlines = DiagnosticOutlineNone) noexcept;
};

} // namespace ofps::sdk
