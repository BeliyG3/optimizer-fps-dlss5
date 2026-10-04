#pragma once

// Scenes and pass chains of the carried-frame grid tests on D3D11 WARP (test_temporal_grid_shaders.cpp): a
// 256x128 frame, the reprojection's inputs, additions of every acceptance, and Cells + Compose over an addition on
// the native passes or on a grid.
#include "temporal_cs_harness.h"

#include <cmath>
#include <limits>

namespace temporal_grid_scenes {

using namespace temporal_cs;

inline constexpr UINT kW = 256, kH = 128, kLowW = kW / 16, kLowH = kH / 16;
inline constexpr float kNaN = std::numeric_limits<float>::quiet_NaN(), kInf = std::numeric_limits<float>::infinity();

inline float Hash(UINT x, UINT y, UINT salt)
{
    std::uint32_t h = x * 73856093u ^ y * 19349663u ^ salt * 83492791u;
    h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
    return static_cast<float>(h & 0xffffu) / 65536.0f;
}
inline float Q(float v) { return std::floor(v * 256.0f) / 256.0f; } // exact in fp16

inline Root Constants(const Texel &params, const Texel &tune, const Texel &fill, const Texel &smoothing)
{
    Root c{};
    const Texel rows[9] = {{float(kW), float(kH), 1.0f / kW, 1.0f / kH}, {0, 0, float(kW), float(kH)}, {0, 0, float(kW), float(kH)},
                           {0, 0, float(kW), float(kH)}, {float(kW), float(kH), 1, 1}, params, tune, fill, smoothing};
    for (int i = 0; i < 9; ++i) std::memcpy(&c[i * 4], rows[i].data(), sizeof(Texel));
    return c;
}

struct Shaders {
    std::vector<char> reproject, cells, compose, reprojectGrid, cellsGrid, composeGrid;
    explicit Shaders(const std::filesystem::path &dir, bool grid = true)
        : reproject(ReadFile(dir / "temporal_Reproject_cs.dxbc")), cells(ReadFile(dir / "temporal_Cells_cs.dxbc")),
          compose(ReadFile(dir / "temporal_Compose_cs.dxbc"))
    {
        if (!grid) return;
        reprojectGrid = ReadFile(dir / "temporal_ReprojectGrid_cs.dxbc");
        cellsGrid = ReadFile(dir / "temporal_CellsGrid_cs.dxbc");
        composeGrid = ReadFile(dir / "temporal_ComposeGrid_cs.dxbc");
    }
};

// Inputs of the reprojection: a patch that moved 4 px, noise, NaN links; or (constant) a still grey frame whose
// stored residual is one grey value everywhere.
struct ReprojectScene {
    std::vector<ComPtr<ID3D11ShaderResourceView>> keep;
    Views views{}; // t12..t19 (older passes) stay unbound and read as zero
    ReprojectScene(Gpu &gpu, bool constant, float k)
    {
        Image color(kW, kH), colorF(kW, kH), residual(kW, kH), older(kW, kH), low(kLowW, kLowH), expect(kW, kH, {kNaN, 1.0f, kNaN, 1.0f});
        std::vector<float> depth(kW * kH), depthF(kW * kH), chain(kW * kH * 2);
        for (UINT y = 0; y < kH; ++y)
            for (UINT x = 0; x < kW; ++x) {
                const UINT i = y * kW + x;
                const bool rows = y >= 30 && y < 90, now = rows && x >= 100 && x < 160, then = rows && x >= 96 && x < 156;
                const Texel background{Q(0.2f + 0.3f * x / kW), Q(0.3f + 0.2f * Hash(x, y, 1)), 0.4f, 1.0f};
                color.at(x, y) = constant ? Texel{0.25f, 0.25f, 0.25f, 1.0f} : now ? Texel{0.9f, 0.2f, 0.1f, 1.0f} : background;
                colorF.at(x, y) = constant ? color.at(x, y) : then ? Texel{0.9f, 0.2f, 0.1f, 1.0f} : background;
                depth[i] = !constant && now ? 0.3f : 0.8f;
                depthF[i] = !constant && then ? 0.3f : 0.8f;
                chain[i * 2] = constant ? 0.0f : now ? -4.0f : 0.5f;
                chain[i * 2 + 1] = constant ? 0.0f : now ? 0.0f : -0.25f;
                if (!constant && Hash(x, y, 2) < 0.03f) chain[i * 2] = kNaN;
                residual.at(x, y) = constant ? Texel{k, k, k, 1.0f}
                                             : Texel{Q(Hash(x, y, 3) - 0.5f) * 0.5f, Q(Hash(x, y, 4) - 0.5f) * 0.5f, Q(Hash(x, y, 5) - 0.5f) * 0.5f, 1.0f};
                if (!constant && Hash(x, y, 6) < 0.02f) residual.at(x, y)[1] = kNaN;
                older.at(x, y) = constant ? residual.at(x, y) : Texel{residual.at(x, y)[2], 0.0625f, residual.at(x, y)[0], 1.0f};
            }
        for (UINT y = 0; y < kLowH; ++y)
            for (UINT x = 0; x < kLowW; ++x) low.at(x, y) = constant ? Texel{k, k, k, 1.0f} : Texel{0.0625f * (x % 4), -0.03125f, 0.125f, 1.0f};
        auto add = [&](int slot, ComPtr<ID3D11ShaderResourceView> view) { views[slot] = view.Get(); keep.push_back(view); };
        add(0, gpu.Srv(color)); add(2, gpu.Srv(residual)); add(7, gpu.Srv(colorF)); add(8, gpu.Srv(low)); add(10, gpu.Srv(older));
        add(11, gpu.Srv(expect));
        add(4, gpu.Srv(kW, kH, DXGI_FORMAT_R32G32_FLOAT, chain.data(), 2 * sizeof(float)));
        add(5, gpu.Srv(kW, kH, DXGI_FORMAT_R32_FLOAT, depth.data(), sizeof(float)));
        add(6, gpu.Srv(kW, kH, DXGI_FORMAT_R32_FLOAT, depthF.data(), sizeof(float)));
    }
};

// older passes, Catmull-Rom, colour tolerance, history search, fill floor, chain search radius, luma band, ratio, share
inline constexpr float kReprojectConfigs[3][9] = {{0, 0, 0.08f, 0, 0, 1, 0, 0, 1}, {1, 1, 0.08f, 0, 1, 6, 0, 1, 0.6f}, {0, 1, 0, -1, 0, 1, -1, 1, 1}};
inline Root ReprojectConstants(const float (&k)[9])
{
    return Constants({k[0], k[1], 0, 0.05f}, {k[2], 1, k[3], k[4]}, {1, float(kLowW), float(kLowH), k[5]}, {k[6], 24, k[7], k[8]});
}

// What Compose and Cells read: the frame (t0), its depth (t5), the addition (t2), the cells' look (t1) and addition (t8).
struct ComposeInputs {
    std::vector<ComPtr<ID3D11ShaderResourceView>> keep;
    Views views{};
    void Set(int slot, ComPtr<ID3D11ShaderResourceView> view) { views[slot] = view.Get(); keep.push_back(view); }
};

inline Image Frame(bool grey)
{
    Image color(kW, kH);
    for (UINT y = 0; y < kH; ++y)
        for (UINT x = 0; x < kW; ++x)
            color.at(x, y) = grey ? Texel{0.25f, 0.25f, 0.25f, 1.0f}
                                  : Texel{Q(0.3f + 0.4f * Hash(x, y, 1)) + (x >= kW / 2 ? 0.5f : 0.0f), Q(0.3f + 0.4f * Hash(x, y, 2)), 0.25f, 1.0f};
    return color;
}

inline ComposeInputs MakeComposeInputs(Gpu &gpu, const Image &color, const Image &addition)
{
    std::vector<float> depth(kW * kH);
    for (UINT y = 0; y < kH; ++y)
        for (UINT x = 0; x < kW; ++x) depth[y * kW + x] = (x >= kW / 2 ? 0.8f : 0.4f) + Q(0.01f * Hash(x, y, 6));
    ComposeInputs in;
    in.Set(0, gpu.Srv(color)); in.Set(2, gpu.Srv(addition));
    in.Set(5, gpu.Srv(kW, kH, DXGI_FORMAT_R32_FLOAT, depth.data(), sizeof(float)));
    return in;
}

// Additions with every kind of acceptance (accepted, partial, rejected, lone -1); `specials` adds -0, NaN and infinities.
inline Image Additions(bool specials)
{
    const float alphas[9] = {1.0f, 0.9f, 0.5f, 0.45f, 0.3f, 0.2f, 0.1f, 0.0f, -1.0f};
    const float odd[5] = {-0.0f, 0.0f, kNaN, kInf, -kInf};
    Image a(kW, kH);
    for (UINT y = 0; y < kH; ++y)
        for (UINT x = 0; x < kW; ++x) {
            Texel &t = a.at(x, y);
            for (int c = 0; c < 3; ++c) t[c] = Q(Hash(x, y, 10 + c) - 0.5f) + 1.0f / 256.0f;
            t[3] = alphas[static_cast<int>(Hash(x, y, 3) * 9.0f)];
            if (specials && Hash(x, y, 4) < 0.15f) t[static_cast<int>(Hash(x, y, 7) * 3.0f)] = odd[static_cast<int>(Hash(x, y, 5) * 5.0f)];
            if (x >= 20 && x <= 90 && y >= 10 && y <= 60) t = {0.25f, 0.25f, -0.125f, 1.0f}; // an accepted surface
        }
    return a;
}

// colour tolerance, cells bound, luma band (0 on, -1 off), ratio domain, smoothing radius
inline constexpr float kComposeConfigs[5][5] = {{0.08f, 0, 0, 0, 6}, {0, 1, 0, 0, 6}, {0.08f, 1, 0, 1, 24}, {0.08f, 0, -1, 1, 6}, {0.08f, 1, 0, 0, 0}};
inline Root ComposeConstants(const float (&k)[5])
{
    return Constants({0, 0, 0, 0.05f}, {k[0], 1, 0, 0}, {k[1], float(kLowW), float(kLowH), 1}, {k[2], k[4], k[3], 1});
}

inline std::array<Image, 2> Run(Gpu &gpu, const std::vector<char> &code, const Views &views, const Root &root, Lanes lanes,
                                std::array<Image, 2> out)
{
    gpu.Run(code, views, root, lanes, lanes[0], lanes[1], out);
    return out;
}
inline std::array<Image, 2> Native(UINT w, UINT h, const Texel &fill = {}) { return {Image(w, h, fill), Image(w, h, fill)}; }

// Cells then Compose over `addition` (as t2), on the grid `grid` (0 = the native passes).
inline Image ComposeChain(Gpu &gpu, const Shaders &s, const Image &color, const Image &addition, const float (&k)[5], UINT gridW, UINT gridH)
{
    ComposeInputs in = MakeComposeInputs(gpu, color, addition);
    const Root root = ComposeConstants(k);
    const bool grid = gridW != 0;
    if (k[1] != 0) {
        const auto cells = Run(gpu, grid ? s.cellsGrid : s.cells, in.views, root, {kLowW, kLowH, 0, 0, 0, 0, gridW, gridH}, Native(kLowW, kLowH));
        in.Set(8, gpu.Srv(cells[0])); in.Set(1, gpu.Srv(cells[1]));
    }
    return Run(gpu, grid ? s.composeGrid : s.compose, in.views, root, {kW, kH, 0, 0, 0, 0, gridW, gridH}, Native(kW, kH))[0];
}

} // namespace temporal_grid_scenes
