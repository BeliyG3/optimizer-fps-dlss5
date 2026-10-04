#pragma once

// D3D11 WARP harness for single temporal compute passes (core/shaders/temporal_cs.hlsl): float images as SRVs,
// two RGBA32F UAVs that start from given images and are read back, b0 (36 floats), b1 (8 lanes) and the two
// samplers of the machine's root signature. Used by test_temporal_grid_shaders.cpp.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <vector>

namespace temporal_cs {

using Microsoft::WRL::ComPtr;
using Texel = std::array<float, 4>;
using Views = std::array<ID3D11ShaderResourceView *, 20>;
using Root = std::array<float, 36>;
using Lanes = std::array<std::uint32_t, 8>;

inline void Check(HRESULT hr) { if (FAILED(hr)) throw std::runtime_error("D3D operation failed"); }
inline void Require(bool ok, const char *what) { if (!ok) throw std::runtime_error(what); }

struct Image {
    UINT width = 0, height = 0;
    std::vector<Texel> texels;
    Image() = default;
    Image(UINT w, UINT h, const Texel &fill = {}) : width(w), height(h), texels(static_cast<std::size_t>(w) * h, fill) {}
    Texel &at(UINT x, UINT y) { return texels[static_cast<std::size_t>(y) * width + x]; }
    const Texel &at(UINT x, UINT y) const { return texels[static_cast<std::size_t>(y) * width + x]; }
};

inline bool SameBits(const Texel &a, const Texel &b) { return std::memcmp(a.data(), b.data(), sizeof(Texel)) == 0; }
inline bool SameBits(const Image &a, const Image &b)
{
    return a.width == b.width && a.height == b.height &&
           std::memcmp(a.texels.data(), b.texels.data(), a.texels.size() * sizeof(Texel)) == 0;
}

inline std::vector<char> ReadFile(const std::filesystem::path &path)
{
    std::ifstream file(path, std::ios::binary);
    std::vector<char> code((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    Require(!code.empty(), "shader binary missing");
    return code;
}

struct Gpu {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Buffer> b0, b1;
    ComPtr<ID3D11SamplerState> samplers[2];

    Gpu()
    {
        Check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context));
        D3D11_BUFFER_DESC cb{};
        cb.Usage = D3D11_USAGE_DEFAULT; cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cb.ByteWidth = sizeof(Root); Check(device->CreateBuffer(&cb, nullptr, &b0));
        cb.ByteWidth = sizeof(Lanes); Check(device->CreateBuffer(&cb, nullptr, &b1));
        D3D11_SAMPLER_DESC sd{};
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP; sd.MaxLOD = D3D11_FLOAT32_MAX;
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR; Check(device->CreateSamplerState(&sd, &samplers[0]));
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT; Check(device->CreateSamplerState(&sd, &samplers[1]));
    }

    ComPtr<ID3D11ShaderResourceView> Srv(UINT w, UINT h, DXGI_FORMAT format, const void *data, UINT texelBytes)
    {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = w; desc.Height = h; desc.MipLevels = desc.ArraySize = 1;
        desc.Format = format; desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_IMMUTABLE; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        const D3D11_SUBRESOURCE_DATA init{data, w * texelBytes, 0};
        ComPtr<ID3D11Texture2D> texture; Check(device->CreateTexture2D(&desc, &init, &texture));
        ComPtr<ID3D11ShaderResourceView> view; Check(device->CreateShaderResourceView(texture.Get(), nullptr, &view));
        return view;
    }
    ComPtr<ID3D11ShaderResourceView> Srv(const Image &image)
    {
        return Srv(image.width, image.height, DXGI_FORMAT_R32G32B32A32_FLOAT, image.texels.data(), sizeof(Texel));
    }

    // One dispatch of `threads`; u0 and u1 start as `out` and hold what the pass left there afterwards.
    void Run(const std::vector<char> &code, const Views &views, const Root &root, const Lanes &lanes, UINT threadsX, UINT threadsY,
             std::array<Image, 2> &out)
    {
        ComPtr<ID3D11ComputeShader> shader; Check(device->CreateComputeShader(code.data(), code.size(), nullptr, &shader));
        context->CSSetShader(shader.Get(), nullptr, 0);
        context->CSSetShaderResources(0, static_cast<UINT>(views.size()), views.data());
        ID3D11SamplerState *rawSamplers[2] = {samplers[0].Get(), samplers[1].Get()};
        context->CSSetSamplers(0, 2, rawSamplers);
        context->UpdateSubresource(b0.Get(), 0, nullptr, root.data(), 0, 0);
        context->UpdateSubresource(b1.Get(), 0, nullptr, lanes.data(), 0, 0);
        ID3D11Buffer *buffers[2] = {b0.Get(), b1.Get()};
        context->CSSetConstantBuffers(0, 2, buffers);
        ComPtr<ID3D11Texture2D> targets[2];
        ComPtr<ID3D11UnorderedAccessView> uavs[2];
        for (int i = 0; i < 2; ++i) {
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = out[i].width; desc.Height = out[i].height; desc.MipLevels = desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_DEFAULT; desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
            const D3D11_SUBRESOURCE_DATA init{out[i].texels.data(), out[i].width * static_cast<UINT>(sizeof(Texel)), 0};
            Check(device->CreateTexture2D(&desc, &init, &targets[i]));
            Check(device->CreateUnorderedAccessView(targets[i].Get(), nullptr, &uavs[i]));
        }
        ID3D11UnorderedAccessView *raw[2] = {uavs[0].Get(), uavs[1].Get()};
        context->CSSetUnorderedAccessViews(0, 2, raw, nullptr);
        context->Dispatch((threadsX + 15) / 16, (threadsY + 7) / 8, 1);
        ID3D11UnorderedAccessView *none[2] = {};
        context->CSSetUnorderedAccessViews(0, 2, none, nullptr);
        for (int i = 0; i < 2; ++i) {
            D3D11_TEXTURE2D_DESC desc{};
            targets[i]->GetDesc(&desc);
            desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            ComPtr<ID3D11Texture2D> staging; Check(device->CreateTexture2D(&desc, nullptr, &staging));
            context->CopyResource(staging.Get(), targets[i].Get());
            D3D11_MAPPED_SUBRESOURCE mapped{}; Check(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
            for (UINT y = 0; y < out[i].height; ++y)
                std::memcpy(&out[i].at(0, y), static_cast<const char *>(mapped.pData) + y * mapped.RowPitch, out[i].width * sizeof(Texel));
            context->Unmap(staging.Get(), 0);
        }
    }
};

} // namespace temporal_cs
