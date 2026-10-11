// Why a pixel gets the model's "no history" marker (PwTMarkerReason, core/shaders/temporal_residual.hlsli) on D3D11 WARP:
// CSMarkerReason writes 1 off screen, 2 depth test, 3 colour gate, 4 non-finite and 0 where the chain is kept, and
// CSModelMotion hands the model the "none" vector (4*width, 0) exactly where the reason is not 0 and the chain itself
// everywhere else.
// - The Task 6 scene (16x8, DebugTemporalMarkerRule 0): each reason where its test fails, in today's order.
// - The rule scene (48x16): DebugTemporalMarkerRule 0..4 (PwTParams.z) against a CPU model of the tests, bit for bit -
//   1 the colour gate never marks, 2 it marks only where the depth mismatch is at least tol, 3 / 4 = 1 / 2 marked only
//   where at least 12 of the 25 texels of the 5x5 neighbourhood (clamped to the motion sub-rect) fail too; rule 0 is
//   today's verdict (Task 6) on every case.
// - The threshold scene (48x16): DebugTemporalMarkerNeighbours (PwTParams.y, 0 = 12) for rules 3 and 4 - an isolated pair,
//   a 1-texel line and a 2-texel band, each 6 long where long; rule 0 ignores the threshold.
// - Two-pass V3: every run goes three ways - MarkerFail then the pass reading its mask at t1 (PwTParams.x = 1, rules 3/4
//   only, as the machine records it), the same passes without MarkerFail (every test inline) and the passes as they were
//   at 672eb87 (fixtures/temporal_marker_single_pass) - and all three must write the same bytes on every pixel, also
//   on a random 64x32 scene (fixed seed) for every rule and threshold. MarkerFail's mask is the base test's verdict.
// - A motion sub-rect smaller than the texture (dynamic resolution): the 5x5 clamps to the rect, so a failing texel on
//   its inner border is not pushed over the threshold by the texels past it (672eb87 clamped to the texture: marked).
// Usage: ofps_temporal_marker_reason_tests <shader dir> <single-pass fixture dir>
#include "temporal_cs_harness.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <random>

using namespace temporal_cs;

namespace {
constexpr float kDepthTolerance = 0.05f, kColourTolerance = 0.08f, kStep = 0.25f; // the chain: in frame for every pixel
const Texel kGrey{0.4f, 0.4f, 0.4f, 1.0f}, kHue{0.6f, 0.35f, 0.25f, 1.0f};      // another hue at the same luma range

// Depth now is 0.5 everywhere; the residual frame's depth, colour and the chain differ where a case puts them.
struct Scene {
    UINT w, h, rw, rh; // texture; the host's motion sub-rect at (0, 0), also the native size and the other rects
    std::vector<float> chain, depthF;
    Image colourF;
    bool modelled = true; // the CPU model (Base, Reason) predicts it; a random scene has float ties it may get wrong
    Scene(UINT width, UINT height)
        : w(width), h(height), rw(width), rh(height), chain(std::size_t(width) * height * 2, kStep), depthF(std::size_t(width) * height, 0.5f),
          colourF(width, height, kGrey)
    {
    }
    void Depth(UINT x0, UINT y0, UINT x1, UINT y1, float d)
    {
        for (UINT y = y0; y <= y1; ++y)
            for (UINT x = x0; x <= x1; ++x) depthF[y * w + x] = d;
    }
    void Hue(UINT x0, UINT y0, UINT x1, UINT y1)
    {
        for (UINT y = y0; y <= y1; ++y)
            for (UINT x = x0; x <= x1; ++x) colourF.at(x, y) = kHue;
    }
    void Chain(UINT x, UINT y, float cx) { chain[(y * w + x) * 2] = cx; }
};

// PwTDepthMismatch (temporal_mapping.hlsli) for depths in 0..1.
float Mismatch(float a, float b)
{
    const float d = std::fabs(a - b);
    return std::max(d / std::max(std::max(a, b), 1e-6f), d / std::max(std::max(1.0f - a, 1.0f - b), 1e-6f));
}

// The base test with the variant's colour rule (0 today, 1 never, 2 only where the depth mismatch is at least tol).
// Native = motion texture here, so the chain's end is the texel centre plus the chain; PwTDepthMismatchAround takes the
// best of the four depth texels around it (clamped), the colour gate reads the texel the end lies in (a hue: it fails).
UINT Base(const Scene &s, UINT x, UINT y, int colour)
{
    const float cx = s.chain[(y * s.w + x) * 2], cy = s.chain[(y * s.w + x) * 2 + 1];
    if (!std::isfinite(cx) || !std::isfinite(cy)) return 4;
    const float qx = float(x) + 0.5f + cx, qy = float(y) + 0.5f + cy;
    if (qx < 0.0f || qy < 0.0f || qx >= float(s.rw) || qy >= float(s.rh)) return 1;
    const int bx = int(std::floor(qx - 0.5f)), by = int(std::floor(qy - 0.5f));
    float best = 1e9f;
    for (int k = 0; k < 4; ++k) {
        const int tx = std::clamp(bx + (k & 1), 0, int(s.rw) - 1), ty = std::clamp(by + (k >> 1), 0, int(s.rh) - 1);
        best = std::min(best, Mismatch(0.5f, s.depthF[std::size_t(ty) * s.w + tx]));
    }
    if (best > 2.0f * kDepthTolerance) return 2;
    const bool hue = SameBits(s.colourF.at(UINT(qx), UINT(qy)), kHue);
    if (hue && (colour == 0 || (colour == 2 && best >= kDepthTolerance))) return 3;
    return 0;
}

UINT Reason(const Scene &s, UINT x, UINT y, int rule, int need)
{
    const int colour = rule > 2 ? rule - 2 : rule;
    const UINT reason = Base(s, x, y, colour);
    if (rule < 3 || reason == 0 || reason == 1 || reason == 4) return reason;
    int failing = 0;
    for (int dy = -2; dy <= 2; ++dy)
        for (int dx = -2; dx <= 2; ++dx)
            failing += Base(s, UINT(std::clamp(int(x) + dx, 0, int(s.rw) - 1)), UINT(std::clamp(int(y) + dy, 0, int(s.rh) - 1)), colour) != 0;
    return failing >= (need > 0 ? need : 12) ? reason : 0;
}

struct Output {
    Image reason, model;
};

// The current passes and the single-pass ones of 672eb87.
struct Passes {
    std::vector<char> reason, model, fail, oldReason, oldModel;
};

enum class Route { TwoPass, Inline, Old };

Output RunRoute(Gpu &gpu, const Passes &p, const Scene &s, int rule, int need, Route route)
{
    Image colour(s.w, s.h, kGrey), expect(s.w, s.h, {std::nanf(""), 1.0f, std::nanf(""), 1.0f});
    std::vector<float> depth(std::size_t(s.w) * s.h, 0.5f);
    Views v{};
    auto c0 = gpu.Srv(colour), c7 = gpu.Srv(s.colourF), c11 = gpu.Srv(expect);
    auto c4 = gpu.Srv(s.w, s.h, DXGI_FORMAT_R32G32_FLOAT, s.chain.data(), 2 * sizeof(float));
    auto c5 = gpu.Srv(s.w, s.h, DXGI_FORMAT_R32_FLOAT, depth.data(), sizeof(float));
    auto c6 = gpu.Srv(s.w, s.h, DXGI_FORMAT_R32_FLOAT, s.depthF.data(), sizeof(float));
    v[0] = c0.Get(); v[4] = c4.Get(); v[5] = c5.Get(); v[6] = c6.Get(); v[7] = c7.Get(); v[11] = c11.Get();
    const float w = float(s.w), h = float(s.h), rw = float(s.rw), rh = float(s.rh);
    const bool mask = route == Route::TwoPass && rule >= 3; // Resources::RecordMarkerFail
    const Texel rows[7] = {{rw, rh, 1.0f / rw, 1.0f / rh}, {0, 0, rw, rh}, {0, 0, rw, rh}, {0, 0, rw, rh}, {w, h, 1, 1},
                           {mask ? 1.0f : 0.0f, float(need), float(rule), kDepthTolerance}, {kColourTolerance, 1, 0, 0}};
    Root root{};
    for (int i = 0; i < 7; ++i) std::memcpy(&root[i * 4], rows[i].data(), sizeof(Texel));
    const Lanes lanes{s.w, s.h, 0, 0, 0, 0, 0, 0};
    ComPtr<ID3D11ShaderResourceView> c1;
    if (mask) {
        std::array<Image, 2> fail{Image(s.w, s.h, {255, 0, 0, 0}), Image(s.w, s.h)};
        gpu.Run(p.fail, v, root, lanes, s.w, s.h, fail, DXGI_FORMAT_R8_UINT);
        std::vector<std::uint8_t> bytes(fail[0].texels.size());
        for (UINT y = 0; y < s.h; ++y)
            for (UINT x = 0; x < s.w; ++x) {
                bytes[y * s.w + x] = std::uint8_t(fail[0].at(x, y)[0]);
                if (s.modelled && UINT(bytes[y * s.w + x]) != Base(s, x, y, rule - 2)) {
                    std::fprintf(stderr, "rule %d, pixel (%u, %u): MarkerFail wrote %u\n", rule, x, y, UINT(bytes[y * s.w + x]));
                    Require(false, "MarkerFail differs from the base test");
                }
            }
        c1 = gpu.Srv(s.w, s.h, DXGI_FORMAT_R8_UINT, bytes.data(), 1);
        v[1] = c1.Get();
    }
    const bool old = route == Route::Old;
    std::array<Image, 2> reason{Image(s.w, s.h, {255, 0, 0, 0}), Image(s.w, s.h)};
    gpu.Run(old ? p.oldReason : p.reason, v, root, lanes, s.w, s.h, reason, DXGI_FORMAT_R8_UINT);
    std::array<Image, 2> model{Image(s.w, s.h), Image(s.w, s.h)};
    gpu.Run(old ? p.oldModel : p.model, v, root, lanes, s.w, s.h, model);
    return {reason[0], model[0]};
}

// The two-pass route, after the inline and 672eb87's single-pass routes were found to write the same bytes everywhere
// (the inline one alone when `old` is false: 672eb87 clamped the 5x5 to the texture, a sub-rect differs on purpose).
Output Run(Gpu &gpu, const Passes &p, const Scene &s, int rule, int need = 0, bool old = true)
{
    const Output two = RunRoute(gpu, p, s, rule, need, Route::TwoPass);
    for (Route route : {Route::Inline, Route::Old}) {
        if (route == Route::Old && !old) continue;
        const Output other = RunRoute(gpu, p, s, rule, need, route);
        for (UINT y = 0; y < s.h; ++y)
            for (UINT x = 0; x < s.w; ++x)
                if (!SameBits(two.reason.at(x, y), other.reason.at(x, y)) || !SameBits(two.model.at(x, y), other.model.at(x, y))) {
                    std::fprintf(stderr, "rule %d, threshold %d, pixel (%u, %u): the two-pass route differs from the %s route\n", rule, need, x, y, route == Route::Old ? "672eb87" : "inline");
                    Require(false, "the two-pass marker differs from the single-pass one");
                }
    }
    return two;
}

// Both planes against the CPU model, bit for bit: the reason, and the chain or the "none" vector. Counts per reason.
void Compare(const Scene &s, const Output &out, int rule, int (&count)[5], int need = 0)
{
    for (UINT y = 0; y < s.h; ++y)
        for (UINT x = 0; x < s.w; ++x) {
            const UINT got = UINT(out.reason.at(x, y)[0]), want = Reason(s, x, y, rule, need);
            if (got != want) {
                std::fprintf(stderr, "rule %d, pixel (%u, %u): reason %u, expected %u\n", rule, x, y, got, want);
                Require(false, "the marker reason differs from the tests' verdict");
            }
            ++count[want];
            const UINT i = y * s.w + x;
            const Texel none{4.0f * float(s.w), 0, 0, 0}, kept{s.chain[i * 2], s.chain[i * 2 + 1], 0, 0};
            if (!SameBits(out.model.at(x, y), want ? none : kept)) {
                std::fprintf(stderr, "rule %d, pixel (%u, %u): ModelMotion disagrees with reason %u\n", rule, x, y, want);
                Require(false, "ModelMotion and the marker reason disagree");
            }
        }
}

// Task 6: in-frame chain, one column off screen, one NaN, a 30 % deeper block (x 8..12, y 1..4), a hue (x 1..3, y 4..7).
void TaskSixScene(Gpu &gpu, const Passes &passes)
{
    Scene s(16, 8);
    s.Depth(8, 1, 12, 4, 0.65f);
    s.Hue(1, 4, 3, 7);
    for (UINT y = 0; y < s.h; ++y) s.Chain(5, y, -8.0f);
    s.Chain(10, 6, std::numeric_limits<float>::quiet_NaN());
    int count[5] = {};
    Compare(s, Run(gpu, passes, s, 0), 0, count);
    std::printf("reasons: %d keep, %d off screen, %d depth, %d colour, %d non-finite\n", count[0], count[1], count[2], count[3],
                count[4]);
    Require(count[1] == 8 && count[2] == 12 && count[3] == 12 && count[4] == 1, "the scene lacks a reason");
}

// The rule cases on 48x16, each with a pixel whose verdict per rule 0..4 is fixed:
//   colour only (x 12..19, y 4..11, equal depth)            1 0 0 0 0
//   colour + 1.2 tol deeper (x 24..31, y 4..11)             1 0 1 0 1
//   isolated depth failure (4, 13: a 2x2 deeper patch)      1 1 1 0 0
//   8x8 depth block (x 36..43, y 4..11)                     1 1 1 1 1
//   texture corner (0, 0) in an 8x8 failing corner block    1 1 1 1 1 (the 5x5 clamps to the rect = texture)
//   isolated off screen (10, 14) and non-finite (46, 14)    1 1 1 1 1
void RuleScene(Gpu &gpu, const Passes &passes)
{
    Scene s(48, 16);
    s.Depth(0, 0, 8, 8, 0.65f);
    s.Hue(12, 4, 19, 11);
    s.Hue(24, 4, 31, 11);
    s.Depth(24, 4, 31, 11, 0.53f); // mismatch 0.06: above tol, within 2 tol
    s.Depth(4, 13, 5, 14, 0.65f);
    s.Depth(36, 4, 44, 12, 0.65f);
    s.Chain(10, 14, -60.0f);
    s.Chain(46, 14, std::numeric_limits<float>::quiet_NaN());
    struct Case { UINT x, y; bool marked[5]; const char *name; };
    const Case cases[] = {{15, 7, {true, false, false, false, false}, "colour only"},
                          {27, 7, {true, false, true, false, true}, "colour with 1.2 tol depth"},
                          {4, 13, {true, true, true, false, false}, "isolated failure"},
                          {39, 7, {true, true, true, true, true}, "8x8 block"},
                          {0, 0, {true, true, true, true, true}, "corner of a corner block"},
                          {10, 14, {true, true, true, true, true}, "off screen"},
                          {46, 14, {true, true, true, true, true}, "non-finite"}};
    for (int rule = 0; rule <= 4; ++rule) {
        const Output out = Run(gpu, passes, s, rule);
        for (const Case &c : cases)
            if ((out.reason.at(c.x, c.y)[0] != 0.0f) != c.marked[rule]) {
                std::fprintf(stderr, "rule %d, %s (%u, %u): %s\n", rule, c.name, c.x, c.y, c.marked[rule] ? "kept" : "marked");
                Require(false, "a rule case has the wrong verdict");
            }
        int count[5] = {};
        Compare(s, out, rule, count);
        std::printf("rule %d: %d keep, %d off screen, %d depth, %d colour, %d non-finite\n", rule, count[0], count[1], count[2],
                    count[3], count[4]);
    }
}

// DebugTemporalMarkerNeighbours on 48x16. A deeper 2x2 depthF patch makes exactly one texel fail (the depth test takes the
// best of the four texels around the chain's end), so a (n+1)x(m+1) patch fails an n x m block. A 1-texel line has at
// most 5 failing texels in any 5x5 window, a 2-texel band 10, a pair 2. Verdicts at thresholds 0 (= 12), 12, 8, 6, 5, 2:
//   pair (4..5, 4)                 kept kept kept kept kept marked
//   1-texel line (14..19, 4)       kept kept kept kept marked marked
//   2-texel band (26..31, 4..5)    kept kept marked marked marked marked
void ThresholdScene(Gpu &gpu, const Passes &passes)
{
    Scene s(48, 16);
    s.Depth(4, 4, 6, 5, 0.65f);
    s.Depth(14, 4, 20, 5, 0.65f);
    s.Depth(26, 4, 32, 6, 0.65f);
    const int needs[] = {0, 12, 8, 6, 5, 2};
    struct Case { UINT x, y; bool marked[6]; const char *name; };
    const Case cases[] = {{4, 4, {false, false, false, false, false, true}, "pair"},
                          {16, 4, {false, false, false, false, true, true}, "1-texel line"},
                          {28, 4, {false, false, true, true, true, true}, "2-texel band"}};
    for (int rule : {0, 3, 4})
        for (int k = 0; k < 6; ++k) {
            const Output out = Run(gpu, passes, s, rule, needs[k]);
            for (const Case &c : cases)
                if ((out.reason.at(c.x, c.y)[0] != 0.0f) != (rule == 0 || c.marked[k])) {
                    std::fprintf(stderr, "rule %d, threshold %d, %s (%u, %u): wrong verdict\n", rule, needs[k], c.name, c.x, c.y);
                    Require(false, "a threshold case has the wrong verdict");
                }
            int count[5] = {};
            Compare(s, out, rule, count, needs[k]);
            std::printf("rule %d, threshold %d: %d keep, %d depth\n", rule, needs[k], count[0], count[2]);
        }
}

// A random 64x32 scene (fixed seed): deeper and shallower patches of 1..6 texels, hue patches, a chain of up to 0.75 texel
// either way with a few chains off screen or non-finite. Only the three routes' agreement is required (float ties near
// the tolerances are the GPU's to break); the counts show every reason and V3 sparing failing texels.
void RandomScene(Gpu &gpu, const Passes &passes)
{
    Scene s(64, 32);
    s.modelled = false;
    std::mt19937 random(20261009u);
    std::uniform_int_distribution<UINT> px(0, s.w - 1), py(0, s.h - 1), size(1, 6);
    std::uniform_real_distribution<float> deep(0.42f, 0.7f), step(-0.75f, 0.75f), unit(0.0f, 1.0f);
    for (int k = 0; k < 40; ++k) {
        const UINT x = px(random), y = py(random);
        s.Depth(x, y, std::min(x + size(random), s.w - 1), std::min(y + size(random), s.h - 1), deep(random));
    }
    for (int k = 0; k < 16; ++k) {
        const UINT x = px(random), y = py(random);
        s.Hue(x, y, std::min(x + size(random), s.w - 1), std::min(y + size(random), s.h - 1));
    }
    for (float &v : s.chain) v = step(random);
    for (std::size_t i = 0; i < s.chain.size(); i += 2) {
        const float u = unit(random);
        if (u < 0.01f) s.chain[i] = -100.0f;
        else if (u < 0.015f) s.chain[i + 1] = std::numeric_limits<float>::quiet_NaN();
    }
    for (int rule = 0; rule <= 4; ++rule)
        for (int need : {0, 12, 8, 6, 5, 2, 25}) {
            if (rule < 3 && need != 0) continue; // the threshold is read by rules 3/4 alone
            const Output out = Run(gpu, passes, s, rule, need);
            int count[5] = {};
            for (const Texel &t : out.reason.texels) ++count[UINT(t[0])];
            std::printf("random, rule %d, threshold %d: %d keep, %d off screen, %d depth, %d colour, %d non-finite\n", rule, need,
                        count[0], count[1], count[2], count[3], count[4]);
        }
}
// Dynamic resolution: a 40x12 motion sub-rect in a 48x16 texture. A deeper 2x2 patch at (39..40, 6..7) fails the rect's
// border texel (39, 6) alone. Its 5x5 clamped to the rect holds 3 failing texels (itself, repeated), below 8: kept under
// rules 3/4 at threshold 8; clamped to the texture (672eb87) the ten texels past the rect test as off screen: 11, marked.
void SubRectScene(Gpu &gpu, const Passes &passes)
{
    Scene s(48, 16);
    s.rw = 40;
    s.rh = 12;
    s.Depth(39, 6, 40, 7, 0.65f);
    for (int rule : {0, 3, 4}) {
        const Output out = Run(gpu, passes, s, rule, 8, false);
        if ((out.reason.at(39, 6)[0] != 0.0f) != (rule == 0))
            Require(false, "a sub-rect border texel has the wrong verdict (the 5x5 must clamp to the motion rect)");
        if (rule > 0 && RunRoute(gpu, passes, s, rule, 8, Route::Old).reason.at(39, 6)[0] == 0.0f)
            Require(false, "the sub-rect case does not tell the rect clamp from the texture clamp");
        int count[5] = {};
        Compare(s, out, rule, count, 8);
        std::printf("sub-rect, rule %d, threshold 8: %d keep, %d off screen, %d depth\n", rule, count[0], count[1], count[2]);
    }
}
} // namespace

int main(int argc, char **argv)
{
    try {
        Require(argc == 3, "usage: ofps_temporal_marker_reason_tests <shader dir> <single-pass fixture dir>");
        const std::filesystem::path dir = argv[1], fixture = argv[2];
        const Passes passes{ReadFile(dir / "temporal_MarkerReason_cs.dxbc"), ReadFile(dir / "temporal_ModelMotion_cs.dxbc"),
                            ReadFile(dir / "temporal_MarkerFail_cs.dxbc"), ReadFile(fixture / "temporal_MarkerReason_cs.dxbc"),
                            ReadFile(fixture / "temporal_ModelMotion_cs.dxbc")};
        Gpu gpu;
        TaskSixScene(gpu, passes);
        RuleScene(gpu, passes);
        ThresholdScene(gpu, passes);
        RandomScene(gpu, passes);
        SubRectScene(gpu, passes);
        std::printf("PASS: marker reasons, the rule variants and ModelMotion agree on WARP; two-pass V3 = single-pass V3\n");
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
