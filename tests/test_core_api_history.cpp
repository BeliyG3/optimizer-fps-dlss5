#include "test_core_api_scenarios.h"
#include "test_core_api_checks.h"
#include "test_core_api_codec.h"

#include <string>

// The NR history rule (core/frame/model_passes.h) through the core API, Mode Off. A feature is evaluated with Reset
// only when its last successful evaluate is not the frame the motion it gets reaches back to (or it has none). The
// fake model hosts record every run's feature and Reset: P = the host's real feature, E = an extra pass.
//  - ModelPasses 1: TemporalMode 1 steady, 1 -> 0 two frames after a full frame (one reset), 0 -> 1 (none);
//  - spread: steady, SpreadPasses off and on between publishes, a failed extra pass, a host reset whose stage-0
//    evaluate fails, and a codec resolve that fails after its model ran;
//  - the background mode: a rebuilt job's first pass, and the first pass after a host reset on a frame with no kick.
// The unspread warped path's declined transfer is not reachable through the core with these fakes
// (test_core_api_compute_transfer.cpp, RunBaseDecision); its frame ran the model, so the next one-frame motion matches,
// the case "TemporalMode 0 -> 1" checks below.
namespace coretest {
namespace {
OfpsSettingsValues Temporal(const OfpsSettingsValues &from, int mode, int passes, int spread, int every = 3) {
    auto settings = from;
    SetInt(settings, OFPS_SET_MODE, 0);
    SetInt(settings, OFPS_SET_TEMPORAL_MODE, mode);
    SetInt(settings, OFPS_SET_TEMPORAL_EVERY, every);
    SetInt(settings, OFPS_SET_MODEL_PASSES, passes);
    SetInt(settings, OFPS_SET_SPREAD_PASSES, spread);
    SetInt(settings, OFPS_SET_DEBUG_ASYNC_NO_REALTIME, 1); // GLOBAL_REALTIME without the privilege is a debug-layer error
    SetInt(settings, OFPS_SET_DEBUG_ASYNC_SHOW_PASS, 0);
    return settings;
}

struct Session {
    WarpDevice &w;
    IOfpsCore *core;
    HostFrame &frame;
    FakeModelHost &model;
    IOfpsFeature *feature = nullptr;

    bool Open(const char *what) {
        Check(CreateFeatureOnList(w, core, &model, &feature) == OFPS_OK && feature, std::string(what) + ": the feature creates");
        return feature != nullptr;
    }
    void Close() {
        feature->Release();
        DrainReleasedModels(w, core);
        Check(model.releaseCalls == model.createCalls && model.live.empty(), "history: every model is released");
    }
    void Set(const OfpsSettingsValues &settings) { Check(core->SetSettings(&settings) == OFPS_OK, "history: settings apply"); }
    EvalRun Step(std::uint32_t hostReset = 0, const OfpsFrameInputs *inputs = nullptr) {
        return RunEvaluate(w, core, feature, frame, frame.output.Get(), hostReset, inputs);
    }
    void Steps(int count) { for (int i = 0; i < count; ++i) Step(); }
    std::size_t Mark() const { return model.runs.size(); }
    std::string Runs(std::size_t from) const {
        std::string s;
        for (std::size_t i = from; i < model.runs.size(); ++i)
            s += (model.runs[i].handle == feature->CurrentModelHandle() ? "P" : "E") + std::to_string(model.runs[i].reset) + " ";
        return s;
    }
    void Expect(std::size_t from, const char *runs, const char *what) const {
        const std::string got = Runs(from);
        Check(got == runs, std::string("history: ") + what + " (runs \"" + got + "\", expected \"" + runs + "\")");
    }
};

void SinglePass(WarpDevice &w, IOfpsCore *core, HostFrame &frame, const OfpsSettingsValues &before) {
    FakeModelHost model;
    Session s{w, core, frame, model};
    s.Set(Temporal(before, 1, 1, 0));
    if (!s.Open("single pass")) return;
    std::size_t mark = s.Mark();
    s.Steps(10); // full frames 1, 4, 7, 10
    s.Expect(mark, "P1 P0 P0 P0 ", "ModelPasses 1, TemporalMode 1: only a new feature's first evaluate resets");
    s.Step(); // carried
    s.Set(Temporal(before, 0, 1, 0));
    mark = s.Mark();
    s.Steps(3);
    s.Expect(mark, "P1 P0 P0 ", "TemporalMode 1 -> 0 two frames after the last full frame: exactly one reset");
    s.Set(Temporal(before, 1, 1, 0));
    mark = s.Mark();
    s.Steps(4); // full, carried, carried, full
    s.Expect(mark, "P0 P0 ", "TemporalMode 0 -> 1: no reset (the feature ran on the previous frame)");
    s.Close();
}

void Spread(WarpDevice &w, IOfpsCore *core, HostFrame &frame, const OfpsSettingsValues &before) {
    FakeModelHost model;
    Session s{w, core, frame, model};
    s.Set(Temporal(before, 1, 2, 1));
    if (!s.Open("spread")) return;
    const EvalRun creation = s.Step();
    Check(creation.ok && creation.eval.path == OFPS_PATH_CREATION_FRAME && model.createCalls == 2 && model.runs.empty(),
          "history: the extra pass is created on an evaluate-free frame");
    void *const extra = model.lastHandle;
    std::size_t mark = s.Mark();
    s.Steps(6); // two cycles: stage 1, stage 2, idle
    s.Expect(mark, "P1 E1 P0 E0 ", "spread: new features reset once, the steady cycle never");

    s.Set(Temporal(before, 1, 2, 0)); // SpreadPasses off between publishes
    mark = s.Mark();
    s.Steps(4); // full, carried, carried, full
    s.Expect(mark, "P1 E1 P0 E0 ", "SpreadPasses off: both features last ran several frames back, so both reset once");

    s.Steps(2); // carried, carried
    model.failHandle = extra;
    mark = s.Mark();
    s.Step(); // full: pass 1 succeeds, the extra pass fails, no residual
    model.failHandle = nullptr;
    s.Step(); // the full frame again
    s.Expect(mark, "P0 E0 P1 E0 ", "a failed extra pass: pass 1 (history ahead of the chain) resets, the extra pass does not");

    s.Set(Temporal(before, 1, 2, 1)); // SpreadPasses on again
    mark = s.Mark();
    s.Steps(3);
    s.Expect(mark, "P0 E1 ", "SpreadPasses on: pass 1 ran on the previous frame and keeps its history, the extra pass resets");

    model.failHandle = s.feature->CurrentModelHandle();
    mark = s.Mark();
    s.Step(1); // a host reset: the cycle restarts with stage 1, whose evaluate fails
    model.failHandle = nullptr;
    s.Steps(2);
    s.Expect(mark, "P1 P1 E1 ", "a host reset whose stage-1 evaluate failed: stage 1 still resets on its next evaluate, then stage 2");
    s.Close();
}

// A host codec whose ResolveAnswer can fail after the model ran (the spread stage then records no residual).
struct ResolveFailHost : CodecHost {
    bool failResolve = false;
    int ResolveAnswer(ID3D12GraphicsCommandList *cmd, const OfpsResource *answer, const OfpsFrameInputs *frame) override {
        if (!failResolve) return CodecHost::ResolveAnswer(cmd, answer, frame);
        ++resolveCalls;
        return OFPS_E_DEVICE;
    }
};

void ResolveFailure(WarpDevice &w, IOfpsCore *core, HostFrame &frame, const OfpsSettingsValues &before) {
    ResolveFailHost model;
    Session s{w, core, frame, model};
    s.Set(Temporal(before, 1, 2, 1));
    Check(model.Initialize(w, kW, kH), "history: the codec host initializes");
    if (!s.Open("resolve failure")) return;
    s.Step(); // the extra pass's creation frame
    std::size_t mark = s.Mark();
    s.Steps(3);
    s.Expect(mark, "P1 E1 ", "codec spread: both stages start");
    model.failResolve = true;
    mark = s.Mark();
    s.Step(); // stage 1: the model runs, its answer does not resolve, no residual
    model.failResolve = false;
    s.Steps(2); // stage 1 again, stage 2
    s.Expect(mark, "P0 P1 E0 ", "a failed resolve: the stage's history moved past its chain, so its next evaluate resets");
    s.Close();
}

// Steps until the model has run `runs` times in all (or `limit` steps).
void StepUntil(Session &s, std::size_t runs, int limit, const OfpsFrameInputs *inputs = nullptr) {
    for (int i = 0; i < limit && s.model.runs.size() < runs; ++i) s.Step(0, inputs);
}

void Background(WarpDevice &w, IOfpsCore *core, HostFrame &frame, const OfpsSettingsValues &before) {
    FakeModelHost model;
    Session s{w, core, frame, model};
    s.Set(Temporal(before, 3, 1, 0, 4));
    const auto ui = CreateTexture(w.device.Get(), kW, kH, kColorFormat, D3D12_RESOURCE_FLAG_NONE, kInputRest);
    if (!ui || !s.Open("background")) return;
    StepUntil(s, 3, 24); // three passes kicked
    s.Expect(0, "P1 P0 P0 ", "background: a new feature's first pass resets, the next ones follow the pending chain");
    s.Steps(2); // no kick: the job's last pass is two frames back
    // A UI input from now on: the job's private copies no longer match, so it is rebuilt and its chains dropped.
    OfpsFrameInputs in = FrameInputs(frame, frame.output.Get(), 0);
    in.ui = Resource(ui.Get(), kColorFormat, {0, 0, kW, kH}, kInputRest);
    const std::size_t mark = s.Mark();
    StepUntil(s, 5, 24, &in);
    s.Expect(mark, "P1 P0 ", "background: a rebuilt job's first pass resets once");
    // A host reset on a frame that kicks nothing: the reset reaches no model, the chains drop, and the next pass's
    // one-frame vectors do not reach back to the last pass: it resets.
    OfpsFrameInputs reset = in;
    reset.hostReset = 1;
    s.Step(0, &in); // no kick (one frame after a pass)
    const std::size_t resetMark = s.Mark();
    s.Step(0, &reset); // no kick either
    Check(s.Mark() == resetMark, "history: the host reset frame kicks no pass");
    StepUntil(s, resetMark + 2, 24, &in);
    s.Expect(resetMark, "P1 P0 ", "background: after a host reset on a frame with no kick, the next pass resets once");
    const OfpsStatus status = CurrentStatus(core);
    Check(status.temporalMode == 3, "history: the passes ran in the background mode");
    s.Close();
}
} // namespace

void ScenarioModelHistory(WarpDevice &w, IOfpsCore *core, FakeHost &, HostFrame &frame) {
    const auto before = CurrentSettings(core);
    SinglePass(w, core, frame, before);
    Spread(w, core, frame, before);
    ResolveFailure(w, core, frame, before);
    Background(w, core, frame, before);
    Check(core->SetSettings(&before) == OFPS_OK, "history: the previous settings are restored");
}
} // namespace coretest
