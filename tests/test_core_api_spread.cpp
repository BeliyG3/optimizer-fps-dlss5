#include "test_core_api_scenarios.h"
#include "test_core_api_checks.h"
#include "test_core_api_grid_model.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// The spread schedule (ModelPasses 2, SpreadPasses 1, TemporalMode 1 every 3rd frame) through the core API, Mode Off:
//  - the raw snapshot (core/temporal/raw_snapshot.h): the same colour pixels once in an RGBA16F texture the plain copy
//    takes and once in a TYPELESS one it refuses, so the format-converting reprojection (the route the snapshot always
//    took before) runs instead; every frame's output and every model input match byte for byte.
namespace coretest {
namespace {
OfpsSettingsValues SpreadSettings(const OfpsSettingsValues &from, int spread) {
    auto settings = from;
    SetInt(settings, OFPS_SET_MODE, 0);
    SetInt(settings, OFPS_SET_TEMPORAL_MODE, 1);
    SetInt(settings, OFPS_SET_TEMPORAL_EVERY, 3);
    SetInt(settings, OFPS_SET_MODEL_PASSES, 2);
    SetInt(settings, OFPS_SET_SPREAD_PASSES, spread);
    return settings;
}

// The texels of a RGBA16 capture, without the rows' padding.
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

// RGB of RGBA16F texels (Bytes), as ReadRgb reads a capture.
std::vector<float> Rgb(const std::vector<std::uint8_t> &texels) {
    std::vector<float> rgb;
    for (std::size_t i = 0; i + 8 <= texels.size(); i += 8)
        for (std::size_t c = 0; c < 3; ++c) {
            std::uint16_t half;
            std::memcpy(&half, texels.data() + i + c * 2, sizeof(half));
            rgb.push_back(HalfToFloat(half));
        }
    return rgb;
}

std::uint64_t Fnv(const std::vector<std::vector<std::uint8_t>> &blocks) {
    std::uint64_t h = 1469598103934665603ull;
    for (const auto &block : blocks)
        for (const std::uint8_t b : block) h = (h ^ b) * 1099511628211ull;
    return h;
}

bool LoggedSince(const FakeHost &host, std::size_t from, const char *needle) {
    for (std::size_t i = from; i < host.logs.size(); ++i)
        if (host.logs[i].find(needle) != std::string::npos) return true;
    return false;
}

// One feature through `frames` evaluates: every output, and every model input (the snapshot or the carried frame).
struct Recorded {
    std::vector<std::vector<std::uint8_t>> outputs, inputs;
    std::vector<std::uint32_t> paths;
    std::size_t runs = 0;
};
Recorded RecordSpread(WarpDevice &w, IOfpsCore *core, HostFrame &frame, int frames) {
    Recorded r;
    CheckerModelHost model;
    IOfpsFeature *feature = nullptr;
    Check(model.Initialize(w) && CreateFeatureOnList(w, core, &model, &feature) == OFPS_OK && feature,
          "spread snapshot: the checker model and the feature create");
    if (!feature) return r;
    for (int i = 0; i < frames; ++i) {
        const auto runs = model.runCalls;
        const EvalRun run = RunEvaluate(w, core, feature, frame, frame.output.Get(), 0);
        Check(run.ok && run.result == OFPS_OK, "spread snapshot: frame " + std::to_string(i) + " evaluates");
        r.outputs.push_back(Bytes(run.output));
        r.paths.push_back(run.eval.path);
        if (model.runCalls > runs) r.inputs.push_back(Bytes(model.colorCapture));
    }
    r.runs = model.runCalls;
    const OfpsStatus s = CurrentStatus(core);
    Check(s.modelPassesRunning == 2 && s.temporalMode == 1, "spread snapshot: two passes spread on the synchronous schedule");
    feature->Release();
    DrainReleasedModels(w, core);
    return r;
}

void SnapshotRoutes(WarpDevice &w, IOfpsCore *core, FakeHost &host, HostFrame &frame) {
    constexpr int kFrames = 13; // the creation frame, then four cycles of three
    std::size_t mark = host.logs.size();
    const Recorded copied = RecordSpread(w, core, frame, kFrames);
    Check(LoggedSince(host, mark, "spread raw snapshot: plain copy"), "spread snapshot: an RGBA16F colour is copied");
    HostFrame typeless = frame;
    typeless.color = CreateTexture(w.device.Get(), kW, kH, DXGI_FORMAT_R16G16B16A16_TYPELESS, D3D12_RESOURCE_FLAG_NONE, kInputRest);
    Check(typeless.color != nullptr, "spread snapshot: the TYPELESS colour allocates");
    if (!typeless.color) return;
    mark = host.logs.size();
    const Recorded converted = RecordSpread(w, core, typeless, kFrames);
    Check(LoggedSince(host, mark, "spread raw snapshot: format-converting reprojection"),
          "spread snapshot: a TYPELESS colour (another resource format than the output's) takes the reprojection");
    std::printf("spread snapshot digest: copy %016llx, reprojection %016llx (%zu frames, %zu model runs)\n",
                static_cast<unsigned long long>(Fnv(copied.outputs) ^ Fnv(copied.inputs)),
                static_cast<unsigned long long>(Fnv(converted.outputs) ^ Fnv(converted.inputs)), copied.outputs.size(), copied.runs);
    std::fflush(stdout);
    Check(copied.runs >= 8 && copied.inputs.size() == copied.runs, "spread snapshot: both stages ran in every cycle");
    Check(copied.paths == converted.paths && copied.outputs.size() == converted.outputs.size() &&
              copied.inputs.size() == converted.inputs.size(),
          "spread snapshot: both routes run the same schedule");
    for (std::size_t i = 0; i < copied.outputs.size() && i < converted.outputs.size(); ++i)
        Check(!copied.outputs[i].empty() && copied.outputs[i] == converted.outputs[i],
              "spread snapshot: frame " + std::to_string(i) + " is byte for byte the same on both routes");
    for (std::size_t i = 0; i < copied.inputs.size() && i < converted.inputs.size(); ++i)
        Check(!copied.inputs[i].empty() && copied.inputs[i] == converted.inputs[i],
              "spread snapshot: model input " + std::to_string(i) + " is byte for byte the same on both routes");
    const Difference edit = Compare(Rgb(copied.outputs.back()), SourceRgb());
    Check(edit.valid && edit.pixels > kW * kH / 4, "spread snapshot: the frames carry the model's edit, not the colour");
}
} // namespace

void ScenarioSpreadPasses(WarpDevice &w, IOfpsCore *core, FakeHost &host, HostFrame &frame) {
    const auto before = CurrentSettings(core);
    auto settings = SpreadSettings(before, 1);
    Check(core->SetSettings(&settings) == OFPS_OK, "spread scenario: two spread passes");
    SnapshotRoutes(w, core, host, frame);
    Check(core->SetSettings(&before) == OFPS_OK, "spread scenario: the previous settings are restored");
}
} // namespace coretest
