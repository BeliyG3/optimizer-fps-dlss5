#include "d3d11_cases.h"

namespace d3d11_cases {

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


} // namespace d3d11_cases
