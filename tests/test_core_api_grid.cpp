#include "test_core_api_scenarios.h"
#include "test_core_api_checks.h"
#include "test_core_api_grid_model.h"

#include <cstring>
#include <iostream>

// TemporalGrid (2026.10.1) on the background route, through the core API, proven by the frames it writes. A warped
// TemporalMode 3 carried frame gets the model's grid from the scheduler's own carried frame (async_scheduler.cpp,
// tinOut); an unwarped one keeps the native reprojection.
//
// The model's edit is a one-texel checker (test_core_api_grid_model.h) and the host's vectors are zero, so between two
// kicks (N = 8) every carried frame of an adopted pass is the same image: colour + the reprojected residual. On such a
// steady frame TemporalGrid is switched on for one frame and off for the next, both frames kicking nothing and carried
// by the background route; on those frames tinOut is the only caller of the grid (the kick's WarpedBody does not run,
// and the synchronous InterpolateBody would clear the background status reason).

namespace coretest {
namespace {
constexpr const char *kWarped = "(model in the background, warped)";
constexpr const char *kUnwarped = "(model in the background, no warp)";

// Peripheral 80/90 (or Off) at Global scale 50: the model's density 0.5, a 320x180 grid for the 640x360 frame.
OfpsSettingsValues GridSettings(const OfpsSettingsValues &from, int mode, int grid) {
    auto settings = from;
    SetInt(settings, OFPS_SET_MODE, mode);
    SetInt(settings, OFPS_SET_COLOR_FILTER, 2);
    SetFloat(settings, OFPS_SET_CENTER_X, 80.0f);
    SetFloat(settings, OFPS_SET_CENTER_Y, 80.0f);
    SetFloat(settings, OFPS_SET_WORK_X, 90.0f);
    SetFloat(settings, OFPS_SET_WORK_Y, 90.0f);
    SetFloat(settings, OFPS_SET_GLOBAL_SCALE, 50.0f);
    SetInt(settings, OFPS_SET_TEMPORAL_MODE, 3);
    SetInt(settings, OFPS_SET_TEMPORAL_EVERY, 8); // a kick every 8th frame: frames 1..7 after it kick nothing
    SetInt(settings, OFPS_SET_TEMPORAL_GRID, grid);
    SetInt(settings, OFPS_SET_DEBUG_WARP_PATH, 1);
    SetInt(settings, OFPS_SET_DEBUG_ASYNC_NO_REALTIME, 1); // GLOBAL_REALTIME without the privilege is a debug-layer error
    SetInt(settings, OFPS_SET_DEBUG_ASYNC_SHOW_PASS, 0);   // the reprojection, not the last pass as is
    SetInt(settings, OFPS_SET_DEBUG_TEMPORAL_PHASE_IN, 0); // an adopted pass is shown at once
    return settings;
}

struct Frame {
    EvalRun run;
    bool kicked = false;
    std::vector<float> rgb;
};

struct Session {
    WarpDevice &w;
    IOfpsCore *core;
    HostFrame &frame;
    CheckerModelHost &model;
    IOfpsFeature *feature;
    ID3D12Resource *still; // zero motion vectors

    Frame Next(bool reset) {
        const auto runs = model.runCalls;
        OfpsFrameInputs in = FrameInputs(frame, frame.output.Get(), reset ? 1u : 0u);
        in.motion = Resource(still, DXGI_FORMAT_R16G16_FLOAT, in.motion.rect, kInputRest);
        Frame f{RunEvaluate(w, core, feature, frame, frame.output.Get(), 0, &in)};
        f.kicked = model.runCalls > runs;
        f.rgb = ReadRgb(f.run.output);
        return f;
    }
    // A frame that kicks nothing and is carried by the background route.
    bool Carried(const Frame &f, const char *reason) const {
        const OfpsStatus s = CurrentStatus(core);
        return f.run.ok && f.run.result == OFPS_OK && !f.kicked && f.run.eval.path == OFPS_PATH_CARRIED &&
               s.temporalMode == 3 && s.reason != nullptr && std::strstr(s.reason, reason) != nullptr;
    }
    bool Same(const Frame &a, const Frame &b) const {
        const Difference d = Compare(a.rgb, b.rgb);
        return d.valid && d.pixels == 0;
    }
};

// Grid off. Carried frames until two in a row are identical at most 5 frames after a kick, so frames 6 and 7 (no
// kick, no adoption: the pass was adopted before the steady pair) are left for the switch.
bool FindSteady(Session &s, const char *reason, Frame &steady) {
    int since = -1;
    Frame previous;
    for (int i = 0; i < 48; ++i) {
        Frame f = s.Next(false);
        if (f.kicked) {
            since = 0;
            previous = {};
            continue;
        }
        if (since < 0) continue;
        ++since;
        const bool carried = s.Carried(f, reason);
        if (carried && since <= 5 && !previous.rgb.empty() && s.Same(f, previous)) {
            steady = f;
            return true;
        }
        previous = carried ? f : Frame{};
    }
    return false;
}

void CheckDifference(bool ok, const char *message, const Difference &d) {
    Check(ok, message);
    if (!ok) std::cerr << "  differing pixels " << d.pixels << ", max " << d.max << ", mean " << d.mean << '\n';
}

// One feature: warm-up, a steady carried frame with the grid off, then one frame with it on and one with it off again.
struct Switched {
    bool ok = false;
    Frame off, on, back;
};
Switched Run(WarpDevice &w, IOfpsCore *core, HostFrame &frame, const OfpsSettingsValues &before, int mode,
             const char *reason) {
    Switched result;
    auto settings = GridSettings(before, mode, 0);
    CheckerModelHost model;
    const auto still = CreateTexture(w.device.Get(), kW, kH, DXGI_FORMAT_R16G16_FLOAT,
                                     D3D12_RESOURCE_FLAG_NONE, kInputRest); // committed: zero-initialised
    IOfpsFeature *feature = nullptr;
    Check(core->SetSettings(&settings) == OFPS_OK && still && model.Initialize(w),
          "grid scenario: settings, still vectors and the checker model");
    Check(CreateFeatureOnList(w, core, &model, &feature) == OFPS_OK && feature, "grid scenario: the feature creates");
    if (!feature || !still) return result;
    Session s{w, core, frame, model, feature, still.Get()};
    for (int i = 0; i < 20; ++i) s.Next(i == 0);
    const bool steady = FindSteady(s, reason, result.off);
    Check(steady, "grid scenario: carried frames reach a steady image between two kicks");
    if (steady) {
        settings = GridSettings(before, mode, 1);
        Check(core->SetSettings(&settings) == OFPS_OK, "grid scenario: TemporalGrid on between frames");
        result.on = s.Next(false);
        const bool onCarried = s.Carried(result.on, reason);
        settings = GridSettings(before, mode, 0);
        Check(core->SetSettings(&settings) == OFPS_OK, "grid scenario: TemporalGrid off between frames");
        result.back = s.Next(false);
        result.ok = onCarried && s.Carried(result.back, reason);
        Check(result.ok, "grid scenario: both switched frames kick nothing and are carried by the background route");
    }
    feature->Release();
    DrainReleasedModels(w, core);
    return result;
}

// Warped: the steady frame is the colour plus an adopted pass's residual; with the grid on the same addition comes
// from the model's grid (it differs from the native route, by no more than the addition itself); off again, the native
// route's frame returns byte for byte.
void Warped(WarpDevice &w, IOfpsCore *core, HostFrame &frame, const OfpsSettingsValues &before) {
    const Switched r = Run(w, core, frame, before, 2, kWarped);
    if (!r.ok) return;
    const Difference added = Compare(r.off.rgb, SourceRgb());
    CheckDifference(added.valid && added.pixels > kW * kH / 4 && added.mean > 0.01f,
                    "grid scenario: a warped carried frame is the colour plus the reprojected residual of an adopted pass",
                    added);
    const Difference grid = Compare(r.on.rgb, r.off.rgb);
    CheckDifference(grid.valid && grid.pixels > kW * kH / 100 && grid.mean > 1.0e-4f && grid.max <= 2.0f * added.max + 1.0e-3f,
                    "grid scenario: TemporalGrid on, the warped carried frame takes its addition from the model's grid",
                    grid);
    const Difference back = Compare(r.back.rgb, r.off.rgb);
    CheckDifference(back.valid && back.pixels == 0,
                    "grid scenario: TemporalGrid off again, the native route's frame returns byte for byte", back);
}

// Unwarped (Mode Off): the same switch changes nothing - the carried frame stays on the native reprojection.
void Unwarped(WarpDevice &w, IOfpsCore *core, HostFrame &frame, const OfpsSettingsValues &before) {
    const Switched r = Run(w, core, frame, before, 0, kUnwarped);
    if (!r.ok) return;
    const Difference added = Compare(r.off.rgb, SourceRgb());
    CheckDifference(added.valid && added.pixels > kW * kH / 4 && added.mean > 0.01f,
                    "grid scenario: an unwarped carried frame is the colour plus the reprojected residual", added);
    const Difference on = Compare(r.on.rgb, r.off.rgb), back = Compare(r.back.rgb, r.off.rgb);
    CheckDifference(on.valid && on.pixels == 0, "grid scenario: TemporalGrid on leaves an unwarped carried frame native", on);
    CheckDifference(back.valid && back.pixels == 0, "grid scenario: and off again as well", back);
}
} // namespace

void ScenarioTemporalGrid(WarpDevice &w, IOfpsCore *core, FakeHost &, HostFrame &frame) {
    const auto before = CurrentSettings(core);
    Warped(w, core, frame, before);
    Unwarped(w, core, frame, before);
    Check(core->SetSettings(&before) == OFPS_OK, "grid scenario: the previous settings are restored");
}

} // namespace coretest
