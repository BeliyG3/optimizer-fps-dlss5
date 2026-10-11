// The sky field's passes on D3D11 WARP (core/shaders/temporal_sky.hlsli, TemporalSkyField):
//   (1) reduction: CSSkyDown is the exact masked average of a 4x4 point lattice per block, with its coverage, through a
//       depth guide at a sub-rect offset and half the native resolution; the reversed-Z far plane too; NaN/Inf and
//       depths beside the far plane never count, nor a non-finite residual;
//   (2) push-pull: a sky corner fills the whole frame with finite values inside the sky's range, its own texels kept;
//       no sky gives (0, 0) everywhere; the support every texel carries is the 1x1 coverage; on odd grids (240x135,
//       15x9) the 1x1 level is the true share of sky and the true coverage-weighted mean;
//   (9) off path: Reproject, Compose, ComposeGrid and Apply write master's bytes (tests/fixtures/temporal_pre_sky) with
//       the field unbound, bound with less than 2 % support, or bound while the frame has no far-plane pixel.
// The frame-forming passes with the field on: test_temporal_sky_compose.cpp; the field weighed by the carried frame's
// own share of sky (fix round 2): test_temporal_sky_gate.cpp.
// Usage: ofps_temporal_sky_shaders_tests <shader dir> <master fixture dir> <round 1 fixture dir>
#include "temporal_sky_scenes.h"

#include <cstdio>

using namespace temporal_sky_scenes;

void TestSkyComposition(Gpu &gpu, const SkyShaders &s); // test_temporal_sky_compose.cpp
void TestSkyGate(Gpu &gpu, const SkyShaders &s, const SkyShaders &master, const SkyShaders &round1); // test_temporal_sky_gate.cpp

namespace {
constexpr UINT kDepthW = 160, kDepthH = 80;
const Texel kDepthRect{8, 4, 128, 64}; // half the native resolution, offset inside a larger texture

// The guide texel a native pixel reads (PwTRectTexel), in the shader's own float steps.
std::pair<UINT, UINT> GuideTexel(float px, float py)
{
    auto axis = [](float p, float native, float x, float w) {
        return UINT(std::clamp(x + p * (1.0f / native) * w, x, x + w - 1.0f));
    };
    return {axis(px, float(kW), kDepthRect[0], kDepthRect[2]), axis(py, float(kH), kDepthRect[1], kDepthRect[3])};
}

// The guide: blocks 0-1 all sky, 2-3 none, the rest a mix of sky, values beside it, NaN, Inf, 0 and linear depth.
std::vector<float> Guide(float farPlane)
{
    const float nearEnd = farPlane == 1.0f ? 0.0f : 1.0f; // the other end: never sky
    std::vector<float> d(kDepthW * kDepthH, farPlane);  // the border outside the rect: a wrong mapping would read sky here
    for (UINT y = 0; y < UINT(kDepthRect[3]); ++y)
        for (UINT x = 0; x < UINT(kDepthRect[2]); ++x) {
            const float h = Hash(x, y, 40);
            const UINT block = x / 8; // 16 native px = 8 guide texels
            float v = h < 0.4f ? farPlane : h < 0.5f ? 0.99999994f : h < 0.55f ? 1.0000001f : h < 0.6f ? kNaN : h < 0.65f ? kInf
                    : h < 0.7f ? nearEnd : h < 0.75f ? 2.5f : h < 0.8f ? -kInf : 0.5f;
            if (block < 2) v = farPlane;
            else if (block < 4) v = 0.5f;
            d[(y + UINT(kDepthRect[1])) * kDepthW + x + UINT(kDepthRect[0])] = v;
        }
    return d;
}

Image Residual()
{
    Image r(kW, kH);
    for (UINT y = 0; y < kH; ++y)
        for (UINT x = 0; x < kW; ++x) {
            r.at(x, y) = {Q(Hash(x, y, 41) - 0.5f) * 0.5f, Q(Hash(x, y, 42)) * 0.25f, -Q(Hash(x, y, 43)) * 0.125f, 1.0f};
            const float h = Hash(x, y, 44);
            if (h < 0.03f) r.at(x, y)[1] = kNaN;
            else if (h < 0.05f) r.at(x, y)[0] = kInf;
        }
    return r;
}

Image ReferenceDown(const std::vector<float> &guide, const Image &residual, float farPlane)
{
    Image out(kLowW, kLowH);
    for (UINT ty = 0; ty < kLowH; ++ty)
        for (UINT tx = 0; tx < kLowW; ++tx) {
            double sum[3] = {};
            int n = 0;
            for (int k = 0; k < 16; ++k) {
                const float px = std::min(std::floor(float(tx) * 16.0f + (float(k & 3) + 0.5f) * 4.0f), float(kW) - 1.0f);
                const float py = std::min(std::floor(float(ty) * 16.0f + (float(k >> 2) + 0.5f) * 4.0f), float(kH) - 1.0f);
                const auto [gx, gy] = GuideTexel(px + 0.5f, py + 0.5f);
                const float d = guide[gy * kDepthW + gx];
                const Texel &r = residual.at(UINT(px), UINT(py));
                if (!std::isfinite(d) || d != farPlane || !std::isfinite(r[0]) || !std::isfinite(r[1]) || !std::isfinite(r[2])) continue;
                for (int c = 0; c < 3; ++c) sum[c] += r[c];
                ++n;
            }
            for (int c = 0; c < 3; ++c) out.at(tx, ty)[c] = n ? float(sum[c] / n) : 0.0f;
            out.at(tx, ty)[3] = float(n) / 16.0f;
        }
    return out;
}

void TestReduction(Gpu &gpu, const SkyShaders &s)
{
    const Image residual = Residual();
    auto residualView = gpu.Srv(residual);
    for (const float farPlane : {1.0f, 0.0f}) {
        const std::vector<float> guide = Guide(farPlane);
        auto depth = DepthView(gpu, kDepthW, kDepthH, guide);
        Views v{};
        v[2] = residualView.Get(); v[6] = depth.Get();
        const Image got = Run(gpu, s.down, v, FieldRoot(farPlane, kDepthRect), {kLowW, kLowH}, Native(kLowW, kLowH))[0];
        const Image want = ReferenceDown(guide, residual, farPlane);
        float worst = 0.0f;
        int full = 0, none = 0, partial = 0;
        for (UINT y = 0; y < kLowH; ++y)
            for (UINT x = 0; x < kLowW; ++x) {
                const Texel &g = got.at(x, y), &w = want.at(x, y);
                Require(g[3] == w[3], "(1) the coverage is not the exact share of far-plane samples");
                for (int c = 0; c < 3; ++c) {
                    Require(std::isfinite(g[c]), "(1) the reduction wrote a non-finite mean");
                    worst = std::max(worst, std::abs(g[c] - w[c]));
                }
                (w[3] == 1.0f ? full : w[3] == 0.0f ? none : partial) += 1;
            }
        std::printf("(1) reduction, far plane %.0f: %d full, %d partial, %d empty texels; max |mean - reference| %.3g\n", farPlane, full,
                    partial, none, worst);
        Require(worst <= 1.0e-6f, "(1) the reduction is not the masked average of its samples");
        Require(full > 0 && none > 0 && partial > 0, "(1) the scene lacks full, partial or empty blocks");
    }
}

// A residual of `sky` where the guide is at the far plane, 2.0 (a building's edit) elsewhere.
Field FieldFor(Gpu &gpu, const SkyShaders &s, const std::vector<float> &depth, float sky, float farPlane = 1.0f)
{
    Image r(kW, kH, {2.0f, 2.0f, 2.0f, 1.0f});
    for (UINT y = 0; y < kH; ++y)
        for (UINT x = 0; x < kW; ++x)
            if (depth[y * kW + x] == farPlane) r.at(x, y) = {sky + Q(float(x + y) / 512.0f), sky, sky, 1.0f};
    auto rv = gpu.Srv(r);
    auto dv = DepthView(gpu, kW, kH, depth);
    return BuildField(gpu, s, rv.Get(), dv.Get(), FieldRoot(farPlane));
}

void TestPushPull(Gpu &gpu, const SkyShaders &s)
{
    // Sky in the top-left 32x32 corner only: the field fills the whole frame from it.
    std::vector<float> corner(kW * kH, 0.5f);
    float lo = 1e9f, hi = -1e9f;
    for (UINT y = 0; y < 32; ++y)
        for (UINT x = 0; x < 32; ++x) {
            corner[y * kW + x] = 1.0f;
            lo = std::min(lo, 0.125f + Q(float(x + y) / 512.0f));
            hi = std::max(hi, 0.125f + Q(float(x + y) / 512.0f));
        }
    const Field f = FieldFor(gpu, s, corner, 0.125f);
    const float support = f.pulled.back().at(0, 0)[3];
    Require(support == 4.0f / 128.0f, "(2) the 1x1 coverage is not the frame's share of sky blocks");
    float step = 0.0f;
    for (UINT y = 0; y < kLowH; ++y)
        for (UINT x = 0; x < kLowW; ++x) {
            const Texel &t = f.Finest().at(x, y);
            Require(std::isfinite(t[0]) && std::isfinite(t[1]) && std::isfinite(t[2]), "(2) the filled field is not finite");
            Require(t[3] == support, "(2) a texel does not carry the 1x1 coverage as the support");
            Require(t[0] >= lo - 1e-6f && t[0] <= hi + 1e-6f && t[1] >= 0.125f - 1e-6f && t[1] <= 0.125f + 1e-6f,
                    "(2) the filled field leaves the sky's range (a building's edit came through)");
            if (x < 2 && y < 2) Require(t[0] == f.pulled[0].at(x, y)[0], "(2) a sky block lost its own mean");
            if (x + 1 < kLowW) step = std::max(step, std::abs(f.Finest().at(x + 1, y)[0] - t[0]));
            if (y + 1 < kLowH) step = std::max(step, std::abs(f.Finest().at(x, y + 1)[0] - t[0]));
        }
    std::printf("(2) push-pull: a 32x32 sky corner fills the frame, support %.5f, largest step between texels %.4g (sky range %.4g)\n",
                support, step, hi - lo);
    // No sky at all: every level stays (0, 0).
    const Field none = FieldFor(gpu, s, std::vector<float>(kW * kH, 0.5f), 0.125f);
    for (const auto *levels : {&none.pulled, &none.filled})
        for (const Image &level : *levels)
            for (const Texel &t : level.texels)
                Require(t[0] == 0.0f && t[1] == 0.0f && t[2] == 0.0f && t[3] == 0.0f, "(2) a frame without sky left a value in the field");
    // One sky block of 128: support 1/128, under the 2 % rule (test_temporal_sky_compose.cpp shows nothing changes).
    std::vector<float> sparse(kW * kH, 0.5f);
    for (UINT y = 48; y < 64; ++y)
        for (UINT x = 80; x < 96; ++x) sparse[y * kW + x] = 1.0f;
    Require(FieldFor(gpu, s, sparse, 0.125f).Finest().at(0, 0)[3] == 1.0f / 128.0f, "(2) one sky block is not 1/128 support");
    std::printf("(2) no sky: (0, 0) on every level; one sky block: support 1/128 (< 0.02)\n");
}

// Odd grids (4K's 240x135, and 15x9): the 1x1 level is the true share of sky over the finest cells and the true
// coverage-weighted mean, however the sky lies; the half-empty edge texels of odd sizes weigh by what they cover.
void TestOddPull(Gpu &gpu, const SkyShaders &s)
{
    struct Grid { UINT w, h; };
    for (const Grid g : {Grid{240, 135}, Grid{15, 9}}) {
        const auto pattern = [&](int kind, UINT x, UINT y) -> float {
            switch (kind) {
            case 0: return y == 0 ? 1.0f : 0.0f;              // one row of sky (the review's case: 1/135 at 240x135)
            case 1: return y + 1 == g.h ? 1.0f : 0.0f;         // the last row: the half-empty edge texels' row
            case 2: return x + 1 == g.w ? 1.0f : 0.0f;         // the last column
            default: return std::floor(Hash(x, y, 80) * 17.0f) / 16.0f; // a share of 16 lattice samples per cell
            }
        };
        for (int kind = 0; kind < 4; ++kind) {
            Image finest(g.w, g.h);
            double cover = 0.0, weighted[3] = {};
            for (UINT y = 0; y < g.h; ++y)
                for (UINT x = 0; x < g.w; ++x) {
                    const float a = pattern(kind, x, y);
                    const Texel t{Q(Hash(x, y, 81)), Q(Hash(x, y, 82)) - 0.5f, 0.25f, a};
                    finest.at(x, y) = t;
                    cover += a;
                    for (int c = 0; c < 3; ++c) weighted[c] += double(t[c]) * a;
                }
            const Field f = PullPush(gpu, s, finest, FieldRoot(1.0f));
            const Texel &top = f.pulled.back().at(0, 0);
            const double share = cover / double(g.w * g.h);
            Require(std::abs(top[3] - share) <= 1e-5 * std::max(share, 1e-3),
                    "(2) the 1x1 coverage of an odd grid is not the true share of sky");
            for (int c = 0; c < 3; ++c)
                Require(std::abs(top[c] - weighted[c] / cover) <= 1e-5, "(2) the 1x1 mean of an odd grid is not the coverage-weighted mean");
            if (kind == 0 && g.w == 240)
                std::printf("(2) odd grids: one sky row of 240x135 gives support %.5f = 1/135\n", top[3]);
        }
    }
}
} // namespace

void TestSkyMasterIdentity(Gpu &gpu, const SkyShaders &now, const SkyShaders &master); // test_temporal_sky_compose.cpp

int main(int argc, char **argv)
{
    try {
        Require(argc == 4, "usage: ofps_temporal_sky_shaders_tests <shader dir> <master fixture dir> <round 1 fixture dir>");
        Gpu gpu;
        const SkyShaders shaders(argv[1]), master(argv[1], argv[2]), round1(argv[1], argv[3]);
        TestReduction(gpu, shaders);
        TestPushPull(gpu, shaders);
        TestOddPull(gpu, shaders);
        TestSkyComposition(gpu, shaders);
        TestSkyMasterIdentity(gpu, shaders, master);
        TestSkyGate(gpu, shaders, master, round1);
        std::printf("PASS: temporal sky field passes on WARP\n");
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
