// The carried-frame grid's compute passes on D3D11 WARP (core/shaders/temporal_cs.hlsl, PW_T_GRID):
//   (a) against the pre-step-B baseline (tests/fixtures/temporal_pre_grid): Reproject, Cells and Compose from the
//       current source write the same bytes;
//   (b) a grid of the native size (power of two): ReprojectGrid writes the native addition bit for bit; Compose
//       pixels that take the share-0 shortcut match exactly, the rest stay within a tolerance (max/mean printed);
//   (c) half grid: ReprojectGrid writes only the grid corner; with a constant addition the frame is colour +
//       that constant, with NaN or a large value everywhere outside the corner (nothing outside is read);
//   (e) half grid of differing additions and acceptances, lone marks among them: CellsGrid + ComposeGrid write the
//       same bytes as Cells + Compose over the grid upsampled on the CPU with bilinear RGB and the NEAREST texel's
//       acceptance, and differ from an upsampling with interpolated acceptance or with the lone mark lost.
// Usage: ofps_temporal_grid_shaders_tests <shader dir> [<pre-step-B shader dir>]
#include "temporal_grid_scenes.h"

#include <algorithm>
#include <cstdio>

using namespace temporal_grid_scenes;

namespace {
void TestReference(Gpu &gpu, const Shaders &now, const Shaders &before)
{
    ReprojectScene scene(gpu, false, 0.0f);
    const Texel sentinel{12345.0f, -6789.0f, 42.0f, 7.0f};
    for (const auto &k : kReprojectConfigs)
        for (std::uint32_t skip : {0u, 1u}) {
            const Lanes lanes{kW, kH, 0, 0, 0, skip, 0, 0};
            const auto a = Run(gpu, now.reproject, scene.views, ReprojectConstants(k), lanes, Native(kW, kH, sentinel));
            const auto b = Run(gpu, before.reproject, scene.views, ReprojectConstants(k), lanes, Native(kW, kH, sentinel));
            Require(SameBits(a[0], b[0]) && SameBits(a[1], b[1]), "(a) Reproject differs from the pre-step-B build");
        }
    const Image color = Frame(false), addition = Additions(true);
    for (const auto &k : kComposeConfigs) {
        Require(SameBits(ComposeChain(gpu, now, color, addition, k, 0, 0), ComposeChain(gpu, before, color, addition, k, 0, 0)),
                "(a) Cells + Compose differ from the pre-step-B build");
    }
    std::printf("(a) Reproject, Cells and Compose are byte-identical to the pre-step-B build\n");
}

void TestGridNative(Gpu &gpu, const Shaders &s)
{
    ReprojectScene scene(gpu, false, 0.0f);
    for (const auto &k : kReprojectConfigs) {
        const auto native = Run(gpu, s.reproject, scene.views, ReprojectConstants(k), {kW, kH, 0, 0, 0, 1, 0, 0}, Native(kW, kH));
        const auto grid = Run(gpu, s.reprojectGrid, scene.views, ReprojectConstants(k), {kW, kH, 0, 0, 0, 1, kW, kH}, Native(kW, kH));
        Require(SameBits(native[1], grid[1]), "(b) ReprojectGrid at the native size differs from Reproject");
    }
    const Image color = Frame(false), addition = Additions(false);
    double maxDiff = 0.0, sumDiff = 0.0;
    std::size_t samples = 0, shortcut = 0;
    for (const auto &k : kComposeConfigs) {
        const Image native = ComposeChain(gpu, s, color, addition, k, 0, 0), grid = ComposeChain(gpu, s, color, addition, k, kW, kH);
        for (UINT y = 0; y < kH; ++y)
            for (UINT x = 0; x < kW; ++x) {
                const Texel &aw = addition.at(x, y);
                // The centre read alone decides: no smoothing at all, or a firmly accepted, nonzero addition (share 0).
                if (k[4] == 0 || (aw[3] >= 0.9f && aw[0] != 0.0f && aw[1] != 0.0f && aw[2] != 0.0f)) {
                    Require(SameBits(native.at(x, y), grid.at(x, y)), "(b) a share-0 pixel differs on a native-size grid");
                    ++shortcut;
                }
                for (int c = 0; c < 4; ++c) {
                    const double d = std::abs(double(native.at(x, y)[c]) - double(grid.at(x, y)[c]));
                    Require(std::isfinite(d), "(b) a non-finite pixel on a native-size grid");
                    maxDiff = std::max(maxDiff, d); sumDiff += d; ++samples;
                }
            }
    }
    std::printf("(b) native-size grid: Reproject bit-identical; %zu share-0 pixels exact; Compose max |diff| %.3g, mean %.3g\n",
                shortcut, maxDiff, sumDiff / double(samples));
    Require(maxDiff <= 1.0e-3, "(b) Compose on a native-size grid is outside the tolerance");
}

void TestGridHalf(Gpu &gpu, const Shaders &s)
{
    constexpr UINT gw = kW / 2, gh = kH / 2;
    constexpr float k = 0.125f;
    auto outside = [](UINT x, UINT y) { return x >= gw || y >= gh; };
    // colour + the addition as the pass writes it: in the ratio domain (s = 1) the addition is a log gain.
    auto expected = [](float colour, float add, const float (&cfg)[5]) {
        return cfg[3] != 0 ? (1.0f + colour) * std::exp(add) - 1.0f : colour + add;
    };
    // The reprojection of a still grey frame with a constant residual writes the grid corner and nothing else.
    ReprojectScene scene(gpu, true, k);
    const Texel sentinel{kNaN, kNaN, kNaN, kNaN};
    const auto written = Run(gpu, s.reprojectGrid, scene.views, ReprojectConstants(kReprojectConfigs[0]), {gw, gh, 0, 0, 0, 1, gw, gh},
                             Native(kW, kH, sentinel));
    for (UINT y = 0; y < kH; ++y)
        for (UINT x = 0; x < kW; ++x) {
            Require(SameBits(written[0].at(x, y), sentinel), "(c) ReprojectGrid wrote u0");
            const Texel &t = written[1].at(x, y);
            if (outside(x, y)) Require(SameBits(t, sentinel), "(c) ReprojectGrid wrote outside the grid corner");
            else Require(std::abs(t[0] - k) < 1e-4f && std::abs(t[1] - k) < 1e-4f && std::abs(t[2] - k) < 1e-4f,
                         "(c) the grid's addition is not the constant residual");
        }
    // Its addition (NaN outside the corner) through CellsGrid + ComposeGrid: colour + the constant, everywhere.
    const Image grey = Frame(true);
    for (const auto &cfg : kComposeConfigs) {
        const Image out = ComposeChain(gpu, s, grey, written[1], cfg, gw, gh);
        for (const Texel &t : out.texels)
            for (int c = 0; c < 3; ++c)
                Require(std::abs(t[c] - expected(0.25f, k, cfg)) < 2e-4f, "(c) half grid: the frame is not colour + the constant addition");
    }
    // A corner of rejected and lone pixels (smoothing taps and cells all run) with a large value or NaN outside.
    const Image color = Frame(false);
    for (const float poison : {1024.0f, kNaN}) {
        Image addition(kW, kH, {poison, poison, poison, 1.0f});
        const float alphas[4] = {0.3f, -1.0f, 0.1f, 0.6f};
        for (UINT y = 0; y < gh; ++y)
            for (UINT x = 0; x < gw; ++x) addition.at(x, y) = {k, k, k, alphas[(x * 7 + y * 3) % 4]};
        for (const auto &cfg : kComposeConfigs) {
            const Image out = ComposeChain(gpu, s, color, addition, cfg, gw, gh);
            for (UINT y = 0; y < kH; ++y)
                for (UINT x = 0; x < kW; ++x)
                    for (int c = 0; c < 3; ++c)
                        Require(std::abs(out.at(x, y)[c] - expected(color.at(x, y)[c], k, cfg)) < 2e-4f,
                                std::isnan(poison) ? "(c) half grid read a NaN texel outside the corner"
                                                   : "(c) half grid read a texel outside the corner");
        }
    }
    std::printf("(c) half grid: only the corner is written; the frame is colour + the addition with NaN/1024 outside it\n");
}

enum class Acceptance { Nearest, Interpolated, LoneLost };

// What PwTAddition reads from a grid corner for every native pixel: RGB bilinear at the pixel's centre, clamped to
// [0.5, grid - 0.5 - 1/64] grid texels, and the acceptance of the nearest texel - or, to show the scene can tell,
// an interpolated acceptance, or the nearest one with the lone mark (-1) lost. Power-of-two sizes and values on a
// 1/256 step keep every product and sum exact, so this matches the GPU's filter bit for bit.
Image Upsample(const Image &grid, Acceptance acceptance)
{
    struct Axis { UINT i0, i1, nearest; float f; };
    auto axis = [](UINT i, UINT g, UINT n) {
        const float centre = (float(i) + 0.5f) * (float(g) / float(n));
        const float q = std::clamp(centre, 0.5f, float(g) - (g < n ? 0.515625f : 0.5f)) - 0.5f;
        const UINT i0 = UINT(std::floor(q));
        return Axis{i0, std::min(i0 + 1, g - 1), std::min(UINT(centre), g - 1), q - float(i0)};
    };
    Image out(kW, kH);
    for (UINT y = 0; y < kH; ++y)
        for (UINT x = 0; x < kW; ++x) {
            const Axis ax = axis(x, grid.width, kW), ay = axis(y, grid.height, kH);
            const Texel &t00 = grid.at(ax.i0, ay.i0), &t10 = grid.at(ax.i1, ay.i0), &t01 = grid.at(ax.i0, ay.i1), &t11 = grid.at(ax.i1, ay.i1);
            const float w00 = (1 - ax.f) * (1 - ay.f), w10 = ax.f * (1 - ay.f), w01 = (1 - ax.f) * ay.f, w11 = ax.f * ay.f;
            Texel &o = out.at(x, y);
            for (int c = 0; c < 4; ++c) o[c] = t00[c] * w00 + t10[c] * w10 + t01[c] * w01 + t11[c] * w11;
            const float nearest = grid.at(ax.nearest, ay.nearest)[3];
            if (acceptance == Acceptance::Nearest) o[3] = nearest;
            if (acceptance == Acceptance::LoneLost) o[3] = std::max(nearest, 0.0f);
        }
    return out;
}

void TestGridAcceptance(Gpu &gpu, const Shaders &s)
{
    constexpr UINT gw = kW / 2, gh = kH / 2;
    // Every grid texel's addition differs from its neighbours', and so does its acceptance; -1 sits beside 1, 0.6 and 0.35.
    const float alphas[6] = {1.0f, -1.0f, 0.6f, 0.0f, 0.35f, -1.0f};
    Image grid(gw, gh), texture(kW, kH, {kNaN, kNaN, kNaN, kNaN});
    for (UINT y = 0; y < gh; ++y)
        for (UINT x = 0; x < gw; ++x) {
            Texel t{};
            for (int c = 0; c < 3; ++c) t[c] = Q(Hash(x, y, 30 + c) - 0.5f) + 1.0f / 256.0f;
            t[3] = alphas[(x + 2 * y) % 6];
            grid.at(x, y) = texture.at(x, y) = t;
        }
    const Image color = Frame(false);
    const Image nearest = Upsample(grid, Acceptance::Nearest), interpolated = Upsample(grid, Acceptance::Interpolated),
                lost = Upsample(grid, Acceptance::LoneLost);
    std::size_t interpolatedDiffers = 0, lostDiffers = 0;
    auto differing = [](const Image &a, const Image &b) {
        std::size_t n = 0;
        for (std::size_t i = 0; i < a.texels.size(); ++i) n += SameBits(a.texels[i], b.texels[i]) ? 0 : 1;
        return n;
    };
    for (const auto &cfg : kComposeConfigs) {
        const Image onGrid = ComposeChain(gpu, s, color, texture, cfg, gw, gh);
        Require(SameBits(onGrid, ComposeChain(gpu, s, color, nearest, cfg, 0, 0)),
                "(e) the grid passes do not read bilinear RGB with the nearest texel's acceptance");
        interpolatedDiffers += differing(onGrid, ComposeChain(gpu, s, color, interpolated, cfg, 0, 0));
        lostDiffers += differing(onGrid, ComposeChain(gpu, s, color, lost, cfg, 0, 0));
    }
    std::printf("(e) half grid: bilinear RGB with the nearest acceptance, bit for bit; an interpolated acceptance would change "
                "%zu pixels, a lost lone mark %zu\n", interpolatedDiffers, lostDiffers);
    Require(interpolatedDiffers > 0, "(e) the scene cannot tell an interpolated acceptance from the nearest one");
    Require(lostDiffers > 0, "(e) the scene cannot tell a lost lone mark");
}
} // namespace

int main(int argc, char **argv)
{
    try {
        Require(argc == 2 || argc == 3, "usage: ofps_temporal_grid_shaders_tests <shader dir> [<pre-step-B shader dir>]");
        Gpu gpu;
        const Shaders shaders(argv[1]);
        if (argc == 3) TestReference(gpu, shaders, Shaders(argv[2], false));
        TestGridNative(gpu, shaders);
        TestGridHalf(gpu, shaders);
        TestGridAcceptance(gpu, shaders);
        std::printf("PASS: temporal grid passes on WARP\n");
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
