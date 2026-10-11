// DebugTemporalCarryChainRule 5/6 and the merged chain passes (phase 3 step 1c: core/temporal/carry_chain.cpp,
// core/shaders/temporal_chain_pair.hlsli). D3D12 WARP with the debug layer, 64x36 motion texels over a 256x128 frame:
//   - the merge: with Refine on, over three frames of a textured scene with colour and depth changes (a 1 px shift
//     everywhere, so Refine corrects; the expectation off, which on a still camera would hide the depth changes - the live
//     runs of test_temporal_chain_rule.cpp have it on), every main rule 0..4 with every carry rule 1..4 gives the main chain
//     byte for byte the single chain of its rule and the carry chain the single chain of the carry rule - what the
//     separate passes of e857106 / 3908823 gave each chain;
//   - rule 5 (Refine off, the chain (2, 0) a frame, two frames): a speckled colour change (one texel in four, swaying
//     blades) keeps the carry chain, a uniform 12x12 change at equal depth (a reflection) restarts it where at least 18
//     of the 25 texels around change too, an 8x8 depth change restarts it; the main chain restarts by its own rule (0, 3);
//   - rule 6 (colour test off, Refine on, no game motion, two frames): a uniform 2 px shift of the picture restarts the
//     carry chain inside the area; random shifts per 4x4 block keep it (at least 95 % of the texels: where a few random
//     neighbours happen to agree, rule 6 restarts too, and the test prints how often); rule 5 restarts nothing there;
//   - a motion sub-rect smaller than the texture and offset in it (rules 3 and 5, Refine off, the chain (2, 0) a frame, two
//     frames): AccumulatePair's rule-3 chain (main and carry) is byte for byte LinkFail + Accumulate's, and both rules
//     match the chain expected with the 5x5 votes clamped to the sub-rect, on a scene where that clamp decides the
//     verdict at the sub-rect's corners (a clamp to the texture would keep those chains).
// Usage: ofps_temporal_carry_rules_tests <shader dir>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "temporal_chain_fixture.h"
#include "core/temporal/machine.h"

#include <dxgi1_4.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <vector>

using namespace ofps::core::temporal;
using namespace temporal_d3d12;

namespace {
int failures = 0;

void Check(bool condition, const char *message)
{
    if (condition) return;
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
}

// A smooth texture with gradients both ways that Refine registers on everywhere, 0.1..0.9.
float Pattern(float x, float y) { return 0.5f + 0.2f * std::sin(0.5f * x + 0.5f) + 0.2f * std::sin(0.45f * y + 1.0f); }

bool In(int x, int y, const D3D12_RECT &r) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }

// The native pixels the tests of motion texels [x0, x1] x [y0, y1] read in the residual's frame after two frames of the
// chain (2, 0) (test_temporal_chain_rule.cpp's Ends).
D3D12_RECT Ends(int x0, int y0, int x1, int y1)
{
    const auto row = [](int y, float offset) {
        return static_cast<LONG>(std::floor((static_cast<float>(y) + 0.5f) * static_cast<float>(kH) / kMh + offset));
    };
    return {4 * x0 + 16, row(y0, -0.5f), 4 * x1 + 20, row(y1, 0.5f) + 1};
}

struct Scene {
    ComPtr<ID3D12Resource> colourThen, depthThen, colourNow, depthNow, fresh, motion;
    bool Ready() const { return colourThen && depthThen && colourNow && depthNow && fresh && motion; }
};

// The motion sub-rect of the sub-rect runs: texels [8, 40) x [6, 22) of the 64x36 texture, 8 native pixels a texel.
constexpr Rect kSubRect{8, 6, 32, 16};

struct Config {
    int rule, carry;
    bool refine, expect;
    float tolerance;
    int frames;
    bool subRect = false; // the host's motion covers kSubRect only
};

// A full pass on the scene's residual frame, then `frames` accumulations on its current one; Acc() and CarryAcc() into
// readback slots 2 * slot and 2 * slot + 1.
bool Record(ID3D12Device *device, Recording &r, Machine &m, const ofps::core::gpu::Shaders &shaders, const Scene &s,
            const Config &config, ID3D12Resource *readback, std::uint32_t slot)
{
    char error[256]{};
    if (!m.Initialize(device, shaders, kW, kH, kColor, kColor, kMw, kMh, DXGI_FORMAT_R32_FLOAT, kW, kH, error, sizeof(error))) {
        std::cerr << error << '\n';
        return false;
    }
    m.SetUsePoint(r.Point(), 0);
    FrameInputs in{};
    in.color = s.colourThen.Get(); in.depth = s.depthThen.Get(); in.motion = s.motion.Get();
    in.colorView = kColor; in.depthView = DXGI_FORMAT_R32_FLOAT; in.motionView = DXGI_FORMAT_R16G16_FLOAT;
    in.colorRect = in.depthRect = {0, 0, kW, kH};
    in.motionRect = config.subRect ? kSubRect : Rect{0, 0, kMw, kMh};
    in.hostInputState = in.depthState = in.motionState = kRead;
    in.refine = config.refine;
    in.expectedDepth = config.expect;
    in.colorTolerance = config.tolerance;
    in.depthThreshold = 0.1f;
    in.chainRule = config.rule;
    in.carryChainRule = config.carry;
    m.RecordResidual(r.list.Get(), in, s.fresh.Get(), kRead);
    in.color = s.colourNow.Get(); in.depth = s.depthNow.Get();
    for (int f = 0; f < config.frames; ++f) m.RecordAccumulate(r.list.Get(), in);
    ReadAcc(r.list.Get(), m.Acc(), readback, 2 * slot);
    ReadAcc(r.list.Get(), m.CarryAcc(), readback, 2 * slot + 1);
    return !m.TakeExhausted() && m.CarryChainCreated() == (config.carry != 0);
}

const unsigned char *Plane(const unsigned char *bytes, std::uint32_t slot) { return bytes + static_cast<std::size_t>(slot) * kSlot; }

// Texel (x, y)'s two halves in `slot` as one value, for byte comparisons.
std::uint32_t Bits(const unsigned char *bytes, std::uint32_t slot, int x, int y)
{
    std::uint32_t v;
    std::memcpy(&v, Plane(bytes, slot) + (static_cast<std::size_t>(y) * kMw + x) * 4, 4);
    return v;
}

// --- the scenes --------------------------------------------------------------------------------------------------------

// The merge: no game motion (the chain is Refine's), a 1 px shift everywhere, and in the residual's frame a band at 0.5 w
// that changed colour, an 8x8 block at 3 w, a block and a spot that changed colour at 1.5 w (colour tolerance 0.1).
Scene MergeScene(ID3D12Device *device, Recording &r, std::vector<ComPtr<ID3D12Resource>> &keep)
{
    const D3D12_RECT band{16, 0, 80, 128}, deep{144, 8, 176, 40}, block{144, 72, 176, 104}, spot{200, 60, 204, 64};
    Scene s;
    s.colourThen = Uploaded(device, r, kColor, [&](int x, int y) {
        const bool changed = In(x, y, band) || In(x, y, block) || In(x, y, spot);
        return Pattern(static_cast<float>(x), static_cast<float>(y)) * (changed ? 2.5f : 1.0f);
    }, keep);
    s.depthThen = Uploaded(device, r, DXGI_FORMAT_R32_FLOAT, [&](int x, int y) {
        return In(x, y, band) ? 0.475f : In(x, y, deep) ? 0.35f : (In(x, y, block) || In(x, y, spot)) ? 0.425f : 0.5f;
    }, keep);
    s.colourNow = Uploaded(device, r, kColor, [](int x, int y) { return Pattern(static_cast<float>(x - 1), static_cast<float>(y)); }, keep);
    s.depthNow = Uploaded(device, r, DXGI_FORMAT_R32_FLOAT, [](int, int) { return 0.5f; }, keep);
    s.fresh = Uploaded(device, r, kColor, [](int, int) { return 0.75f; }, keep);
    s.motion = Motion(device, r, 0.0f, 0.0f);
    return s;
}

// Rule 5: the chain (2, 0) a frame; in the residual's frame, at the chains' ends after two frames, speckles one texel in
// four 25 % brighter (x 4..26, y 4..30, both even), a uniform 12x12 block U 25 % brighter (x 36..47, y 4..15), both at
// equal depth, and an 8x8 block D at 3 w (x 36..43, y 22..29). Colour tolerance 0.04.
bool Speckled(int x, int y) { return x >= 4 && x <= 26 && y >= 4 && y <= 30 && x % 2 == 0 && y % 2 == 0; }
bool Uniform(int x, int y) { return x >= 36 && x <= 47 && y >= 4 && y <= 15; }
bool Deep(int x, int y) { return x >= 36 && x <= 43 && y >= 22 && y <= 29; }

Scene CoherentColourScene(ID3D12Device *device, Recording &r, std::vector<ComPtr<ID3D12Resource>> &keep)
{
    std::vector<D3D12_RECT> bright{Ends(36, 4, 47, 15)};
    for (int y = 0; y < static_cast<int>(kMh); ++y)
        for (int x = 0; x < static_cast<int>(kMw); ++x)
            if (Speckled(x, y)) bright.push_back(Ends(x, y, x, y));
    const D3D12_RECT deep = Ends(36, 22, 43, 29);
    Scene s;
    s.colourThen = Uploaded(device, r, kColor, [&](int x, int y) {
        for (const D3D12_RECT &b : bright)
            if (In(x, y, b)) return 0.625f;
        return 0.5f;
    }, keep);
    s.depthThen = Uploaded(device, r, DXGI_FORMAT_R32_FLOAT, [&](int x, int y) { return In(x, y, deep) ? 0.35f : 0.5f; }, keep);
    s.colourNow = Uploaded(device, r, kColor, [](int, int) { return 0.5f; }, keep);
    s.depthNow = Uploaded(device, r, DXGI_FORMAT_R32_FLOAT, [](int, int) { return 0.5f; }, keep);
    s.fresh = Uploaded(device, r, kColor, [](int, int) { return 0.75f; }, keep);
    s.motion = Motion(device, r, 2.0f, 0.0f);
    return s;
}

// Rule 6: no game motion; the current picture is the residual frame's shifted by 2 px to the right over the native area
// [32, 96) x [22, 106) (motion texels x 8..23, y 6..29), and by 2 px in a random direction (right, left, down or up)
// per 4x4 block over [128, 224) x [22, 106) (x 32..55).
Scene CoherentMotionScene(ID3D12Device *device, Recording &r, std::vector<ComPtr<ID3D12Resource>> &keep)
{
    const D3D12_RECT uniform{32, 22, 96, 106}, mixed{128, 22, 224, 106};
    Scene s;
    s.colourThen = Uploaded(device, r, kColor, [](int x, int y) { return Pattern(static_cast<float>(x), static_cast<float>(y)); }, keep);
    s.colourNow = Uploaded(device, r, kColor, [&](int x, int y) {
        float sx = 0.0f, sy = 0.0f;
        if (In(x, y, uniform)) sx = 2.0f;
        if (In(x, y, mixed)) {
            const unsigned h = static_cast<unsigned>(x / 4) * 73856093u ^ static_cast<unsigned>(y / 4) * 19349663u;
            const int type = static_cast<int>((h ^ (h >> 13)) * 2654435761u >> 30);
            sx = type == 0 ? 2.0f : type == 1 ? -2.0f : 0.0f;
            sy = type == 2 ? 2.0f : type == 3 ? -2.0f : 0.0f;
        }
        return Pattern(static_cast<float>(x) - sx, static_cast<float>(y) - sy);
    }, keep);
    s.depthThen = Uploaded(device, r, DXGI_FORMAT_R32_FLOAT, [](int, int) { return 0.5f; }, keep);
    s.depthNow = Uploaded(device, r, DXGI_FORMAT_R32_FLOAT, [](int, int) { return 0.5f; }, keep);
    s.fresh = Uploaded(device, r, kColor, [](int, int) { return 0.75f; }, keep);
    s.motion = Motion(device, r, 0.0f, 0.0f);
    return s;
}

// The sub-rect runs: after two frames of the chain (2, 0) texel (x, y) of kSubRect ends on the native 8x8 block
// (x - 4, y - 6) (in the frame for x <= 35). The residual's frame there: a uniform colour change C at equal depth over
// x 8..10, y 6..8 (the sub-rect's top-left corner), a colour change with a depth hint H (1.5 w) over x 8..9, y 20..21
// (bottom-left corner) and a block D at 3 w over x 20..23, y 12..15. Colour tolerance 0.04.
enum SubClass { kNone, kC, kH, kD };
SubClass SubAt(int x, int y)
{
    if (x >= 8 && x <= 10 && y >= 6 && y <= 8) return kC;
    if (x >= 8 && x <= 9 && y >= 20 && y <= 21) return kH;
    if (x >= 20 && x <= 23 && y >= 12 && y <= 15) return kD;
    return kNone;
}

Scene SubRectScene(ID3D12Device *device, Recording &r, std::vector<ComPtr<ID3D12Resource>> &keep)
{
    const auto at = [](int x, int y) { return SubAt(x / 8 + 4, y / 8 + 6); }; // the texel whose chain ends at native (x, y)
    Scene s;
    s.colourThen = Uploaded(device, r, kColor, [&](int x, int y) {
        const SubClass k = at(x, y);
        return k == kC || k == kH ? 0.625f : 0.5f;
    }, keep);
    s.depthThen = Uploaded(device, r, DXGI_FORMAT_R32_FLOAT, [&](int x, int y) {
        const SubClass k = at(x, y);
        return k == kD ? 0.35f : k == kH ? 0.425f : 0.5f;
    }, keep);
    s.colourNow = Uploaded(device, r, kColor, [](int, int) { return 0.5f; }, keep);
    s.depthNow = Uploaded(device, r, DXGI_FORMAT_R32_FLOAT, [](int, int) { return 0.5f; }, keep);
    s.fresh = Uploaded(device, r, kColor, [](int, int) { return 0.75f; }, keep);
    s.motion = Motion(device, r, 2.0f, 0.0f);
    return s;
}

// --- the runs ----------------------------------------------------------------------------------------------------------

constexpr int kSingles = 5, kPairs = 20;                    // the merge: main rules 0..4 alone, then (main, carry 1..4)
constexpr std::uint32_t kColour0 = kSingles + kPairs;       // rule 5: (0, 5) and (3, 5)
constexpr std::uint32_t kMotion6 = kColour0 + 2;            // rule 6: (0, 6), and (0, 5) on the same scene
constexpr std::uint32_t kSub0 = kMotion6 + 2;              // the sub-rect: (3, 0) on LinkFail, (3, 5), (0, 3)
constexpr std::uint32_t kRunCount = kSub0 + 3;

// The chain's x after two frames of rule 5's scene: 2 restarted, 4 inherited; the vote counts colour changes over the
// 5x5 neighbourhood clamped to the motion texture.
float CoherentColourX(int x, int y, int rule)
{
    const auto colour = [](int u, int v) { return Speckled(u, v) || Uniform(u, v); };
    if (Deep(x, y)) return 2.0f;
    if (rule == 3 || !colour(x, y)) return 4.0f; // rule 3: no depth hint here, only D restarts
    if (rule == 0) return 2.0f;
    int votes = 0;
    for (int oy = -2; oy <= 2; ++oy)
        for (int ox = -2; ox <= 2; ++ox)
            votes += colour(std::clamp(x + ox, 0, static_cast<int>(kMw) - 1), std::clamp(y + oy, 0, static_cast<int>(kMh) - 1)) ? 1 : 0;
    return votes >= PW_TEMPORAL_COHERENT_VOTES ? 2.0f : 4.0f;
}

std::uint32_t WrongColour(const unsigned char *bytes, std::uint32_t slot, int rule, const char *name)
{
    std::uint32_t wrong = 0;
    for (int y = 0; y < static_cast<int>(kMh); ++y)
        for (int x = 0; x < static_cast<int>(kMw); ++x) {
            float ax, ay;
            AccAt(bytes, slot, x, y, ax, ay);
            const float want = CoherentColourX(x, y, rule);
            if (std::fabs(ax - want) <= 1e-3f && std::fabs(ay) <= 1e-3f) continue;
            if (wrong++ < 4) std::fprintf(stderr, "  %s, texel (%d, %d): (%g, %g), expected (%g, 0)\n", name, x, y, ax, ay, want);
        }
    return wrong;
}

// The chain's x after two frames of the sub-rect scene by rule 3 or 5 (2 restarted, 4 inherited); the 5x5 vote is clamped
// to kSubRect, or with `toTexture` to the whole texture.
float SubRectX(int x, int y, int rule, bool toTexture)
{
    const auto bit = [rule](SubClass k) { return rule == 3 ? k == kH || k == kD : k == kC || k == kH; };
    const SubClass own = SubAt(x, y);
    if (rule == 5 && own == kD) return 2.0f;
    if (!bit(own)) return 4.0f;
    const int lx = toTexture ? 0 : 8, hx = toTexture ? static_cast<int>(kMw) - 1 : 39;
    const int ly = toTexture ? 0 : 6, hy = toTexture ? static_cast<int>(kMh) - 1 : 21;
    int votes = 0;
    for (int oy = -2; oy <= 2; ++oy)
        for (int ox = -2; ox <= 2; ++ox) votes += bit(SubAt(std::clamp(x + ox, lx, hx), std::clamp(y + oy, ly, hy))) ? 1 : 0;
    return votes >= (rule == 3 ? 8 : PW_TEMPORAL_COHERENT_VOTES) ? 2.0f : 4.0f;
}

void CheckSubRect(const unsigned char *bytes)
{
    const std::uint32_t single = kSub0, pair35 = kSub0 + 1, pair03 = kSub0 + 2;
    Check(std::memcmp(Plane(bytes, 2 * pair35), Plane(bytes, 2 * single), kSlot) == 0,
          "sub-rect: AccumulatePair's rule-3 main chain is byte for byte LinkFail + Accumulate's");
    Check(std::memcmp(Plane(bytes, 2 * pair03 + 1), Plane(bytes, 2 * single), kSlot) == 0,
          "sub-rect: AccumulatePair's rule-3 carry chain is byte for byte LinkFail + Accumulate's");
    for (const int rule : {3, 5}) {
        const std::uint32_t slot = rule == 3 ? 2 * single : 2 * pair35 + 1;
        std::uint32_t wrong = 0, clampDecides = 0;
        for (int y = 0; y < static_cast<int>(kMh); ++y)
            for (int x = 0; x < static_cast<int>(kMw); ++x) {
                const float want = SubRectX(x, y, rule, false);
                if (want != SubRectX(x, y, rule, true)) ++clampDecides;
                float ax, ay;
                AccAt(bytes, slot, x, y, ax, ay);
                if (std::fabs(ax - want) <= 1e-3f && std::fabs(ay) <= 1e-3f) continue;
                if (wrong++ < 4) std::fprintf(stderr, "  sub-rect rule %d, texel (%d, %d): (%g, %g), expected (%g, 0)\n", rule, x, y, ax, ay, want);
            }
        std::printf("sub-rect rule %d: %u of %u texels differ from the expected chain; the sub-rect clamp decides %u\n", rule, wrong,
                    kMw * kMh, clampDecides);
        Check(wrong == 0, "sub-rect: the chain restarts by the 5x5 vote clamped to the motion sub-rect");
        Check(clampDecides > 0, "sub-rect: the clamp decides some verdicts on this scene (the check above tests it)");
    }
}

void CheckMerge(const unsigned char *bytes)
{
    std::uint32_t differing = 0;
    for (int k = 0; k < kPairs; ++k) {
        const int main = k / 4, carry = 1 + k % 4;
        const std::uint32_t slot = kSingles + k;
        const bool mainSame = std::memcmp(Plane(bytes, 2 * slot), Plane(bytes, 2 * main), kSlot) == 0;
        const bool carrySame = std::memcmp(Plane(bytes, 2 * slot + 1), Plane(bytes, 2 * carry), kSlot) == 0;
        std::printf("merge: main %d, carry %d: main chain %s, carry chain %s\n", main, carry, mainSame ? "identical" : "DIFFERS",
                    carrySame ? "identical" : "DIFFERS");
        Check(mainSame, "merge: the main chain is byte for byte the single chain of its rule");
        Check(carrySame, "merge: the carry chain is byte for byte the single chain of the carry rule");
        if (main != carry && std::memcmp(Plane(bytes, 2 * main), Plane(bytes, 2 * carry), kSlot) != 0) ++differing;
    }
    std::printf("merge: %u of the pairs have chains of different rules that differ\n", differing);
    Check(differing >= 12, "merge: the rules give different chains on this scene (the comparisons above can fail)");
}

void CheckCoherentMotion(const unsigned char *bytes)
{
    const std::uint32_t six = kMotion6, five = kMotion6 + 1;
    Check(std::memcmp(Plane(bytes, 2 * five), Plane(bytes, 2 * five + 1), kSlot) == 0,
          "rule 6 scene: with no colour test rule 5 restarts nothing (carry chain = main chain)");
    Check(std::memcmp(Plane(bytes, 2 * six), Plane(bytes, 2 * five), kSlot) == 0, "rule 6 scene: the main chain ignores the carry rule");
    std::uint32_t restarted = 0, inside = 0, mixedKept = 0, mixed = 0;
    for (int y = 8; y <= 27; ++y)
        for (int x = 10; x <= 21; ++x) { // the uniform area's texels whose 5x5 neighbourhood lies inside it
            float mx, my, cx, cy;
            AccAt(bytes, 2 * six, x, y, mx, my);
            AccAt(bytes, 2 * six + 1, x, y, cx, cy);
            ++inside;
            if (mx < -0.25f && cx > mx + 0.1f) ++restarted; // the main chain took two corrections, the restarted one
        }
    for (int y = 6; y <= 29; ++y)
        for (int x = 32; x <= 55; ++x, ++mixed)
            if (Bits(bytes, 2 * six, x, y) == Bits(bytes, 2 * six + 1, x, y)) ++mixedKept; else std::printf("mixed restart (%d,%d)\n", x, y);
    std::printf("rule 6: %u / %u texels inside the uniform shift restarted, %u / %u of the random shifts kept\n", restarted, inside,
                mixedKept, mixed);
    Check(restarted == inside, "rule 6: a uniform visible shift against no game motion restarts the carry chain");
    Check(mixedKept * 20 >= mixed * 19, "rule 6: random shifts mostly keep it (at least 95 % of the texels)");
}

int TestCarryRules(ID3D12Device *device, ID3D12CommandQueue *queue, const char *shaderDirectory)
{
    ofps::core::gpu::Shaders shaders;
    Check(LoadShaders(shaderDirectory, shaders, true) && LoadChainPasses(shaderDirectory, shaders), "carry rules: the shaders load");
    Recording r;
    Check(r.Create(device), "carry rules: the fixtures allocate");
    if (failures) return failures;
    std::vector<ComPtr<ID3D12Resource>> keep;
    const Scene merge = MergeScene(device, r, keep), colour = CoherentColourScene(device, r, keep), motion = CoherentMotionScene(device, r, keep);
    const Scene sub = SubRectScene(device, r, keep);
    const ComPtr<ID3D12Resource> readback = AccReadback(device, 2 * kRunCount);
    Check(merge.Ready() && colour.Ready() && motion.Ready() && sub.Ready() && readback, "carry rules: the scenes upload");
    if (failures) return failures;
    Machine machines[kRunCount];
    for (std::uint32_t k = 0; k < kRunCount; ++k) {
        Config config{};
        const Scene *scene = &merge;
        if (k < kSingles) config = {static_cast<int>(k), 0, true, false, 0.1f, 3};
        else if (k < kColour0) config = {static_cast<int>(k - kSingles) / 4, 1 + static_cast<int>(k - kSingles) % 4, true, false, 0.1f, 3};
        else if (k < kMotion6) { config = {k == kColour0 ? 0 : 3, 5, false, false, 0.04f, 2}; scene = &colour; }
        else if (k < kSub0) { config = {0, k == kMotion6 ? 6 : 5, true, false, 0.0f, 2}; scene = &motion; }
        else {
            const int rules[3][2] = {{3, 0}, {3, 5}, {0, 3}};
            config = {rules[k - kSub0][0], rules[k - kSub0][1], false, false, 0.04f, 2, true};
            scene = &sub;
        }
        Check(Record(device, r, machines[k], shaders, *scene, config, readback.Get(), k), "carry rules: a run records");
        if (failures) return failures;
    }
    const unsigned char *bytes = RunAndMap(device, queue, r, readback.Get(), 2 * kRunCount);
    Check(bytes != nullptr, "carry rules: the GPU completes the passes and the chains read back");
    if (failures) return failures;
    CheckMerge(bytes);
    for (std::uint32_t k = kColour0; k < kMotion6; ++k) {
        const int rule = k == kColour0 ? 0 : 3;
        const std::uint32_t mainWrong = WrongColour(bytes, 2 * k, rule, "rule 5 scene, main chain");
        const std::uint32_t carryWrong = WrongColour(bytes, 2 * k + 1, 5, "rule 5 scene, carry chain");
        std::printf("rule 5 (main rule %d): %u / %u of %u texels differ from the expected main / carry chain\n", rule, mainWrong,
                    carryWrong, kMw * kMh);
        Check(mainWrong == 0, "rule 5 scene: the main chain restarts by its own rule");
        Check(carryWrong == 0, "rule 5: speckled colour changes keep the carry chain, a uniform area and depth restart it");
    }
    CheckCoherentMotion(bytes);
    CheckSubRect(bytes);
    const D3D12_RANGE none{0, 0};
    readback->Unmap(0, &none);
    return failures;
}
} // namespace

int main(int argc, char **argv)
{
    if (argc != 2) {
        std::cerr << "usage: ofps_temporal_carry_rules_tests <shader dir>\n";
        return 1;
    }
    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter> warp;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    D3D12_COMMAND_QUEUE_DESC desc{};
    desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || FAILED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))) ||
        FAILED(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))) ||
        FAILED(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue)))) {
        std::cerr << "FAIL: a D3D12 WARP device and queue\n";
        return 1;
    }
    int result = TestCarryRules(device.Get(), queue.Get(), argv[1]);
    ComPtr<ID3D12InfoQueue> info;
    if (debug && SUCCEEDED(device.As(&info))) {
        for (UINT64 i = 0, n = info->GetNumStoredMessages(); i < n; ++i) {
            SIZE_T size = 0;
            if (FAILED(info->GetMessage(i, nullptr, &size))) continue;
            std::vector<char> storage(size);
            auto *message = reinterpret_cast<D3D12_MESSAGE *>(storage.data());
            if (SUCCEEDED(info->GetMessage(i, message, &size)) && message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) {
                std::cerr << "FAIL: D3D12 debug layer: " << message->pDescription << '\n';
                ++result;
            }
        }
    }
    if (result == 0) std::cout << "PASS: DebugTemporalCarryChainRule 5/6 and the merged chain passes on WARP\n";
    return result == 0 ? 0 : 1;
}
