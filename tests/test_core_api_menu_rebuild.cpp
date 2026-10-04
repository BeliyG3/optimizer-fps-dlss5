#include "test_core_api_scenarios.h"
#include "test_core_api_checks.h"

// Menu mode, Task 12 fix round 1: a menu frame through the core never creates a model (ModelHostNgx refuses; the fake
// model host's refuseCreate here). The core has already committed the new layout or model-pass count when the creation
// is refused, so without more its next evaluate would not retry (the control checks below). Menu mode then calls
// RequestModelRebuild, and the game's next evaluate re-creates the model and its extra passes.
namespace coretest {
namespace {
void SetLayout(IOfpsCore *core, int mode, int passes) {
    OfpsSettingsValues v = CurrentSettings(core);
    SetInt(v, OFPS_SET_MODE, mode);
    SetInt(v, OFPS_SET_MODEL_PASSES, passes);
    SetInt(v, OFPS_SET_TEMPORAL_MODE, 0);
    SetFloat(v, OFPS_SET_CENTER_X, 80.0f);
    SetFloat(v, OFPS_SET_CENTER_Y, 80.0f);
    SetFloat(v, OFPS_SET_WORK_X, 90.0f);
    SetFloat(v, OFPS_SET_WORK_Y, 90.0f);
    Check(core->SetSettings(&v) == OFPS_OK, "menu rebuild: the layout is set");
}

// A few host evaluates: creation frames settle.
EvalRun Settle(WarpDevice &w, IOfpsCore *core, IOfpsFeature *feature, HostFrame &frame) {
    EvalRun run;
    for (int i = 0; i < 3; ++i) run = RunEvaluate(w, core, feature, frame, frame.output.Get(), 0);
    return run;
}
} // namespace

void ScenarioMenuRebuild(WarpDevice &w, IOfpsCore *core, HostFrame &frame) {
    const OfpsSettingsValues saved = CurrentSettings(core);
    SetLayout(core, 2, 1);
    FakeModelHost model;
    IOfpsFeature *feature = nullptr;
    Check(CreateFeatureOnList(w, core, &model, &feature) == OFPS_OK && feature, "menu rebuild: the feature is created");
    if (!feature) { core->SetSettings(&saved); return; }
    EvalRun run = RunEvaluate(w, core, feature, frame, frame.output.Get(), 1);
    const std::uint32_t warpedW = model.lastCreate.w;
    Check(run.ok && model.createCalls == 1 && warpedW < kW, "menu rebuild: the host's evaluate runs a compressed model");

    // Leg 1: the extent changes (Peripheral -> Off) and the core's creation is refused, as inside a menu call.
    SetLayout(core, 0, 1);
    model.refuseCreate = true;
    run = RunEvaluate(w, core, feature, frame, frame.output.Get(), 0);
    model.refuseCreate = false;
    Check(model.refusedCreates == 1 && model.createCalls == 1, "menu rebuild: the refused creation made no model");
    const std::uint32_t runsBefore = model.runCalls;
    run = RunEvaluate(w, core, feature, frame, frame.output.Get(), 0);
    Check(model.createCalls == 1 && model.runCalls == runsBefore,
          "menu rebuild (control): without a rebuild request the core's next evaluate does not retry the creation");
    feature->RequestModelRebuild(); // what menu_core_pass.cpp does after a refused creation
    run = Settle(w, core, feature, frame);
    Check(run.ok && run.result == OFPS_OK && model.createCalls == 2 && model.lastCreate.w == kW && model.lastCreate.h == kH &&
              model.runCalls > runsBefore,
          "menu rebuild: after the request the game's next evaluate creates the model at the new extent and runs it");

    // Leg 2: the model-pass count changes (1 -> 2) and the extra pass's creation is refused.
    SetLayout(core, 2, 1);
    run = Settle(w, core, feature, frame);
    Check(run.ok && CurrentStatus(core).modelPassesRunning == 1, "menu rebuild: back to one compressed model");
    const std::uint32_t created = model.createCalls, refused = model.refusedCreates;
    SetLayout(core, 2, 2);
    model.refuseCreate = true;
    run = RunEvaluate(w, core, feature, frame, frame.output.Get(), 0);
    model.refuseCreate = false;
    Check(model.refusedCreates > refused && model.createCalls == created, "menu rebuild: the extra pass's creation was refused");
    run = Settle(w, core, feature, frame);
    Check(model.createCalls == created && CurrentStatus(core).modelPassesRunning == 1,
          "menu rebuild (control): without a rebuild request the extra pass is never created");
    feature->RequestModelRebuild();
    // One evaluate: the re-created model and the extra pass are created on a setup frame (no model run). The fake model
    // host does not model two passes' GPU work, so the count goes back to one before any further evaluate.
    run = RunEvaluate(w, core, feature, frame, frame.output.Get(), 0);
    Check(run.ok && run.eval.path == OFPS_PATH_CREATION_FRAME && model.createCalls == created + 2 &&
              CurrentStatus(core).modelPassesRunning == 2,
          "menu rebuild: after the request the game's next evaluate re-creates the model and its extra pass");
    SetLayout(core, 2, 1);
    run = Settle(w, core, feature, frame);
    Check(run.ok && run.result == OFPS_OK, "menu rebuild: one pass again");

    feature->Release();
    DrainReleasedModels(w, core);
    Check(model.live.empty(), "menu rebuild: every model is released");
    core->SetSettings(&saved);
}
} // namespace coretest
