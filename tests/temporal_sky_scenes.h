#pragma once

// Scenes and pass chains of the sky field tests on D3D11 WARP (test_temporal_sky_shaders.cpp,
// test_temporal_sky_compose.cpp, test_temporal_sky_gate.cpp): the field's own passes (Down, Pull, Push) chained as
// Machine::RecordResidual records them, the carried frame's share of sky (Now, Pull), and the passes that form a frame
// (Compose and its grid variant, Reproject's direct route, Apply) with the field at t20 and that share at t21. Frames
// are 256x128 with a 16x8 low-res grid, as in temporal_grid_scenes.h.
#include "temporal_grid_scenes.h"

#include <algorithm>
#include <cmath>

namespace temporal_sky_scenes {

using namespace temporal_grid_scenes;

inline constexpr int kField = 20;     // t20: PwTSkyField
inline constexpr int kNow = 21;       // t21: PwTSkyNow, the carried frame's share of sky (a of the 1x1 texel)
inline constexpr std::size_t kSky = 36; // b0 row 10: PwTSky.x, the far plane

inline Root WithSky(Root r, float farPlane) { r[kSky] = farPlane; return r; }
inline Root WithDepthRect(Root r, const Texel &rect) { std::memcpy(&r[12], rect.data(), sizeof(Texel)); return r; }

// The current build's passes; with a fixture, its frame passes in place of the current ones: master's four
// (tests/fixtures/temporal_pre_sky) or the first sky field's three (tests/fixtures/temporal_sky_round1, whose Apply is
// the current one). The rest are the current code.
struct SkyShaders {
    std::vector<char> down, pull, push, now, reproject, compose, composeGrid, cells, cellsGrid, apply;
    explicit SkyShaders(const std::filesystem::path &dir)
        : down(ReadFile(dir / "temporal_SkyDown_cs.dxbc")), pull(ReadFile(dir / "temporal_SkyPull_cs.dxbc")),
          push(ReadFile(dir / "temporal_SkyPush_cs.dxbc")), now(ReadFile(dir / "temporal_SkyNow_cs.dxbc")),
          reproject(ReadFile(dir / "temporal_Reproject_cs.dxbc")), compose(ReadFile(dir / "temporal_Compose_cs.dxbc")),
          composeGrid(ReadFile(dir / "temporal_ComposeGrid_cs.dxbc")), cells(ReadFile(dir / "temporal_Cells_cs.dxbc")),
          cellsGrid(ReadFile(dir / "temporal_CellsGrid_cs.dxbc")), apply(ReadFile(dir / "temporal_Apply_cs.dxbc")) {}
    SkyShaders(const std::filesystem::path &dir, const std::filesystem::path &fixture) : SkyShaders(dir)
    {
        reproject = ReadFile(fixture / "temporal_Reproject_cs.dxbc");
        compose = ReadFile(fixture / "temporal_Compose_cs.dxbc");
        composeGrid = ReadFile(fixture / "temporal_ComposeGrid_cs.dxbc");
        if (std::filesystem::exists(fixture / "temporal_Apply_cs.dxbc")) apply = ReadFile(fixture / "temporal_Apply_cs.dxbc");
    }
};

// What PwTSkyNow (t21) holds after CSSkyPull: the carried frame's share of sky in a, rgb unused.
inline Image NowShare(float share) { return Image(1, 1, {0.0f, 0.0f, 0.0f, share}); }

inline ComPtr<ID3D11ShaderResourceView> DepthView(Gpu &gpu, UINT w, UINT h, const std::vector<float> &depth)
{
    return gpu.Srv(w, h, DXGI_FORMAT_R32_FLOAT, depth.data(), sizeof(float));
}

// The constants of the field's passes: the native frame, the low-res grid, the depth guide's sub-rect, the far plane.
inline Root FieldRoot(float farPlane, const Texel &depthRect = {0, 0, float(kW), float(kH)})
{
    return WithDepthRect(WithSky(Constants({0, 0, 0, 0.05f}, {0, 1, 0, 0}, {1, float(kLowW), float(kLowH), 1}, {0, 24, 0, 1}), farPlane),
                         depthRect);
}

struct Field {
    std::vector<Image> pulled, filled; // finest first; the coarsest is filled as it is
    const Image &Finest() const { return filled.front(); }
};

// Pull from `finest` (a finest level of the grid w x h) to 1x1, then Push back down, as SkyField::Build records them:
// each Pull with PwTParams.x = the finest cells a finer texel stands for per side, PwTFill.yz = the grid.
inline Field PullPush(Gpu &gpu, const SkyShaders &s, const Image &finest, Root root)
{
    root[29] = float(finest.width); root[30] = float(finest.height); // PwTFill.yz
    std::vector<std::pair<UINT, UINT>> sizes;
    for (UINT w = finest.width, h = finest.height;; w = (w + 1) / 2, h = (h + 1) / 2) {
        sizes.push_back({w, h});
        if (w == 1 && h == 1) break;
    }
    Field f;
    f.pulled.push_back(finest);
    for (std::size_t k = 1; k < sizes.size(); ++k) {
        auto finer = gpu.Srv(f.pulled[k - 1]);
        Views pull{};
        pull[2] = finer.Get();
        Root level = root;
        level[20] = float(1u << (k - 1)); // PwTParams.x
        f.pulled.push_back(Run(gpu, s.pull, pull, level, {sizes[k].first, sizes[k].second}, Native(sizes[k].first, sizes[k].second))[0]);
    }
    f.filled.resize(sizes.size());
    f.filled.back() = f.pulled.back();
    for (std::size_t k = sizes.size() - 1; k-- > 0;) {
        auto coarser = gpu.Srv(f.filled[k + 1]), own = gpu.Srv(f.pulled[k]);
        Views push{};
        push[1] = coarser.Get(); push[2] = own.Get();
        f.filled[k] = Run(gpu, s.push, push, root, {sizes[k].first, sizes[k].second}, Native(sizes[k].first, sizes[k].second))[0];
    }
    return f;
}

// Down over (residual t2, depth t6), then PullPush: what Machine::RecordResidual records.
inline Field BuildField(Gpu &gpu, const SkyShaders &s, ID3D11ShaderResourceView *residual, ID3D11ShaderResourceView *depth,
                        const Root &root)
{
    Views down{};
    down[2] = residual; down[6] = depth;
    return PullPush(gpu, s, Run(gpu, s.down, down, root, {kLowW, kLowH}, Native(kLowW, kLowH))[0], root);
}

// A field of one value everywhere with the given support (what Push leaves for a uniform sky).
inline Image FlatField(float value, float support) { return Image(kLowW, kLowH, {value, value, value, support}); }

// What the passes read for the field at a native pixel: bilinear at its centre (clamped addressing).
inline Texel SampleField(const Image &field, UINT x, UINT y)
{
    auto axis = [](UINT i, UINT n, UINT size) {
        const float t = (float(i) + 0.5f) / float(n) * float(size) - 0.5f;
        const float base = std::floor(t);
        const int i0 = std::clamp(int(base), 0, int(size) - 1), i1 = std::clamp(int(base) + 1, 0, int(size) - 1);
        return std::make_tuple(i0, i1, t - base);
    };
    const auto [x0, x1, fx] = axis(x, kW, field.width);
    const auto [y0, y1, fy] = axis(y, kH, field.height);
    Texel out{};
    for (int c = 0; c < 4; ++c)
        out[c] = (field.at(x0, y0)[c] * (1 - fx) + field.at(x1, y0)[c] * fx) * (1 - fy) +
                 (field.at(x0, y1)[c] * (1 - fx) + field.at(x1, y1)[c] * fx) * fy;
    return out;
}

// The frame passes' inputs: colour (t0), depth (t5), addition or residual (t2), field (t20) and extras by slot.
struct FrameInputs {
    std::vector<ComPtr<ID3D11ShaderResourceView>> keep;
    Views views{};
    void Set(int slot, ComPtr<ID3D11ShaderResourceView> view) { views[slot] = view.Get(); keep.push_back(view); }
    void Clear(int slot) { views[slot] = nullptr; }
};

// Cells (when the config asks for them) then Compose over the inputs' addition (t2), native or on a grid.
inline Image ComposeRun(Gpu &gpu, const std::vector<char> &compose, const std::vector<char> &cells, FrameInputs in,
                        const Root &root, bool withCells, UINT gridW = 0, UINT gridH = 0)
{
    if (withCells) {
        const auto c = Run(gpu, cells, in.views, root, {kLowW, kLowH, 0, 0, 0, 0, gridW, gridH}, Native(kLowW, kLowH));
        in.Set(8, gpu.Srv(c[0]));
        in.Set(1, gpu.Srv(c[1]));
    }
    return Run(gpu, compose, in.views, root, {kW, kH, 0, 0, 0, 0, gridW, gridH}, Native(kW, kH))[0];
}

} // namespace temporal_sky_scenes
