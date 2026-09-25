#pragma once
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

namespace d3d11_cases {
struct TextureViews {
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11RenderTargetView> rtv;
};

struct UavTexture {
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11UnorderedAccessView> uav;
};
extern int failures;
void Check(bool condition, const char *message);
std::vector<char> ReadBinary(const char *path);
TextureViews CreateTexture(ID3D11Device *device, std::uint32_t width,
                           std::uint32_t height, DXGI_FORMAT format);
UavTexture CreateUavTexture(ID3D11Device *device, std::uint32_t width, std::uint32_t height);
bool DescribeTexture(ID3D11ShaderResourceView *view, D3D11_TEXTURE2D_DESC *description);
bool ReadColorPixel(ID3D11Device *device, ID3D11DeviceContext *context,
                    ID3D11Texture2D *texture, std::uint32_t x, std::uint32_t y,
                    std::array<std::uint8_t, 4> *value);
bool ReadFloatPixel(ID3D11Device *device, ID3D11DeviceContext *context,
                    ID3D11Texture2D *texture, std::uint32_t x, std::uint32_t y, float *value);
bool ReadHalfPixel(ID3D11Device *device, ID3D11DeviceContext *context,
                   ID3D11Texture2D *texture, std::uint32_t x, std::uint32_t y, float *value);
bool ReadHalf2Pixel(ID3D11Device *device, ID3D11DeviceContext *context,
                    ID3D11Texture2D *texture, std::uint32_t x, std::uint32_t y,
                    std::array<float, 2> *value);
bool NearByte(std::uint8_t value, std::uint8_t expected);
void RunAdapterCases(const std::vector<char> &vertex, const std::vector<char> &pack,
                     const std::vector<char> &unpack);
} // namespace d3d11_cases
