// (9) The off path against master (tests/fixtures/temporal_pre_sky), on D3D11 WARP: Reproject (both u0 routes),
// Compose, ComposeGrid and Apply from the current source write master's bytes when the sky field is
//   - unbound (TemporalSkyField off, a menu frame, no field built);
//   - bound with less than 2 % support while the frame has far-plane pixels;
//   - bound with support while the frame has no far-plane pixel at all.
#include "temporal_sky_scenes.h"

#include <cstdio>

using namespace temporal_sky_scenes;

namespace {
struct Case { const char *name; bool bound; float support; bool sky; };
constexpr Case kCases[] = {{"unbound", false, 0.0f, true}, {"under 2 % support", true, 1.0f / 128.0f, true},
                           {"no far-plane pixel", true, 0.5f, false}};

std::vector<float> Depth(bool sky)
{
    std::vector<float> d(kW * kH);
    for (UINT y = 0; y < kH; ++y)
        for (UINT x = 0; x < kW; ++x)
            d[y * kW + x] = y < 40 && (x < 96 || x >= 160) ? (sky ? 1.0f : 0.999f) : (y >= 30 && y < 90 && x >= 100 && x < 160 ? 0.3f : 0.8f);
    return d;
}

void Reproject(Gpu &gpu, const SkyShaders &now, const SkyShaders &master, const Case &c)
{
    for (const bool constant : {false, true}) {
        ReprojectScene scene(gpu, constant, 0.125f);
        auto depth = DepthView(gpu, kW, kH, Depth(c.sky));
        auto field = gpu.Srv(FlatField(0.25f, c.support));
        Views views = scene.views;
        views[5] = depth.Get();
        if (c.bound) views[kField] = field.Get();
        for (const auto &k : kReprojectConfigs)
            for (const std::uint32_t skip : {0u, 1u}) {
                const Root root = WithSky(ReprojectConstants(k), 1.0f);
                const Texel sentinel{12345.0f, -6789.0f, 42.0f, 7.0f};
                const Lanes lanes{kW, kH, 0, 0, 0, skip, 0, 0};
                const auto a = Run(gpu, now.reproject, views, root, lanes, Native(kW, kH, sentinel));
                const auto b = Run(gpu, master.reproject, views, root, lanes, Native(kW, kH, sentinel));
                Require(SameBits(a[0], b[0]) && SameBits(a[1], b[1]), "(9) Reproject differs from master with the sky field off");
            }
    }
}

void Compose(Gpu &gpu, const SkyShaders &now, const SkyShaders &master, const Case &c)
{
    const Image color = Frame(false);
    const std::vector<float> depth = Depth(c.sky);
    for (const bool grid : {false, true}) {
        const UINT gw = grid ? kW / 2 : 0, gh = grid ? kH / 2 : 0;
        const Image addition = Additions(!grid); // NaN and infinities among the native additions
        for (const auto &k : kComposeConfigs) {
            FrameInputs in;
            in.Set(0, gpu.Srv(color)); in.Set(2, gpu.Srv(addition)); in.Set(5, DepthView(gpu, kW, kH, depth));
            if (c.bound) in.Set(kField, gpu.Srv(FlatField(0.25f, c.support)));
            const Root root = WithSky(ComposeConstants(k), 1.0f);
            const auto &cells = grid ? now.cellsGrid : now.cells; // instruction-identical to master's
            const Image a = ComposeRun(gpu, grid ? now.composeGrid : now.compose, cells, in, root, k[1] != 0, gw, gh);
            const Image b = ComposeRun(gpu, grid ? master.composeGrid : master.compose, cells, in, root, k[1] != 0, gw, gh);
            Require(SameBits(a, b), grid ? "(9) ComposeGrid differs from master with the sky field off" : "(9) Compose differs from master with the sky field off");
        }
    }
}

void Apply(Gpu &gpu, const SkyShaders &now, const SkyShaders &master, const Case &c)
{
    Image fresh(kW, kH), older(kW, kH);
    for (UINT y = 0; y < kH; ++y)
        for (UINT x = 0; x < kW; ++x) {
            fresh.at(x, y) = {Q(Hash(x, y, 70) - 0.5f), Q(Hash(x, y, 71)) * 0.25f, 0.125f, 0.0f};
            older.at(x, y) = {Q(Hash(x, y, 72) - 0.5f), -0.25f, Hash(x, y, 73) < 0.02f ? kNaN : 0.5f, 0.0f};
        }
    FrameInputs in;
    in.Set(0, gpu.Srv(Frame(false))); in.Set(2, gpu.Srv(fresh)); in.Set(10, gpu.Srv(older));
    in.Set(5, DepthView(gpu, kW, kH, Depth(c.sky)));
    if (c.bound) in.Set(kField, gpu.Srv(FlatField(0.0f, c.support)));
    for (const float band : {-1.0f, 0.0f})
        for (const float ratio : {0.0f, 1.0f})
            for (const float share : {0.25f, 1.0f}) {
                const Root root = WithSky(Constants({0, 0, 0, 0.05f}, {0.08f, 1, 0, 0}, {1, float(kLowW), float(kLowH), 1}, {band, 24, ratio, share}), 1.0f);
                Require(SameBits(Run(gpu, now.apply, in.views, root, {kW, kH}, Native(kW, kH))[0],
                                 Run(gpu, master.apply, in.views, root, {kW, kH}, Native(kW, kH))[0]),
                        "(9) Apply differs from master with the sky field off");
            }
}
} // namespace

void TestSkyMasterIdentity(Gpu &gpu, const SkyShaders &now, const SkyShaders &master)
{
    for (const Case &c : kCases) {
        Reproject(gpu, now, master, c);
        Compose(gpu, now, master, c);
        Apply(gpu, now, master, c);
        std::printf("(9) field %s: Reproject, Compose, ComposeGrid and Apply write master's bytes\n", c.name);
    }
}
