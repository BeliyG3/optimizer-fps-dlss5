// TemporalSkyField through the machine on D3D12 WARP (Machine::RecordResidual builds the field, RecordReproject and
// RecordApply form the frames), whole sequences: three passes with a phase-in (sync), two carried frames after each,
// on the native compose route, on a half grid and on the direct route (no compose pass). The scene is a sky (far
// plane) around a building, with the whole frame moving, so sky chains run into the building.
//   (9) the setting off from the first frame writes the same bytes as master's passes (tests/fixtures/temporal_pre_sky);
//   (6) a menu evaluation (allowSky false), and a machine without the sky shaders, write the bytes of the setting off;
//   on: every sky pixel of every carried frame is colour + the pass's sky residual, and so is every sky pixel of a
//   publish frame during its phase-in; (7) the same with reversed Z; a scene without sky writes the bytes of off;
//   (11) fix round 2, the carried frames' own depth differs from the passes': a reveal (the passes saw the block's
//   18 % of sky, the carried frames show the 82 % around it) writes the carried frames of off bit for bit, where the
//   first sky field's passes (tests/fixtures/temporal_sky_round1) spread the field over them; the reverse (82 % seen,
//   18 % shown) gives the carried frames' sky colour + the pass's sky residual.
#include "temporal_d3d12_fixture.h"
#include "core/temporal/machine.h"

#include <algorithm>
#include <cstdio>
#include <iostream>

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

struct Scene {
    ComPtr<ID3D12Resource> color, depth, motion, fresh[2];
    ComPtr<ID3D12Resource> carriedDepth; // the carried frames' depth when it is not the passes'
    bool inverted = false;
};

enum class Route { Compose, Grid, Direct };

struct RunDesc {
    const ofps::core::gpu::Shaders *shaders;
    const Scene *scene;
    Route route;
    bool skyField, allowSky;
};

constexpr int kOutputs = 8; // pass 0: 2 carried; passes 1 and 2: the publish frame (phase-in) and 2 carried

void RecordRun(Machine &machine, ID3D12GraphicsCommandList *list, const RunDesc &run, ID3D12Resource *const *outputs)
{
    FrameInputs in{};
    const Scene &scene = *run.scene;
    in.color = scene.color.Get(); in.depth = scene.depth.Get(); in.motion = scene.motion.Get();
    in.colorView = kColor; in.depthView = DXGI_FORMAT_R32_FLOAT; in.motionView = DXGI_FORMAT_R16G16_FLOAT;
    in.colorRect = in.depthRect = in.motionRect = {0, 0, kW, kH};
    in.hostInputState = in.depthState = in.motionState = kRead;
    in.depthInverted = scene.inverted;
    in.residualBlend = 0.5f;
    in.phaseInFrames = 2;
    in.compose = run.route != Route::Direct;
    in.skyField = run.skyField;
    in.allowSky = run.allowSky;
    int next = 0;
    for (int pass = 0; pass < 3; ++pass) {
        in.gridWidth = in.gridHeight = 0;
        if (pass > 0) machine.RecordAccumulate(list, in);
        machine.RecordResidual(list, in, scene.fresh[pass % 2].Get(), kRead);
        if (pass > 0) {
            Check(machine.PhaseInActive(), "sky machine: a pass after the first is phased in");
            machine.RecordApply(list, in, outputs[next++], D3D12_RESOURCE_STATE_COMMON, 0, 0);
        }
        if (run.route == Route::Grid) { in.gridWidth = kW / 2; in.gridHeight = kH / 2; }
        if (scene.carriedDepth) in.depth = scene.carriedDepth.Get();
        for (int carried = 0; carried < 2; ++carried) {
            machine.RecordAccumulate(list, in);
            machine.RecordReproject(list, in, outputs[next++], D3D12_RESOURCE_STATE_COMMON, 0, 0);
        }
        in.depth = scene.depth.Get();
    }
}

// The sky (far-plane) pixels of the scene: outside the painted block (Recording::Paint).
bool Sky(std::size_t texel)
{
    const std::size_t x = (texel / 4) % kW, y = (texel / 4) / kW;
    return !(x >= 40 && x < 140 && y >= 30 && y < 90);
}

// Every sky channel of an output is colour + `residual` (fp16 tolerance); returns the largest deviation.
float SkyDeviation(const Picture &picture, const float (&colour)[3], const float (&residual)[3])
{
    float worst = 0.0f;
    for (std::size_t t = 0; t < picture.size(); ++t)
        if (t % 4 != 3 && Sky(t)) worst = std::max(worst, std::abs(picture[t] - (colour[t % 4] + residual[t % 4])));
    return worst;
}
} // namespace

int TestTemporalSkyMachine(ID3D12Device *device, ID3D12CommandQueue *queue, const char *shaderDirectory, const char *masterFixture,
                           const char *round1Fixture)
{
    ofps::core::gpu::Shaders current, noSky, master, round1;
    Check(LoadShaders(shaderDirectory, current, true) && LoadSkyShaders(shaderDirectory, current), "sky machine: the current shaders load");
    Check(LoadShaders(shaderDirectory, noSky, true) && !noSky.TemporalSkyLoaded(), "sky machine: a set without the sky passes loads");
    Check(LoadShaders(shaderDirectory, master, true) && LoadMasterPasses(masterFixture, master), "sky machine: master's passes load");
    Check(LoadShaders(shaderDirectory, round1, true) && LoadSkyShaders(shaderDirectory, round1), "sky machine: round 1's set loads");
    ReadInto(std::filesystem::path(round1Fixture) / "temporal_Reproject_cs.dxbc", round1.temporalReproject);
    ReadInto(std::filesystem::path(round1Fixture) / "temporal_Compose_cs.dxbc", round1.temporalCompose);
    ReadInto(std::filesystem::path(round1Fixture) / "temporal_ComposeGrid_cs.dxbc", round1.temporalComposeGrid);
    Check(!round1.temporalReproject.empty() && !round1.temporalCompose.empty() && !round1.temporalComposeGrid.empty(),
          "sky machine: round 1's frame passes load");
    Recording r;
    Check(r.Create(device), "sky machine: fixtures allocate");
    if (failures) return failures;

    // Colour, fresh answers and sky residuals in fp16-exact steps; the block is the building.
    const float sky[3] = {0.375f, 0.5f, 0.625f}, building[3] = {0.25f, 0.125f, 0.0625f};
    const float skyResidual[2][3] = {{0.125f, 0.0625f, -0.125f}, {-0.0625f, 0.125f, 0.0625f}};
    auto fresh = [&](int pass) {
        const float base[4] = {sky[0] + skyResidual[pass][0], sky[1] + skyResidual[pass][1], sky[2] + skyResidual[pass][2], 1.0f};
        const float patch[4] = {building[0] + 0.5f, building[1] + 0.25f * float(pass + 1), building[2] + 0.375f, 1.0f};
        return r.Paint(device, kColor, base, patch, true);
    };
    Scene outdoor, reversed, indoor, reveal, narrowing;
    outdoor.color = r.Paint(device, kColor, {sky[0], sky[1], sky[2], 1.0f}, {building[0], building[1], building[2], 1.0f}, true);
    outdoor.depth = r.Paint(device, DXGI_FORMAT_R32_FLOAT, {1.0f, 0, 0, 0}, {0.25f, 0, 0, 0}, true);
    // 8 px a frame: a chain the accumulation restarts is still too long for the cross-pass blend of the residual
    // (PSResidual fades it out from 6 px), so every pass's sky residual is exactly its own.
    outdoor.motion = r.Paint(device, DXGI_FORMAT_R16G16_FLOAT, {8.0f, 0, 0, 0}, {}, false);
    outdoor.fresh[0] = fresh(0);
    outdoor.fresh[1] = fresh(1);
    reversed = outdoor;
    reversed.depth = r.Paint(device, DXGI_FORMAT_R32_FLOAT, {0.0f, 0, 0, 0}, {0.75f, 0, 0, 0}, true);
    reversed.inverted = true;
    indoor = outdoor;
    indoor.depth = r.Paint(device, DXGI_FORMAT_R32_FLOAT, {0.5f, 0, 0, 0}, {0.25f, 0, 0, 0}, true);
    // Sky only in the block: what the passes of the reveal see, and the carried frames of the reverse.
    const ComPtr<ID3D12Resource> blockSky = r.Paint(device, DXGI_FORMAT_R32_FLOAT, {0.25f, 0, 0, 0}, {1.0f, 0, 0, 0}, true);
    reveal = outdoor;
    reveal.depth = blockSky;
    reveal.carriedDepth = outdoor.depth;
    narrowing = outdoor;
    narrowing.carriedDepth = blockSky;

    std::vector<RunDesc> runs;
    for (const Route route : {Route::Compose, Route::Grid, Route::Direct}) {
        runs.push_back({&current, &outdoor, route, true, true});  // on
        runs.push_back({&current, &outdoor, route, false, true}); // off from the first frame
        runs.push_back({&master, &outdoor, route, false, true});  // master's passes
        runs.push_back({&current, &outdoor, route, true, false}); // a menu evaluation
        runs.push_back({&noSky, &outdoor, route, true, true});    // the sky passes missing
    }
    runs.push_back({&current, &reversed, Route::Compose, true, true});
    runs.push_back({&current, &indoor, Route::Compose, true, true});
    runs.push_back({&current, &indoor, Route::Compose, false, true});
    for (const Route route : {Route::Compose, Route::Direct}) {
        runs.push_back({&current, &reveal, route, true, true});  // 18, 20: on
        runs.push_back({&current, &reveal, route, false, true}); // 19, 21: off
    }
    runs.push_back({&current, &narrowing, Route::Compose, true, true});  // 22
    runs.push_back({&current, &narrowing, Route::Compose, false, true}); // 23
    runs.push_back({&round1, &reveal, Route::Compose, true, true});      // 24: the first sky field
    std::vector<ComPtr<ID3D12Resource>> outputs(runs.size() * kOutputs);
    std::vector<Machine> machines(runs.size());
    for (std::size_t i = 0; i < runs.size(); ++i) {
        ID3D12Resource *raw[kOutputs] = {};
        for (int o = 0; o < kOutputs; ++o) raw[o] = (outputs[i * kOutputs + o] = Texture(device, kColor, D3D12_RESOURCE_FLAG_NONE)).Get();
        char error[256]{};
        Check(machines[i].Initialize(device, *runs[i].shaders, kW, kH, kColor, kColor, kW, kH, DXGI_FORMAT_R32_FLOAT, kW, kH, error, sizeof(error)),
              "sky machine: the machine initializes");
        if (!machines[i].Ready()) { std::cerr << error << '\n'; return failures; }
        machines[i].SetUsePoint(r.Point(), 0);
        RecordRun(machines[i], r.list.Get(), runs[i], raw);
        Check(!machines[i].TakeExhausted(), "sky machine: the passes and the field's build record within the descriptor ring");
    }
    const std::vector<Picture> pictures = Execute(device, queue, r, outputs);
    Check(pictures.size() == outputs.size(), "sky machine: the GPU completes the passes and the frames read back");
    if (pictures.size() != outputs.size()) return failures;
    auto picture = [&](std::size_t run, int output) -> const Picture & { return pictures[run * kOutputs + output]; };
    auto same = [&](std::size_t a, std::size_t b) {
        for (int o = 0; o < kOutputs; ++o) if (!SameBits(picture(a, o), picture(b, o))) return false;
        return true;
    };
    const int residualOf[kOutputs] = {0, 0, 1, 1, 1, 0, 0, 0}; // outputs 0-1 pass 0, 2-4 pass 1, 5-7 pass 2
    for (std::size_t route = 0; route < 3; ++route) {
        const std::size_t on = route * 5, off = on + 1;
        Check(same(off, on + 2), "sky machine (9): the setting off from the first frame writes master's bytes");
        Check(same(off, on + 3), "sky machine (6): a menu evaluation writes the bytes of the setting off");
        Check(same(off, on + 4), "sky machine: without the sky passes the frames are those of the setting off");
        Check(!same(on, off), "sky machine: the setting on changes the sky");
        float worst = 0.0f;
        for (int o = 0; o < kOutputs; ++o) worst = std::max(worst, SkyDeviation(picture(on, o), sky, skyResidual[residualOf[o]]));
        // The scene can tell: with the setting off, the carried sky beside the building is not the pass's sky residual.
        float offWorst = 0.0f;
        for (int o = 0; o < kOutputs; ++o) offWorst = std::max(offWorst, SkyDeviation(picture(off, o), sky, skyResidual[residualOf[o]]));
        Check(offWorst > 0.01f, "sky machine: the scene cannot tell the carried sky from the field");
        std::printf("sky machine, route %zu: off = master = menu = no sky passes, bit for bit; on: sky = colour + the pass's sky "
                    "residual within %.3g (off: %.3g)\n", route, worst, offWorst);
        Check(worst <= 2.0e-3f, "sky machine: a sky pixel of a carried or publish frame is not colour + the pass's sky residual");
    }
    float reversedWorst = 0.0f;
    for (int o = 0; o < kOutputs; ++o) reversedWorst = std::max(reversedWorst, SkyDeviation(picture(15, o), sky, skyResidual[residualOf[o]]));
    std::printf("sky machine (7): reversed Z, sky = colour + the pass's sky residual within %.3g\n", reversedWorst);
    Check(reversedWorst <= 2.0e-3f, "sky machine (7): reversed Z sky pixels do not take the field");
    Check(same(16, 17), "sky machine: a scene without sky writes the bytes of the setting off");
    // (11) Fix round 2: the carried frames weigh the field by their own share of sky against the pass's.
    const int carriedOutputs[] = {0, 1, 3, 4, 6, 7};
    auto carriedSame = [&](std::size_t a, std::size_t b) {
        for (const int o : carriedOutputs) if (!SameBits(picture(a, o), picture(b, o))) return false;
        return true;
    };
    Check(carriedSame(18, 19) && carriedSame(20, 21), "sky machine (11): a reveal's carried frames are not those of the setting off");
    Check(!carriedSame(24, 19), "sky machine (11): the scene cannot tell the first sky field's spread from the setting off");
    float narrowWorst = 0.0f, narrowOff = 0.0f;
    for (const int o : carriedOutputs)
        for (std::size_t t = 0; t < picture(22, o).size(); ++t)
            if (t % 4 != 3 && !Sky(t)) { // the block: sky in the carried frames' depth
                const float want = building[t % 4] + skyResidual[residualOf[o]][t % 4];
                narrowWorst = std::max(narrowWorst, std::abs(picture(22, o)[t] - want));
                narrowOff = std::max(narrowOff, std::abs(picture(23, o)[t] - want));
            }
    std::printf("sky machine (11): a reveal (18 %% seen, 82 %% shown) keeps the carried frames of off bit for bit (compose and "
                "direct routes; round 1's passes did not); the reverse (82 %% seen, 18 %% shown) gives the block colour + the pass's sky residual within %.3g "
                "(off: %.3g)\n", narrowWorst, narrowOff);
    Check(narrowWorst <= 2.0e-3f && narrowOff > 0.01f, "sky machine (11): a carried frame showing less sky than the pass saw does not take the field");
    return failures;
}
