// The passes that form a frame, with the sky field at t20 (core/shaders/temporal_sky.hlsli), on D3D11 WARP:
//   (3) Compose: a far-plane pixel gets colour + field(p) whatever its carried addition (a mosaic of two sky tones
//       comes out smooth); every other pixel is bit for bit the pass without the field; Reproject's direct route
//       likewise, its addition and acceptance (u1) untouched; under 2 % support nothing changes;
//   (4) ComposeGrid on a half grid: sky beside a building takes the field, never the building's upsampled addition;
//   (5) Apply during a phase-in: a sky pixel shows the new residual alone, every other pixel the mix as before;
//   (7) reversed Z: 0 is the far plane and 1 is not;
//   (8) NaN/Inf depth is never sky, a non-finite field texel is never used, and a sky pixel of the policy is finite
//       colour + field(p) even where its carried addition was NaN/Inf;
//   (9) master identity (TestSkyMasterIdentity): see test_temporal_sky_shaders.cpp.
#include "temporal_sky_scenes.h"

#include <cstdio>

using namespace temporal_sky_scenes;

namespace {
constexpr float kSupport = 0.25f;

// Rows 0..63 sky (`farPlane`) with a building (0.4) at x 96..159; rows 64.. ground (0.6), with the other end of the
// depth range (never sky) in rows 100..107; NaN/Inf at a few sky pixels when `poison`.
std::vector<float> SkyDepth(float farPlane, bool poison = false)
{
    std::vector<float> d(kW * kH);
    for (UINT y = 0; y < kH; ++y)
        for (UINT x = 0; x < kW; ++x) {
            float v = y < 64 ? (x >= 96 && x < 160 ? 0.4f : farPlane) : (y >= 100 && y < 108 ? 1.0f - farPlane : 0.6f);
            if (poison && y < 64 && v == farPlane && Hash(x, y, 50) < 0.05f) v = Hash(x, y, 51) < 0.5f ? kNaN : kInf;
            d[y * kW + x] = v;
        }
    return d;
}

bool IsSky(const std::vector<float> &depth, UINT x, UINT y, float farPlane) { return depth[y * kW + x] == farPlane; }

// The carried addition: on the sky a mosaic of two tones (what wrong chain ends gave), the building's edit beside it.
Image MosaicAddition()
{
    Image a = Additions(false);
    for (UINT y = 0; y < 64; ++y)
        for (UINT x = 0; x < kW; ++x)
            a.at(x, y) = x >= 96 && x < 160 ? Texel{2.0f, 2.0f, 2.0f, 1.0f}
                                            : (((x >> 1) + (y >> 1)) % 2 ? Texel{0.25f, 0.25f, 0.25f, 0.96f} : Texel{-0.125f, -0.125f, -0.125f, 0.96f});
    return a;
}

Image GradientField(float support)
{
    Image f(kLowW, kLowH);
    for (UINT y = 0; y < kLowH; ++y)
        for (UINT x = 0; x < kLowW; ++x) f.at(x, y) = {float(x) / 64.0f, float(y) / 64.0f, 0.125f, support};
    return f;
}

// colour + field as the pass writes it: in the ratio domain (s = 1) a log gain on the encoded colour.
float Expected(float colour, float field, bool ratio) { return ratio ? (1.0f + colour) * std::exp(field) - 1.0f : colour + field; }

FrameInputs ComposeInputs(Gpu &gpu, const Image &color, const Image &addition, const std::vector<float> &depth)
{
    FrameInputs in;
    in.Set(0, gpu.Srv(color)); in.Set(2, gpu.Srv(addition)); in.Set(5, DepthView(gpu, kW, kH, depth));
    return in;
}

void TestCompose(Gpu &gpu, const SkyShaders &s, float farPlane)
{
    const Image color = Frame(false), addition = MosaicAddition(), field = GradientField(kSupport);
    const std::vector<float> depth = SkyDepth(farPlane);
    float smoothWith = 0.0f, mosaicWithout = 0.0f;
    std::size_t sky = 0;
    for (const auto &k : kComposeConfigs) {
        const Root root = WithSky(ComposeConstants(k), farPlane);
        FrameInputs off = ComposeInputs(gpu, color, addition, depth), on = off;
        on.Set(kField, gpu.Srv(field));
        const Image a = ComposeRun(gpu, s.compose, s.cells, on, root, k[1] != 0), b = ComposeRun(gpu, s.compose, s.cells, off, root, k[1] != 0);
        const bool exact = k[2] < 0 || k[3] == 0; // no luminance band: the field as written
        for (UINT y = 0; y < kH; ++y)
            for (UINT x = 0; x < kW; ++x) {
                if (!IsSky(depth, x, y, farPlane)) {
                    Require(SameBits(a.at(x, y), b.at(x, y)), "(3) a pixel off the far plane changed with the field bound");
                    continue;
                }
                ++sky;
                const Texel f = SampleField(field, x, y);
                for (int c = 0; c < 3 && exact; ++c)
                    Require(std::abs(a.at(x, y)[c] - Expected(color.at(x, y)[c], f[c], k[3] != 0)) <= 2e-5f * (1.0f + std::abs(a.at(x, y)[c])),
                            "(3) a far-plane pixel is not colour + field(p)");
                if (exact && k[3] == 0 && x + 1 < kW && IsSky(depth, x + 1, y, farPlane)) {
                    smoothWith = std::max(smoothWith, std::abs((a.at(x + 1, y)[0] - color.at(x + 1, y)[0]) - (a.at(x, y)[0] - color.at(x, y)[0])));
                    mosaicWithout = std::max(mosaicWithout, std::abs((b.at(x + 1, y)[0] - color.at(x + 1, y)[0]) - (b.at(x, y)[0] - color.at(x, y)[0])));
                }
            }
    }
    std::printf("(3%s) Compose: %zu far-plane pixel reads take colour + field(p); largest step of the addition along a row %.4g "
                "with the field, %.4g carried\n", farPlane == 0.0f ? ", 7 reversed Z" : "", sky, smoothWith, mosaicWithout);
    Require(smoothWith <= 2.0e-3f, "(3) the sky's addition is not smooth with the field");
    Require(mosaicWithout >= 0.3f, "(3) the scene cannot tell a carried mosaic from the field");
    // Under 2 % support, or no sky at all in the pass (support 0): nothing changes anywhere.
    for (const float support : {1.0f / 128.0f, 0.0f}) {
        const Root root = WithSky(ComposeConstants(kComposeConfigs[0]), farPlane);
        FrameInputs off = ComposeInputs(gpu, color, addition, depth), on = off;
        on.Set(kField, gpu.Srv(FlatField(support == 0.0f ? 0.0f : 0.125f, support)));
        Require(SameBits(ComposeRun(gpu, s.compose, s.cells, on, root, false), ComposeRun(gpu, s.compose, s.cells, off, root, false)),
                "(3) a field without 2 % support changed the frame");
    }
}

void TestDirectRoute(Gpu &gpu, const SkyShaders &s)
{
    ReprojectScene scene(gpu, false, 0.0f);
    std::vector<float> depth(kW * kH);
    for (UINT y = 0; y < kH; ++y)
        for (UINT x = 0; x < kW; ++x) depth[y * kW + x] = y < 30 ? 1.0f : (y < 90 && x >= 100 && x < 160 ? 0.3f : 0.8f);
    auto depthView = DepthView(gpu, kW, kH, depth);
    auto fieldView = gpu.Srv(GradientField(kSupport));
    const Image color = [] { Image c(kW, kH); for (UINT y = 0; y < kH; ++y) for (UINT x = 0; x < kW; ++x)
        c.at(x, y) = {Q(0.2f + 0.3f * x / kW), Q(0.3f + 0.2f * Hash(x, y, 1)), 0.4f, 1.0f}; return c; }();
    for (const auto &k : kReprojectConfigs) {
        Views on = scene.views, off = scene.views;
        on[5] = off[5] = depthView.Get();
        on[kField] = fieldView.Get();
        const Root root = WithSky(ReprojectConstants(k), 1.0f);
        const auto a = Run(gpu, s.reproject, on, root, {kW, kH}, Native(kW, kH)), b = Run(gpu, s.reproject, off, root, {kW, kH}, Native(kW, kH));
        Require(SameBits(a[1], b[1]), "(3) the direct route's addition and acceptance (u1) changed with the field");
        const bool exact = k[6] < 0 || k[7] == 0;
        for (UINT y = 0; y < kH; ++y)
            for (UINT x = 0; x < kW; ++x) {
                if (y >= 30) { Require(SameBits(a[0].at(x, y), b[0].at(x, y)), "(3) the direct route changed a pixel off the far plane"); continue; }
                const Texel f = SampleField(GradientField(kSupport), x, y);
                for (int c = 0; c < 3 && exact; ++c)
                    Require(std::abs(a[0].at(x, y)[c] - Expected(color.at(x, y)[c], f[c], k[7] != 0)) <= 2e-5f * (1.0f + std::abs(a[0].at(x, y)[c])),
                            "(3) the direct route's far-plane pixel is not colour + field(p)");
            }
    }
    std::printf("(3) Reproject's direct route: sky rows take colour + field(p); u1 and every other pixel bit for bit\n");
}

void TestGrid(Gpu &gpu, const SkyShaders &s)
{
    constexpr UINT gw = kW / 2, gh = kH / 2;
    Image addition(kW, kH, {kNaN, kNaN, kNaN, kNaN});
    for (UINT y = 0; y < gh; ++y)
        for (UINT x = 0; x < gw; ++x) addition.at(x, y) = x < gw / 2 ? Texel{0.0625f, 0.0625f, 0.0625f, 1.0f} : Texel{2.0f, 2.0f, 2.0f, 1.0f};
    std::vector<float> depth(kW * kH);
    for (UINT y = 0; y < kH; ++y)
        for (UINT x = 0; x < kW; ++x) depth[y * kW + x] = x < kW / 2 ? 1.0f : 0.4f;
    const Image color = Frame(true);
    const float cfg[5] = {0.08f, 0, -1, 0, 6};
    const Root root = WithSky(ComposeConstants(cfg), 1.0f);
    FrameInputs off = ComposeInputs(gpu, color, addition, depth), on = off;
    on.Set(kField, gpu.Srv(FlatField(0.03125f, kSupport)));
    const Image a = ComposeRun(gpu, s.composeGrid, s.cellsGrid, on, root, false, gw, gh);
    const Image b = ComposeRun(gpu, s.composeGrid, s.cellsGrid, off, root, false, gw, gh);
    float bleed = 0.0f;
    for (UINT y = 0; y < kH; ++y)
        for (UINT x = 0; x < kW; ++x) {
            if (x >= kW / 2) { Require(SameBits(a.at(x, y), b.at(x, y)), "(4) the building changed with the field bound"); continue; }
            for (int c = 0; c < 3; ++c) Require(a.at(x, y)[c] == color.at(x, y)[c] + 0.03125f, "(4) a sky pixel on the grid is not colour + field");
            bleed = std::max(bleed, b.at(x, y)[0] - color.at(x, y)[0]);
        }
    std::printf("(4) half grid: sky beside the building takes the field; without it the upsampled addition reached %.3g there\n", bleed);
    Require(bleed > 0.1f, "(4) the scene cannot tell the building's upsampled addition on the sky");
}

void TestApply(Gpu &gpu, const SkyShaders &s)
{
    Image fresh(kW, kH), older(kW, kH);
    for (UINT y = 0; y < kH; ++y)
        for (UINT x = 0; x < kW; ++x) {
            fresh.at(x, y) = {Q(Hash(x, y, 60) - 0.5f) * 0.5f, Q(Hash(x, y, 61)) * 0.25f, 0.125f, 0.0f};
            older.at(x, y) = {Q(Hash(x, y, 62) - 0.5f), -0.25f, Q(Hash(x, y, 63)) * 0.5f, 0.0f};
        }
    const std::vector<float> depth = SkyDepth(1.0f);
    auto root = [](float share) { return WithSky(Constants({0, 0, 0, 0.05f}, {0.08f, 1, 0, 0}, {1, float(kLowW), float(kLowH), 1}, {-1, 24, 0, share}), 1.0f); };
    FrameInputs off = ComposeInputs(gpu, Frame(false), fresh, depth);
    off.Set(10, gpu.Srv(older));
    FrameInputs on = off;
    on.Set(kField, gpu.Srv(FlatField(0.0f, kSupport)));
    const Image mixedOn = Run(gpu, s.apply, on.views, root(0.5f), {kW, kH}, Native(kW, kH))[0];
    const Image mixedOff = Run(gpu, s.apply, off.views, root(0.5f), {kW, kH}, Native(kW, kH))[0];
    const Image newOnly = Run(gpu, s.apply, off.views, root(1.0f), {kW, kH}, Native(kW, kH))[0];
    std::size_t sky = 0;
    for (UINT y = 0; y < kH; ++y)
        for (UINT x = 0; x < kW; ++x) {
            const bool isSky = IsSky(depth, x, y, 1.0f);
            sky += isSky ? 1 : 0;
            Require(SameBits(mixedOn.at(x, y), isSky ? newOnly.at(x, y) : mixedOff.at(x, y)),
                    isSky ? "(5) a sky pixel of the phase-in is not the new residual alone" : "(5) the phase-in changed a pixel off the far plane");
        }
    Require(!SameBits(mixedOff, newOnly), "(5) the scene cannot tell the mix from the new residual");
    std::printf("(5) Apply during a phase-in: %zu sky pixels show the new residual alone, the rest the mix as before\n", sky);
}

// NaN/Inf in the depth, the field and the carried addition. Where the policy applies (a finite far-plane depth, a
// finite field footprint) the frame is finite and colour + field(p), even where the carried addition was NaN or Inf;
// everywhere else it is bit for bit the frame without the field.
void TestNonFinite(Gpu &gpu, const SkyShaders &s)
{
    const std::vector<float> depth = SkyDepth(1.0f, true);
    Image field = GradientField(kSupport);
    field.at(3, 1) = {kNaN, 0.0f, kInf, kSupport};
    const Image color = Frame(false);
    Image addition = MosaicAddition();
    const float odd[3] = {kNaN, kInf, -kInf};
    for (UINT y = 0; y < kH; ++y)
        for (UINT x = 0; x < kW; ++x)
            if (Hash(x, y, 52) < 0.1f) addition.at(x, y)[x % 3] = odd[y % 3];
    const Root root = WithSky(ComposeConstants(kComposeConfigs[0]), 1.0f);
    FrameInputs off = ComposeInputs(gpu, color, addition, depth), on = off;
    on.Set(kField, gpu.Srv(field));
    const Image a = ComposeRun(gpu, s.compose, s.cells, on, root, false), b = ComposeRun(gpu, s.compose, s.cells, off, root, false);
    std::size_t kept = 0, rescued = 0, applied = 0;
    for (UINT y = 0; y < kH; ++y)
        for (UINT x = 0; x < kW; ++x) {
            const float d = depth[y * kW + x];
            const Texel f = SampleField(field, x, y); // non-finite where the bilinear footprint holds texel (3, 1)
            const bool policy = std::isfinite(d) && d == 1.0f && std::isfinite(f[0]) && std::isfinite(f[1]) && std::isfinite(f[2]);
            if (!policy) {
                Require(SameBits(a.at(x, y), b.at(x, y)), "(8) a pixel the policy does not apply to changed with the field bound");
                kept += std::isfinite(d) ? 0 : 1;
                continue;
            }
            ++applied;
            bool carriedFinite = true;
            for (int c = 0; c < 3; ++c) {
                Require(std::isfinite(a.at(x, y)[c]), "(8) a sky pixel of the policy came out NaN or Inf");
                Require(std::abs(a.at(x, y)[c] - (color.at(x, y)[c] + f[c])) <= 2e-5f * (1.0f + std::abs(a.at(x, y)[c])),
                        "(8) a sky pixel of the policy is not colour + field(p)");
                carriedFinite = carriedFinite && std::isfinite(b.at(x, y)[c]);
            }
            rescued += carriedFinite ? 0 : 1;
        }
    std::printf("(8) non-finite inputs: %zu sky pixels take colour + field(p), all finite (%zu of them had a NaN/Inf carried "
                "addition); %zu NaN/Inf-depth pixels and every non-finite field footprint keep the frame without the field\n",
                applied, rescued, kept);
    Require(rescued > 0 && kept > 0, "(8) the scene lacks NaN/Inf carried additions on the sky or NaN/Inf depths");
}
} // namespace

void TestSkyComposition(Gpu &gpu, const SkyShaders &s)
{
    TestCompose(gpu, s, 1.0f);
    TestCompose(gpu, s, 0.0f);
    TestDirectRoute(gpu, s);
    TestGrid(gpu, s);
    TestApply(gpu, s);
    TestNonFinite(gpu, s);
}
