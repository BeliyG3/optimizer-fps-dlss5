// TemporalSkyField in the background mode (TemporalMode 3) on D3D12 WARP: a Background machine driven the way
// core/temporal/async_scheduler.cpp drives it (temporal_background_driver.h: a kick every 3rd frame, adoption on the
// frame after it through UseKickDepth, a carried frame every frame). The scheduler adds nothing sky-specific beyond
// UseKickDepth, which this sequence calls.
// The kick frames see their depth through the left half of a 2x-wide depth texture (sky at 1.0 around a building);
// the adoption frames say the right half (no sky) and reversed Z.
//   - the adopted residual's field is built from the kick's depth as the kick described it: the following frames'
//     sky is colour + the pass's sky residual; with the adoption frame's description (the bug of 83ad206) it is not;
//   - (9) the setting off from the first frame writes master's bytes (tests/fixtures/temporal_pre_sky), and (6) a menu
//     evaluation those of the setting off. The adoption frames describe their depth unlike the kick frames, and since
//     2026.10.2 the depth snapshots are read through their own sub-rect (test_temporal_depth_rect.cpp), which master
//     did not do: (9) compares both with the passes that read t6 built the old way (PW_T_DEPTHF_SHARED_RECT).
#include "temporal_background_driver.h"
#include "core/temporal/sky_field.h"

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

constexpr std::uint32_t kDepthW = 2 * kW;
constexpr int kFrames = 11, kEvery = 3; // kicks on frames 0, 3, 6, 9; adoptions on 1, 4, 7, 10
const Rect kKickRect{0, 0, kW, kH}, kAdoptRect{kW, 0, kW, kH};

// The host depth: sky (1.0) around a building (0.25) in the left half, 0.5 (no sky) in the right half; rests readable.
ComPtr<ID3D12Resource> WideDepth(ID3D12Device *device, Recording &r)
{
    ComPtr<ID3D12Resource> texture = Plain(device, kDepthW, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    if (!texture || r.painted >= 16) return nullptr;
    auto rtv = r.heap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(r.painted++) * device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    device->CreateRenderTargetView(texture.Get(), nullptr, rtv);
    BarrierExternal(r.list.Get(), texture.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
    const float sky[4] = {1.0f, 0, 0, 0}, building[4] = {0.25f, 0, 0, 0}, ground[4] = {0.5f, 0, 0, 0};
    const D3D12_RECT block{40, 30, 140, 90}, right{LONG(kW), 0, LONG(kDepthW), LONG(kH)};
    r.list->ClearRenderTargetView(rtv, sky, 0, nullptr);
    r.list->ClearRenderTargetView(rtv, building, 1, &block);
    r.list->ClearRenderTargetView(rtv, ground, 1, &right);
    BarrierExternal(r.list.Get(), texture.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, kRead);
    return texture;
}

struct Scene {
    ComPtr<ID3D12Resource> color, depth, motion, fresh[2];
};

FrameInputs Inputs(const Scene &scene, int frame)
{
    FrameInputs in{};
    const bool adoption = frame % kEvery == 1;
    in.color = scene.color.Get(); in.depth = scene.depth.Get(); in.motion = scene.motion.Get();
    in.colorView = kColor; in.depthView = DXGI_FORMAT_R32_FLOAT; in.motionView = DXGI_FORMAT_R16G16_FLOAT;
    in.colorRect = in.motionRect = {0, 0, kW, kH};
    in.depthRect = adoption ? kAdoptRect : kKickRect;
    in.depthInverted = adoption;
    in.hostInputState = in.depthState = in.motionState = kRead;
    in.background = true;
    in.phaseInFrames = kEvery - 1;
    return in;
}

struct RunDesc {
    const ofps::core::gpu::Shaders *shaders;
    bool skyField, allowSky, kickDepth; // kickDepth false: the adoption frame's depth description (83ad206)
};

void RecordRun(Machine &machine, ID3D12GraphicsCommandList *list, const Scene &scene, Job &job, const RunDesc &run,
               ID3D12Resource *const *outputs)
{
    ID3D12Resource *const fresh[2] = {scene.fresh[0].Get(), scene.fresh[1].Get()};
    for (int frame = 0; frame < kFrames; ++frame) {
        FrameInputs in = Inputs(scene, frame);
        in.skyField = run.skyField;
        in.allowSky = run.allowSky;
        RecordBackgroundFrame(machine, list, job, in, fresh, outputs[frame], kEvery, run.kickDepth);
    }
}

bool Sky(std::size_t texel)
{
    const std::size_t x = (texel / 4) % kW, y = (texel / 4) / kW;
    return !(x >= 40 && x < 140 && y >= 30 && y < 90);
}

// SkyField::Create on a size no texture can have: false and nothing kept (resources_create.cpp then logs once and leaves
// the field off; the machine is not failed). The runtime's own complaint about the size is kept out of the debug-layer
// check in main(); it is the point of this case.
void TestPyramidFailure(ID3D12Device *device)
{
    ComPtr<ID3D12InfoQueue> info;
    const bool filtered = SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&info)));
    if (filtered) {
        D3D12_MESSAGE_SEVERITY all[] = {D3D12_MESSAGE_SEVERITY_CORRUPTION, D3D12_MESSAGE_SEVERITY_ERROR, D3D12_MESSAGE_SEVERITY_WARNING,
                                        D3D12_MESSAGE_SEVERITY_INFO, D3D12_MESSAGE_SEVERITY_MESSAGE};
        D3D12_INFO_QUEUE_FILTER deny{};
        deny.DenyList.NumSeverities = static_cast<UINT>(std::size(all));
        deny.DenyList.pSeverityList = all;
        info->PushStorageFilter(&deny);
    }
    SkyField tooWide, empty, single;
    Check(!tooWide.Create(device, 1u << 20, 1) && !tooWide.Created(), "sky background: a pyramid wider than a texture can be fails cleanly");
    Check(!empty.Create(device, 0, 8) && !empty.Created(), "sky background: an empty grid gives no pyramid");
    if (filtered) info->PopStorageFilter();
    Check(single.Create(device, 1, 1) && single.Created(), "sky background: a 1x1 grid is its own coarsest level");
    std::printf("sky field: a pyramid that cannot be created leaves nothing behind (Created() false, the field off)\n");
}
} // namespace

int TestTemporalSkyBackground(ID3D12Device *device, ID3D12CommandQueue *queue, const char *shaderDirectory, const char *masterFixture,
                              const char *sharedRectDirectory)
{
    TestPyramidFailure(device);
    ofps::core::gpu::Shaders current, master;
    Check(LoadShaders(shaderDirectory, current, true) && LoadSkyShaders(shaderDirectory, current), "sky background: the current shaders load");
    ofps::core::gpu::Shaders currentShared;
    Check(LoadShaders(shaderDirectory, currentShared, true) && LoadSkyShaders(shaderDirectory, currentShared) &&
              LoadDepthFSharedPasses(sharedRectDirectory, currentShared),
          "sky background: the shared-rect passes load");
    Check(LoadShaders(shaderDirectory, master, true) && LoadDepthFSharedPasses(sharedRectDirectory, master) &&
              LoadMasterPasses(masterFixture, master),
          "sky background: master's passes load");
    Recording r;
    Check(r.Create(device), "sky background: fixtures allocate");
    if (failures) return failures;

    const float sky[3] = {0.375f, 0.5f, 0.625f}, building[3] = {0.25f, 0.125f, 0.0625f};
    const float skyResidual[2][3] = {{0.125f, 0.0625f, -0.125f}, {-0.0625f, 0.125f, 0.0625f}};
    Scene scene;
    scene.color = r.Paint(device, kColor, {sky[0], sky[1], sky[2], 1.0f}, {building[0], building[1], building[2], 1.0f}, true);
    scene.depth = WideDepth(device, r);
    scene.motion = r.Paint(device, DXGI_FORMAT_R16G16_FLOAT, {8.0f, 0, 0, 0}, {}, false); // 8 px a frame: no cross-pass blend
    for (int pass = 0; pass < 2; ++pass) {
        const float base[4] = {sky[0] + skyResidual[pass][0], sky[1] + skyResidual[pass][1], sky[2] + skyResidual[pass][2], 1.0f};
        const float patch[4] = {building[0] + 0.5f, building[1] + 0.25f * float(pass + 1), building[2] + 0.375f, 1.0f};
        scene.fresh[pass] = r.Paint(device, kColor, base, patch, true);
    }
    Check(scene.color && scene.depth && scene.motion && scene.fresh[0] && scene.fresh[1], "sky background: the scene paints");
    if (failures) return failures;

    const RunDesc runs[] = {
        {&current, true, true, true},   // 0: on
        {&current, true, true, false},  // 1: on, the kick's depth read as the adoption frame describes it
        {&current, false, true, true},  // 2: off from the first frame
        {&master, false, true, true},   // 3: master's passes
        {&current, true, false, true},  // 4: a menu evaluation
        {&currentShared, false, true, true}}; // 5: off, the depth snapshots read through the current frame's sub-rect
    constexpr std::size_t kRuns = std::size(runs);
    std::vector<ComPtr<ID3D12Resource>> outputs(kRuns * kFrames);
    std::vector<Machine> machines(kRuns);
    std::vector<Job> jobs(kRuns);
    for (std::size_t i = 0; i < kRuns; ++i) {
        Job &job = jobs[i];
        const bool copies = job.Create(device, r.list.Get(), kDepthW);
        ID3D12Resource *raw[kFrames] = {};
        for (int f = 0; f < kFrames; ++f) raw[f] = (outputs[i * kFrames + f] = Texture(device, kColor, D3D12_RESOURCE_FLAG_NONE)).Get();
        char error[256]{};
        Check(copies &&
                  machines[i].Initialize(device, *runs[i].shaders, kW, kH, kColor, kColor, kW, kH, DXGI_FORMAT_R32_FLOAT, kDepthW, kH,
                                         error, sizeof(error), 3, MachineRole::Background),
              "sky background: the Background machine initializes");
        if (!machines[i].Ready()) { std::cerr << error << '\n'; return failures; }
        machines[i].SetUsePoint(r.Point(), 0);
        RecordRun(machines[i], r.list.Get(), scene, job, runs[i], raw);
        Check(!machines[i].TakeExhausted(), "sky background: the sequence records within the descriptor ring");
    }
    // Frame 0 has no residual yet: its output is left as created (zeros) in every run.
    const std::vector<Picture> pictures = Execute(device, queue, r, outputs);
    Check(pictures.size() == outputs.size(), "sky background: the GPU completes the passes and the frames read back");
    if (pictures.size() != outputs.size()) return failures;
    auto picture = [&](std::size_t run, int frame) -> const Picture & { return pictures[run * kFrames + frame]; };
    auto same = [&](std::size_t a, std::size_t b) {
        for (int f = 0; f < kFrames; ++f) if (!SameBits(picture(a, f), picture(b, f))) return false;
        return true;
    };
    Check(same(5, 3), "sky background (9): the setting off from the first frame writes master's bytes");
    Check(same(2, 4), "sky background (6): a menu evaluation writes the bytes of the setting off");
    // The frames that see the sky (the kick frames' description) after an adoption: colour + the adopted pass's sky residual.
    float worst[2] = {};
    int checked = 0;
    for (int f = 2; f < kFrames; ++f) {
        if (f % kEvery == 1) continue; // an adoption frame: its depth says no sky
        const int pass = ((f - 1) / kEvery) % 2;
        for (std::size_t run = 0; run < 2; ++run)
            for (std::size_t t = 0; t < picture(run, f).size(); ++t)
                if (t % 4 != 3 && Sky(t))
                    worst[run] = std::max(worst[run], std::abs(picture(run, f)[t] - (sky[t % 4] + skyResidual[pass][t % 4])));
        ++checked;
    }
    std::printf("sky background: %d carried frames after adoptions, sky = colour + the adopted pass's sky residual within %.3g; "
                "with the adoption frame's depth description %.3g; off = master = menu, bit for bit\n", checked, worst[0], worst[1]);
    Check(worst[0] <= 2.0e-3f, "sky background: the adopted field does not classify the kick's depth as the kick described it");
    Check(worst[1] > 0.01f, "sky background: the scene cannot tell the kick's depth description from the adoption frame's");
    return failures;
}
