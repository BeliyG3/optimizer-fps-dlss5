// Fix round 2 of the sky field on D3D11 WARP (core/shaders/temporal_sky.hlsli, PwTSkyWeight): a carried frame weighs
// the field by the share of sky the full pass saw (the field's support) against its own (t21: CSSkyNow, CSSkyPull):
//   (10) CSSkyNow: per finest texel the exact share of its 4x4 lattice at the far plane of the CURRENT depth (t5),
//        through a guide at a sub-rect offset and half the native resolution, standard and reversed Z; NaN/Inf and
//        depths beside the far plane never count. Pulled to 1x1 it is the frame's share, and over the depth the field
//        was built from it equals the field's support bit for bit (a still frame takes the field whole);
//   (11a) the pass saw 13.5 %, the frame shows 80 % (bench12's reveal at frame 884): Compose, ComposeGrid and
//        Reproject (both u0 routes) write master's bytes without the field (tests/fixtures/temporal_pre_sky);
//   (11b) the pass saw 40 %, the frame shows 45 %: they write the bytes of the first sky field with the same field
//        (tests/fixtures/temporal_sky_round1);
//   (11c) 24 % against 50 % (weight 0.52): a far-plane pixel's addition is the blend of the ordinary carry's and the
//        field's, a far-plane pixel whose carried addition is NaN/Inf takes the field, every other pixel is the
//        ordinary carry bit for bit.
#include "temporal_sky_scenes.h"

#include <cstdio>

using namespace temporal_sky_scenes;

namespace {
constexpr UINT kGuideW = 160, kGuideH = 80;
const Texel kGuideRect{8, 4, 128, 64}; // half the native resolution, offset inside a larger texture

// PwTSkyWeight on the CPU.
float Weight(float model, float now) { return std::clamp((model / std::max(now, 1e-6f) - 0.35f) * 4.0f, 0.0f, 1.0f); }

// The guide texel a native pixel reads (PwTRectTexel), in the shader's own float steps.
UINT GuideAxis(float p, float native, float x, float w) { return UINT(std::clamp(x + p * (1.0f / native) * w, x, x + w - 1.0f)); }

// The current depth as a guide: the left quarter all sky, the next quarter none, the rest a mix of sky, values beside
// it, NaN, Inf and the other end of the range; the border outside the rect is sky (a wrong mapping would count it).
std::vector<float> Guide(float farPlane)
{
    std::vector<float> d(kGuideW * kGuideH, farPlane);
    for (UINT y = 0; y < UINT(kGuideRect[3]); ++y)
        for (UINT x = 0; x < UINT(kGuideRect[2]); ++x) {
            const float h = Hash(x, y, 90);
            float v = h < 0.45f ? farPlane : h < 0.55f ? 0.99999994f : h < 0.6f ? 1.0000001f : h < 0.65f ? kNaN : h < 0.7f ? kInf
                    : h < 0.75f ? 1.0f - farPlane : 0.5f;
            if (x < 32) v = farPlane;
            else if (x < 64) v = 0.5f;
            d[(y + UINT(kGuideRect[1])) * kGuideW + x + UINT(kGuideRect[0])] = v;
        }
    return d;
}

void TestNow(Gpu &gpu, const SkyShaders &s)
{
    for (const float farPlane : {1.0f, 0.0f}) {
        const std::vector<float> guide = Guide(farPlane);
        auto depth = DepthView(gpu, kGuideW, kGuideH, guide);
        const Root root = FieldRoot(farPlane, kGuideRect);
        Views v{};
        v[5] = depth.Get();
        const Image got = Run(gpu, s.now, v, root, {kLowW, kLowH}, Native(kLowW, kLowH))[0];
        double total = 0.0;
        for (UINT ty = 0; ty < kLowH; ++ty)
            for (UINT tx = 0; tx < kLowW; ++tx) {
                int n = 0;
                for (int k = 0; k < 16; ++k) {
                    const float px = std::min(std::floor(float(tx) * 16.0f + (float(k & 3) + 0.5f) * 4.0f), float(kW) - 1.0f);
                    const float py = std::min(std::floor(float(ty) * 16.0f + (float(k >> 2) + 0.5f) * 4.0f), float(kH) - 1.0f);
                    const float d = guide[GuideAxis(py + 0.5f, float(kH), kGuideRect[1], kGuideRect[3]) * kGuideW +
                                          GuideAxis(px + 0.5f, float(kW), kGuideRect[0], kGuideRect[2])];
                    n += std::isfinite(d) && d == farPlane ? 1 : 0;
                }
                const Texel &t = got.at(tx, ty);
                Require(t[3] == float(n) / 16.0f, "(10) CSSkyNow is not the exact share of the lattice at the far plane");
                Require(t[0] == 0.0f && t[1] == 0.0f && t[2] == 0.0f, "(10) CSSkyNow wrote something besides the share");
                total += float(n) / 16.0f;
            }
        const float share = float(total / double(kLowW * kLowH));
        const Field now = PullPush(gpu, s, got, root);
        Require(std::abs(now.pulled.back().at(0, 0)[3] - share) <= 1e-6f, "(10) the pulled 1x1 is not the frame's share of sky");
        // The same depth as the pass's (t6, a finite residual): the field's support and the measured share agree exactly.
        Image residual(kW, kH, {0.125f, -0.0625f, 0.25f, 1.0f});
        auto residualView = gpu.Srv(residual);
        const Field field = BuildField(gpu, s, residualView.Get(), depth.Get(), root);
        Require(field.Finest().at(0, 0)[3] == now.pulled.back().at(0, 0)[3],
                "(10) over the pass's own depth the measured share differs from the field's support");
        std::printf("(10) CSSkyNow, far plane %.0f: exact lattice shares, 1x1 share %.5f = the field's support over the same depth "
                    "(weight %.0f)\n", farPlane, share, Weight(share, share));
    }
}

// Rows 0..63 sky with a building at x 96..159; rows 64.. ground, with NaN/Inf in a few ground and sky pixels.
std::vector<float> GateDepth()
{
    std::vector<float> d(kW * kH);
    for (UINT y = 0; y < kH; ++y)
        for (UINT x = 0; x < kW; ++x) {
            float v = y < 64 ? (x >= 96 && x < 160 ? 0.4f : 1.0f) : 0.6f;
            if (Hash(x, y, 91) < 0.03f) v = Hash(x, y, 92) < 0.5f ? kNaN : kInf;
            d[y * kW + x] = v;
        }
    return d;
}
bool Sky(const std::vector<float> &depth, UINT x, UINT y) { return depth[y * kW + x] == 1.0f; }

Image GateField(float support)
{
    Image f(kLowW, kLowH);
    for (UINT y = 0; y < kLowH; ++y)
        for (UINT x = 0; x < kLowW; ++x) f.at(x, y) = {float(x) / 64.0f - 0.0625f, float(y) / 32.0f, -0.125f, support};
    return f;
}

struct Shares { float model, now; };
constexpr Shares kReveal{0.135f, 0.80f}, kTurn{0.40f, 0.45f}, kMid{0.24f, 0.50f};

bool Finite(const Texel &t) { return std::isfinite(t[0]) && std::isfinite(t[1]) && std::isfinite(t[2]); }

// A far-plane pixel at weight w, frames as they are: colour + the blend of the ordinary carry's addition and the
// field's; the field alone where the carried addition is not finite.
void RequireBlend(const Texel &got, const Texel &colour, const Texel &carried, const Texel &field, float w, const char *what)
{
    for (int c = 0; c < 3; ++c) {
        const float add = Finite(carried) ? carried[c] + w * (field[c] - carried[c]) : field[c];
        Require(std::isfinite(got[c]) && std::abs(got[c] - (colour[c] + add)) <= 2e-5f * (1.0f + std::abs(got[c])), what);
    }
}

void TestGateCompose(Gpu &gpu, const SkyShaders &s, const SkyShaders &master, const SkyShaders &round1)
{
    const Image color = Frame(false);
    const std::vector<float> depth = GateDepth();
    std::size_t blended = 0, rescued = 0;
    for (const bool grid : {false, true}) {
        const UINT gw = grid ? kW / 2 : 0, gh = grid ? kH / 2 : 0;
        const Image addition = Additions(!grid); // NaN, infinities and -0 among the native additions
        for (const auto &k : kComposeConfigs) {
            const Root root = WithSky(ComposeConstants(k), 1.0f);
            const bool cells = k[1] != 0;
            auto run = [&](const SkyShaders &pass, const Shares *shares, bool withNow) {
                FrameInputs in;
                in.Set(0, gpu.Srv(color)); in.Set(2, gpu.Srv(addition)); in.Set(5, DepthView(gpu, kW, kH, depth));
                if (shares) in.Set(kField, gpu.Srv(GateField(shares->model)));
                if (shares && withNow) in.Set(kNow, gpu.Srv(NowShare(shares->now)));
                return ComposeRun(gpu, grid ? pass.composeGrid : pass.compose, grid ? s.cellsGrid : s.cells, in, root, cells, gw, gh);
            };
            const Image off = run(master, nullptr, false);
            Require(SameBits(run(s, &kReveal, true), off), "(11a) a reveal (13.5 % against 80 %) is not the ordinary carry bit for bit");
            const Image fix = run(round1, &kTurn, false);
            Require(SameBits(run(s, &kTurn, true), fix), "(11b) 40 % against 45 % is not the first sky field bit for bit");
            Require(!SameBits(fix, off), "(11b) the scene cannot tell the field from the ordinary carry");
            const Image mid = run(s, &kMid, true);
            Require(!SameBits(mid, off) && !SameBits(mid, fix), "(11c) a mid ratio is neither blended nor told apart");
            if (k[3] != 0) continue; // the ratio domain: the blend is checked where frames are used as they are
            const Image field = GateField(kMid.model);
            for (UINT y = 0; y < kH; ++y)
                for (UINT x = 0; x < kW; ++x) {
                    if (!Sky(depth, x, y)) {
                        Require(SameBits(mid.at(x, y), off.at(x, y)), "(11c) a pixel off the far plane is not the ordinary carry");
                        continue;
                    }
                    // The ordinary carry's addition: what it added to the colour.
                    const Texel &o = off.at(x, y), &c = color.at(x, y);
                    const Texel carried{o[0] - c[0], o[1] - c[1], o[2] - c[2], 0.0f};
                    RequireBlend(mid.at(x, y), c, carried, SampleField(field, x, y), Weight(kMid.model, kMid.now),
                                 "(11c) a far-plane pixel is not colour + the blend of the carried addition and the field");
                    ++blended;
                    rescued += Finite(carried) ? 0 : 1;
                }
        }
    }
    std::printf("(11) Compose and ComposeGrid: 13.5 %% against 80 %% = master's bytes, 40 %% against 45 %% = round 1's bytes; "
                "24 %% against 50 %% (weight %.2f) blends %zu far-plane pixel reads (%zu with a NaN/Inf carry take the field)\n",
                Weight(kMid.model, kMid.now), blended, rescued);
    Require(rescued > 0, "(11c) the scene lacks a far-plane pixel with a NaN/Inf carried addition");
}

void TestGateDirect(Gpu &gpu, const SkyShaders &s, const SkyShaders &master, const SkyShaders &round1)
{
    ReprojectScene scene(gpu, false, 0.0f);
    std::vector<float> depth(kW * kH);
    for (UINT y = 0; y < kH; ++y)
        for (UINT x = 0; x < kW; ++x) depth[y * kW + x] = y < 30 ? 1.0f : (y < 90 && x >= 100 && x < 160 ? 0.3f : 0.8f);
    auto depthView = DepthView(gpu, kW, kH, depth);
    const Image field = GateField(kMid.model);
    std::size_t blended = 0;
    for (const auto &k : kReprojectConfigs)
        for (const std::uint32_t skip : {0u, 1u}) {
            const Root root = WithSky(ReprojectConstants(k), 1.0f);
            const Lanes lanes{kW, kH, 0, 0, 0, skip, 0, 0};
            const Texel sentinel{12345.0f, -6789.0f, 42.0f, 7.0f};
            std::vector<ComPtr<ID3D11ShaderResourceView>> keep;
            auto run = [&](const SkyShaders &pass, const Shares *shares, bool withNow) {
                Views v = scene.views;
                v[5] = depthView.Get();
                if (shares) { keep.push_back(gpu.Srv(GateField(shares->model))); v[kField] = keep.back().Get(); }
                if (shares && withNow) { keep.push_back(gpu.Srv(NowShare(shares->now))); v[kNow] = keep.back().Get(); }
                return Run(gpu, pass.reproject, v, root, lanes, Native(kW, kH, sentinel));
            };
            const auto off = run(master, nullptr, false), reveal = run(s, &kReveal, true);
            Require(SameBits(reveal[0], off[0]) && SameBits(reveal[1], off[1]), "(11a) Reproject's reveal is not the ordinary carry bit for bit");
            const auto fix = run(round1, &kTurn, false), turn = run(s, &kTurn, true);
            Require(SameBits(turn[0], fix[0]) && SameBits(turn[1], fix[1]), "(11b) Reproject at 40 % against 45 % is not round 1's bytes");
            const auto mid = run(s, &kMid, true);
            Require(SameBits(mid[1], off[1]), "(11c) the direct route's addition and acceptance (u1) changed with the field");
            if (skip != 0 || k[7] != 0) continue;
            Require(!SameBits(fix[0], off[0]), "(11b) the scene cannot tell the field from the ordinary carry");
            for (UINT y = 0; y < kH; ++y)
                for (UINT x = 0; x < kW; ++x) {
                    if (y >= 30) { Require(SameBits(mid[0].at(x, y), off[0].at(x, y)), "(11c) the direct route changed a pixel off the far plane"); continue; }
                    // The ordinary carry's addition is u1; the colour, ReprojectScene's background on the sky rows.
                    const Texel c{Q(0.2f + 0.3f * x / kW), Q(0.3f + 0.2f * Hash(x, y, 1)), 0.4f, 1.0f};
                    RequireBlend(mid[0].at(x, y), c, mid[1].at(x, y), SampleField(field, x, y), Weight(kMid.model, kMid.now),
                                 "(11c) the direct route's far-plane pixel is not colour + the blend");
                    ++blended;
                }
        }
    std::printf("(11) Reproject (both u0 routes): reveal = master's bytes, turn = round 1's bytes, u1 untouched; the mid ratio "
                "blends %zu far-plane pixels\n", blended);
}
} // namespace

void TestSkyGate(Gpu &gpu, const SkyShaders &s, const SkyShaders &master, const SkyShaders &round1)
{
    Require(Weight(kReveal.model, kReveal.now) == 0.0f && Weight(kTurn.model, kTurn.now) == 1.0f &&
                Weight(kMid.model, kMid.now) > 0.4f && Weight(kMid.model, kMid.now) < 0.6f,
            "(11) the shares do not give the weights 0, 1 and a blend");
    TestNow(gpu, s);
    TestGateCompose(gpu, s, master, round1);
    TestGateDirect(gpu, s, master, round1);
}
