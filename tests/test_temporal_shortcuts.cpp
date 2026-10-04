// The carried-frame shortcuts of 2026.10.1 on D3D11 WARP, byte for byte:
//   - the production Compose (it skips the smoothing where its share is exactly 0) writes the same bytes as
//     the same pass built without that shortcut (PW_T_COMPOSE_ALWAYS_SMOOTH);
//   - the reprojection with a null u0 (PwTSkipOut0, before Compose) writes the same addition (u1) as with
//     u0 bound, and leaves u0 alone.
// The scene mixes accepted, partly accepted and rejected additions with -0, +0, NaN and infinite channels,
// and a pixel where an unguarded shortcut would turn the sign of a zero.
// Usage: ofps_temporal_shortcuts_tests <shader dir> <Compose reference .dxbc> [<Reproject reference .dxbc>]
// The optional third argument compares the reprojection with an older build's (u0 and u1, u0 bound).
#include <d3d11.h>
#include <wrl/client.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {
constexpr UINT kW = 48, kH = 24, kLowW = 3, kLowH = 2;
using Texel = std::array<float, 4>;
using Image = std::vector<Texel>;
using Views = std::array<ID3D11ShaderResourceView *, 20>;
constexpr float kNaN = std::numeric_limits<float>::quiet_NaN(), kInf = std::numeric_limits<float>::infinity();

void Check(HRESULT hr) { if (FAILED(hr)) throw std::runtime_error("D3D operation failed"); }
void Require(bool ok, const char *what) { if (!ok) throw std::runtime_error(what); }

std::vector<char> ReadFile(const std::filesystem::path &path)
{
    std::ifstream file(path, std::ios::binary);
    std::vector<char> code((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    Require(!code.empty(), "shader binary missing");
    return code;
}

// Deterministic per-pixel noise in [0, 1), and values a 16-bit float holds exactly (the passes read RGBA16F).
float Hash(UINT x, UINT y, UINT salt)
{
    std::uint32_t h = x * 73856093u ^ y * 19349663u ^ salt * 83492791u;
    h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
    return static_cast<float>(h & 0xffffu) / 65536.0f;
}
float Q(float v) { return std::floor(v * 256.0f) / 256.0f; }

struct Gpu {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Buffer> b0, b1;

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
    ComPtr<ID3D11ShaderResourceView> Srv(const Image &image, UINT w = kW, UINT h = kH)
    {
        return Srv(w, h, DXGI_FORMAT_R32G32B32A32_FLOAT, image.data(), sizeof(Texel));
    }

    // Runs one dispatch over the scene; u0 and u1 start as `fill` and are read back.
    std::array<Image, 2> Run(const std::vector<char> &code, const Views &views, const std::array<float, 36> &constants,
                             std::uint32_t skipOut0, const Texel &fill)
    {
        ComPtr<ID3D11ComputeShader> shader; Check(device->CreateComputeShader(code.data(), code.size(), nullptr, &shader));
        context->CSSetShader(shader.Get(), nullptr, 0);
        context->CSSetShaderResources(0, static_cast<UINT>(views.size()), views.data());
        const std::uint32_t dispatch[8] = {kW, kH, 0, 0, 0, skipOut0, 0, 0};
        context->UpdateSubresource(b0.Get(), 0, nullptr, constants.data(), 0, 0);
        context->UpdateSubresource(b1.Get(), 0, nullptr, dispatch, 0, 0);
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = kW; desc.Height = kH; desc.MipLevels = desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT; desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        const Image initial(kW * kH, fill);
        const D3D11_SUBRESOURCE_DATA init{initial.data(), kW * sizeof(Texel), 0};
        ComPtr<ID3D11Texture2D> targets[2];
        ComPtr<ID3D11UnorderedAccessView> uavs[2];
        for (int i = 0; i < 2; ++i) {
            Check(device->CreateTexture2D(&desc, &init, &targets[i]));
            Check(device->CreateUnorderedAccessView(targets[i].Get(), nullptr, &uavs[i]));
        }
        ID3D11UnorderedAccessView *raw[2] = {uavs[0].Get(), uavs[1].Get()};
        context->CSSetUnorderedAccessViews(0, 2, raw, nullptr);
        context->Dispatch((kW + 15) / 16, (kH + 7) / 8, 1);
        ID3D11UnorderedAccessView *none[2] = {};
        context->CSSetUnorderedAccessViews(0, 2, none, nullptr);
        desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        std::array<Image, 2> out;
        for (int i = 0; i < 2; ++i) {
            ComPtr<ID3D11Texture2D> staging; Check(device->CreateTexture2D(&desc, nullptr, &staging));
            context->CopyResource(staging.Get(), targets[i].Get());
            D3D11_MAPPED_SUBRESOURCE mapped{}; Check(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
            out[i].resize(kW * kH);
            for (UINT y = 0; y < kH; ++y)
                std::memcpy(&out[i][y * kW], static_cast<const char *>(mapped.pData) + y * mapped.RowPitch, kW * sizeof(Texel));
            context->Unmap(staging.Get(), 0);
        }
        return out;
    }
};

bool Same(const Image &a, const Image &b) { return std::memcmp(a.data(), b.data(), a.size() * sizeof(Texel)) == 0; }

std::array<float, 36> Constants(const Texel &params, const Texel &tune, const Texel &fill, const Texel &smoothing)
{
    std::array<float, 36> c{};
    const Texel rows[9] = {{float(kW), float(kH), 1.0f / kW, 1.0f / kH}, {0, 0, float(kW), float(kH)}, {0, 0, float(kW), float(kH)},
                           {0, 0, float(kW), float(kH)}, {float(kW), float(kH), 1, 1}, params, tune, fill, smoothing};
    for (int i = 0; i < 9; ++i) std::memcpy(&c[i * 4], rows[i].data(), sizeof(Texel));
    return c;
}

constexpr UINT kHazardX = 10, kHazardY = 8; // inside a block of accepted additions, all taps within it

void TestCompose(Gpu &gpu, const std::vector<char> &shortcut, const std::vector<char> &reference)
{
    Image color(kW * kH), addition(kW * kH), look(kLowW * kLowH), cells(kLowW * kLowH);
    std::vector<float> depth(kW * kH);
    const float alphas[10] = {1.0f, 0.9f, 0.5f, 0.45f, 0.3f, 0.2f, 0.1f, 0.0f, -1.0f, kNaN};
    const float specials[5] = {-0.0f, 0.0f, kNaN, kInf, -kInf};
    for (UINT y = 0; y < kH; ++y)
        for (UINT x = 0; x < kW; ++x) {
            const UINT i = y * kW + x;
            const bool right = x >= kW / 2;
            color[i] = {Q(0.3f + 0.4f * Hash(x, y, 1)) + (right ? 0.5f : 0.0f), Q(0.3f + 0.4f * Hash(x, y, 2)), 0.25f, 1.0f};
            depth[i] = (right ? 0.8f : 0.4f) + Q(0.01f * Hash(x, y, 6));
            Texel &a = addition[i];
            for (int c = 0; c < 3; ++c) a[c] = Q(Hash(x, y, 10 + c) - 0.5f) + 1.0f / 256.0f;
            a[3] = alphas[static_cast<int>(Hash(x, y, 3) * 10.0f)];
            if (Hash(x, y, 4) < 0.15f) a[static_cast<int>(Hash(x, y, 7) * 3.0f)] = specials[static_cast<int>(Hash(x, y, 5) * 5.0f)];
            if (x >= 2 && x <= 18 && y >= 1 && y <= 15) { // accepted, alike, one surface
                color[i] = {0.0f, 0.5f, 0.25f, 1.0f};
                depth[i] = 0.4f;
                a = {0.25f, 0.25f, -0.125f, 1.0f};
            }
        }
    // Smoothing share 0 and an addition of -0 on a colour of -0: the full pass writes +0 there.
    color[kHazardY * kW + kHazardX][0] = -0.0f;
    addition[kHazardY * kW + kHazardX][0] = -0.0f;
    for (UINT i = 0; i < kLowW * kLowH; ++i) {
        look[i] = {0.3f + 0.1f * i, 0.5f, 0.25f, 0.0f};
        cells[i] = {Q(Hash(i, 0, 20) - 0.5f), Q(Hash(i, 0, 21) - 0.5f), 0.125f, 0.5f};
    }
    cells[4][1] = kNaN;
    const auto colorView = gpu.Srv(color), additionView = gpu.Srv(addition);
    const auto lookView = gpu.Srv(look, kLowW, kLowH), cellsView = gpu.Srv(cells, kLowW, kLowH);
    const auto depthView = gpu.Srv(kW, kH, DXGI_FORMAT_R32_FLOAT, depth.data(), sizeof(float));
    Views views{};
    views[0] = colorView.Get(); views[1] = lookView.Get(); views[2] = additionView.Get();
    views[5] = depthView.Get(); views[8] = cellsView.Get();
    // colour tolerance, cells bound, luma band (0 on, -1 off), ratio domain, smoothing radius
    const float configs[5][5] = {{0.08f, 0, 0, 0, 6}, {0, 1, 0, 0, 6}, {0.08f, 1, 0, 1, 6}, {0.08f, 0, -1, 1, 6}, {0.08f, 1, 0, 0, 0}};
    for (const auto &k : configs) {
        const auto c = Constants({0, 0, 0, 0.05f}, {k[0], 1, 0, 0}, {k[1], float(kLowW), float(kLowH), 1}, {k[2], k[4], k[3], 1});
        const Image fast = gpu.Run(shortcut, views, c, 0, {})[0];
        const Image full = gpu.Run(reference, views, c, 0, {})[0];
        Require(Same(fast, full), "Compose: the shortcut changed the frame");
        if (k[3] != 0 || k[4] == 0) continue; // the checks below read the frame as it is, smoothed
        const Texel &accepted = full[kHazardY * kW + kHazardX + 2];
        Require(accepted[0] == 0.25f && accepted[1] == 0.75f && accepted[2] == 0.125f, "Compose: an accepted pixel was smoothed");
        if (k[1] != 0) continue; // the cells could pull the smoothed addition below zero
        std::uint32_t bits = 0;
        std::memcpy(&bits, &full[kHazardY * kW + kHazardX][0], sizeof(bits));
        Require(bits == 0u, "Compose: the scene lost its signed-zero case (colour + addition would be -0, the pass writes +0)");
    }
}

void TestReproject(Gpu &gpu, const std::vector<char> &code, const std::vector<char> *reference)
{
    Image color(kW * kH), colorF(kW * kH), residual(kW * kH), older(kW * kH), low(kLowW * kLowH), expect(kW * kH);
    std::vector<float> depth(kW * kH), depthF(kW * kH), chain(kW * kH * 2);
    for (UINT y = 0; y < kH; ++y)
        for (UINT x = 0; x < kW; ++x) {
            const UINT i = y * kW + x;
            const bool rows = y >= 6 && y < 18, now = rows && x >= 16 && x < 32, then = rows && x >= 12 && x < 28;
            const Texel background{Q(0.2f + 0.3f * x / kW), Q(0.3f + 0.2f * Hash(x, y, 1)), 0.4f, 1.0f};
            color[i] = now ? Texel{0.9f, 0.2f, 0.1f, 1.0f} : background;
            colorF[i] = then ? Texel{0.9f, 0.2f, 0.1f, 1.0f} : background;
            depth[i] = now ? 0.3f : 0.8f;
            depthF[i] = then ? 0.3f : 0.8f;
            chain[i * 2] = now ? -4.0f : 0.5f;
            chain[i * 2 + 1] = now ? 0.0f : -0.25f;
            if (Hash(x, y, 2) < 0.03f) chain[i * 2] = kNaN;
            residual[i] = {Q(Hash(x, y, 3) - 0.5f) * 0.5f, Q(Hash(x, y, 4) - 0.5f) * 0.5f, Q(Hash(x, y, 5) - 0.5f) * 0.5f, 1.0f};
            if (Hash(x, y, 6) < 0.02f) residual[i][1] = kNaN;
            older[i] = {residual[i][2], 0.0625f, residual[i][0], 1.0f};
            expect[i] = {kNaN, 1.0f, kNaN, 1.0f}; // expectation off
        }
    for (UINT i = 0; i < kLowW * kLowH; ++i) low[i] = {0.0625f * i, -0.03125f, 0.125f, 1.0f};
    const auto colorView = gpu.Srv(color), colorFView = gpu.Srv(colorF), residualView = gpu.Srv(residual);
    const auto olderView = gpu.Srv(older), expectView = gpu.Srv(expect), lowView = gpu.Srv(low, kLowW, kLowH);
    const auto depthView = gpu.Srv(kW, kH, DXGI_FORMAT_R32_FLOAT, depth.data(), sizeof(float));
    const auto depthFView = gpu.Srv(kW, kH, DXGI_FORMAT_R32_FLOAT, depthF.data(), sizeof(float));
    const auto chainView = gpu.Srv(kW, kH, DXGI_FORMAT_R32G32_FLOAT, chain.data(), 2 * sizeof(float));
    Views views{}; // t12..t19 (older passes) stay unbound and read as zero
    views[0] = colorView.Get(); views[2] = residualView.Get(); views[4] = chainView.Get(); views[5] = depthView.Get();
    views[6] = depthFView.Get(); views[7] = colorFView.Get(); views[8] = lowView.Get(); views[10] = olderView.Get();
    views[11] = expectView.Get();
    // older passes, Catmull-Rom, colour tolerance, history search, fill floor, chain search radius, luma band, ratio, share
    const float configs[3][9] = {{0, 0, 0.08f, 0, 0, 1, 0, 0, 1}, {1, 1, 0.08f, 0, 1, 6, 0, 1, 0.6f}, {0, 0, 0, -1, 0, 1, -1, 1, 1}};
    const Texel sentinel{12345.0f, -6789.0f, 42.0f, 7.0f};
    int accepted = 0, rejected = 0;
    for (const auto &k : configs) {
        const auto c = Constants({k[0], k[1], 0, 0.05f}, {k[2], 1, k[3], k[4]}, {1, float(kLowW), float(kLowH), k[5]}, {k[6], 24, k[7], k[8]});
        const auto bound = gpu.Run(code, views, c, 0, sentinel);
        const auto skipped = gpu.Run(code, views, c, 1, sentinel);
        Require(Same(bound[1], skipped[1]), "Reproject: the addition differs without u0");
        Require(Same(skipped[0], Image(kW * kH, sentinel)), "Reproject: u0 was written although it is skipped");
        Require(!Same(bound[0], Image(kW * kH, sentinel)), "Reproject: u0 was not written although it is bound");
        if (reference) {
            const auto old = gpu.Run(*reference, views, c, 0, sentinel);
            Require(Same(old[0], bound[0]) && Same(old[1], bound[1]), "Reproject: differs from the reference build");
        }
        for (const Texel &t : bound[1]) (t[3] >= 0.5f ? accepted : rejected) += 1;
    }
    Require(accepted > 0 && rejected > 0, "Reproject: the scene must have accepted and rejected pixels");
    std::printf("reprojection: %d accepted, %d rejected pixel samples\n", accepted, rejected);
}
} // namespace

int main(int argc, char **argv)
{
    try {
        Require(argc == 3 || argc == 4, "usage: ofps_temporal_shortcuts_tests <shader dir> <Compose reference> [<Reproject reference>]");
        const std::filesystem::path dir(argv[1]);
        const auto compose = ReadFile(dir / "temporal_Compose_cs.dxbc"), reference = ReadFile(argv[2]);
        const auto reproject = ReadFile(dir / "temporal_Reproject_cs.dxbc");
        const std::vector<char> reprojectReference = argc == 4 ? ReadFile(argv[3]) : std::vector<char>{};
        Gpu gpu;
        Check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &gpu.device, nullptr, &gpu.context));
        D3D11_BUFFER_DESC cb{};
        cb.Usage = D3D11_USAGE_DEFAULT; cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cb.ByteWidth = 36 * sizeof(float); Check(gpu.device->CreateBuffer(&cb, nullptr, &gpu.b0));
        cb.ByteWidth = 8 * sizeof(std::uint32_t); Check(gpu.device->CreateBuffer(&cb, nullptr, &gpu.b1));
        ID3D11Buffer *buffers[2] = {gpu.b0.Get(), gpu.b1.Get()};
        gpu.context->CSSetConstantBuffers(0, 2, buffers);
        D3D11_SAMPLER_DESC sd{};
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP; sd.MaxLOD = D3D11_FLOAT32_MAX;
        ComPtr<ID3D11SamplerState> samplers[2];
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR; Check(gpu.device->CreateSamplerState(&sd, &samplers[0]));
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT; Check(gpu.device->CreateSamplerState(&sd, &samplers[1]));
        ID3D11SamplerState *rawSamplers[2] = {samplers[0].Get(), samplers[1].Get()};
        gpu.context->CSSetSamplers(0, 2, rawSamplers);
        TestCompose(gpu, compose, reference);
        TestReproject(gpu, reproject, argc == 4 ? &reprojectReference : nullptr);
        std::printf("PASS: Compose shortcut and reprojection without u0 are byte-identical on WARP\n");
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
