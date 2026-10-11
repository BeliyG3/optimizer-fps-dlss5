#include "test_core_api_scenarios.h"
#include "test_core_api_checks.h"
#include "test_core_api_grid_model.h"
#include "core/context.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

// TemporalSkyField on the spread schedule (ModelPasses 2, SpreadPasses 1, TemporalMode 1 every 3rd frame, Mode Off)
// through the core API, over a host depth whose top half is the far plane (1.0). The hidden stage's carried frame
// (the second pass's model input) and the shown machine's carried frames are both formed with the sky field:
//  - (9) TemporalSkyField=0 from the first frame: every output and every model input is byte for byte what master's
//    four changed passes (tests/fixtures/temporal_pre_sky, swapped into the core's shader set) produce;
//  - TemporalSkyField=1 changes the hidden stage's carried frames (pass 2 sees the field) and the shown frames.
// Static core only: the swap edits the core's loaded shader set (core/context.h).
namespace coretest {
namespace {
std::vector<std::uint8_t> Bytes(const ReadbackCapture &capture) {
    std::vector<std::uint8_t> bytes;
    void *mapped = nullptr;
    const D3D12_RANGE range{0, static_cast<SIZE_T>(capture.byteCount)};
    if (!capture.buffer || FAILED(capture.buffer->Map(0, &range, &mapped)))
        return bytes;
    const auto &f = capture.footprint.Footprint;
    for (UINT y = 0; y < f.Height; ++y) {
        const auto *row = static_cast<const std::uint8_t *>(mapped) + capture.footprint.Offset + static_cast<std::size_t>(y) * f.RowPitch;
        bytes.insert(bytes.end(), row, row + static_cast<std::size_t>(f.Width) * 8);
    }
    const D3D12_RANGE noWrite{0, 0};
    capture.buffer->Unmap(0, &noWrite);
    return bytes;
}

// R32F, the far plane (1.0) in the top half and 0.5 below; rests in kInputRest.
ComPtr<ID3D12Resource> SkyDepth(WarpDevice &w) {
    ID3D12Device *d = w.device.Get();
    ComPtr<ID3D12Resource> depth = CreateTexture(d, kW, kH, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, kInputRest);
    ComPtr<ID3D12DescriptorHeap> heap = CreateRtvHeap(d, 1);
    if (!depth || !heap || !BeginList(w)) return nullptr;
    const auto rtv = RtvAt(d, heap.Get(), 0);
    d->CreateRenderTargetView(depth.Get(), nullptr, rtv);
    const float sky[4] = {1.0f, 0, 0, 0}, ground[4] = {0.5f, 0, 0, 0};
    const D3D12_RECT bottom{0, LONG(kH / 2), LONG(kW), LONG(kH)};
    Transition(w.list.Get(), depth.Get(), kInputRest, D3D12_RESOURCE_STATE_RENDER_TARGET);
    w.list->ClearRenderTargetView(rtv, sky, 0, nullptr);
    w.list->ClearRenderTargetView(rtv, ground, 1, &bottom);
    Transition(w.list.Get(), depth.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, kInputRest);
    return SubmitList(w) && WaitForQueue(d, w.queue.Get()) ? depth : nullptr;
}

struct Recorded {
    std::vector<std::vector<std::uint8_t>> outputs, inputs;
    std::size_t runs = 0;
};
Recorded Record(WarpDevice &w, IOfpsCore *core, HostFrame &frame, ID3D12Resource *depth, int frames) {
    Recorded r;
    CheckerModelHost model;
    IOfpsFeature *feature = nullptr;
    Check(model.Initialize(w) && CreateFeatureOnList(w, core, &model, &feature) == OFPS_OK && feature,
          "sky spread: the checker model and the feature create");
    if (!feature) return r;
    for (int i = 0; i < frames; ++i) {
        OfpsFrameInputs in = FrameInputs(frame, frame.output.Get(), 0);
        in.depth.res = depth;
        const auto runs = model.runCalls;
        const EvalRun run = RunEvaluate(w, core, feature, frame, frame.output.Get(), 0, &in);
        Check(run.ok && run.result == OFPS_OK, "sky spread: frame " + std::to_string(i) + " evaluates");
        r.outputs.push_back(Bytes(run.output));
        if (model.runCalls > runs) r.inputs.push_back(Bytes(model.colorCapture));
    }
    r.runs = model.runCalls;
    Check(CurrentStatus(core).modelPassesRunning == 2, "sky spread: two passes spread on the synchronous schedule");
    feature->Release();
    DrainReleasedModels(w, core);
    return r;
}

bool LoadBinary(const std::filesystem::path &path, std::vector<char> &code) {
    std::ifstream file(path, std::ios::binary);
    code.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    return !code.empty();
}

// Master's Reproject, Compose, ComposeGrid and Apply in the core's loaded set while it lives; machines created then
// (a new feature's) build their pipelines from them.
struct MasterPasses {
    ofps::core::gpu::Shaders &shaders = ofps::core::Ctx().shaders;
    std::vector<char> *slots[4] = {&shaders.temporalReproject, &shaders.temporalCompose, &shaders.temporalComposeGrid, &shaders.temporalApply};
    std::vector<char> saved[4];
    bool ok = true;
    MasterPasses() {
        const char *names[4] = {"temporal_Reproject_cs.dxbc", "temporal_Compose_cs.dxbc", "temporal_ComposeGrid_cs.dxbc", "temporal_Apply_cs.dxbc"};
        for (int i = 0; i < 4; ++i) {
            saved[i] = *slots[i];
            ok = LoadBinary(std::filesystem::path(OFPS_SKY_MASTER_DIR) / names[i], *slots[i]) && ok;
        }
    }
    ~MasterPasses() { for (int i = 0; i < 4; ++i) *slots[i] = saved[i]; }
};

bool Same(const std::vector<std::vector<std::uint8_t>> &a, const std::vector<std::vector<std::uint8_t>> &b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (a[i].empty() || a[i] != b[i]) return false;
    return true;
}
} // namespace

void ScenarioSkySpread(WarpDevice &w, IOfpsCore *core, HostFrame &frame) {
    constexpr int kFrames = 13; // the creation frame, then four cycles of three
    const auto before = CurrentSettings(core);
    auto settings = before;
    SetInt(settings, OFPS_SET_MODE, 0);
    SetInt(settings, OFPS_SET_TEMPORAL_MODE, 1);
    SetInt(settings, OFPS_SET_TEMPORAL_EVERY, 3);
    SetInt(settings, OFPS_SET_MODEL_PASSES, 2);
    SetInt(settings, OFPS_SET_SPREAD_PASSES, 1);
    ComPtr<ID3D12Resource> depth = SkyDepth(w);
    Check(depth != nullptr, "sky spread: the sky depth paints");
    if (!depth) return;
    auto with = [&](int sky) {
        SetInt(settings, OFPS_SET_TEMPORAL_SKY_FIELD, sky);
        Check(core->SetSettings(&settings) == OFPS_OK, "sky spread: the settings are accepted");
    };
    with(0);
    const Recorded off = Record(w, core, frame, depth.Get(), kFrames);
    Recorded master;
    {
        MasterPasses swap;
        Check(swap.ok, "sky spread: master's passes load");
        master = Record(w, core, frame, depth.Get(), kFrames);
    }
    with(1);
    const Recorded on = Record(w, core, frame, depth.Get(), kFrames);
    Check(off.runs >= 8 && off.inputs.size() == off.runs, "sky spread: both stages ran in every cycle");
    Check(Same(off.outputs, master.outputs) && Same(off.inputs, master.inputs),
          "sky spread (9): TemporalSkyField=0 from the first frame writes master's bytes, outputs and model inputs");
    std::size_t inputsChanged = 0, outputsChanged = 0;
    for (std::size_t i = 0; i < off.inputs.size() && i < on.inputs.size(); ++i) inputsChanged += off.inputs[i] != on.inputs[i] ? 1 : 0;
    for (std::size_t i = 0; i < off.outputs.size() && i < on.outputs.size(); ++i) outputsChanged += off.outputs[i] != on.outputs[i] ? 1 : 0;
    std::printf("sky spread: off = master over %zu frames and %zu model inputs, bit for bit; on changes %zu model inputs (the "
                "second pass's carried frames) and %zu frames\n", off.outputs.size(), off.inputs.size(), inputsChanged, outputsChanged);
    std::fflush(stdout);
    Check(inputsChanged > 0, "sky spread: the hidden stage's carried frame (pass 2's input) does not take the sky field");
    Check(outputsChanged > 0, "sky spread: the shown frames do not take the sky field");
    Check(core->SetSettings(&before) == OFPS_OK, "sky spread: the previous settings are restored");
}
} // namespace coretest
