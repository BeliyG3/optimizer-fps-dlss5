#include "test_core_api_checks.h"
#include "ofps_version.h"
#include "test_core_api_entry.h"
#include "test_core_api_fakes.h"
#include "test_core_api_scenarios.h"
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <unordered_set>
namespace {
using namespace coretest;
bool ShadersBesideCoreModule() {
    wchar_t path[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(api.module, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
        return false;
    std::wstring dir(path, n);
    dir.erase(dir.find_last_of(L'\\') + 1);
    const std::wstring probe = dir + L"optimizer-fps-dlss5\\pack_ps.dxbc";
    if (GetFileAttributesW(probe.c_str()) != INVALID_FILE_ATTRIBUTES)
        return true;
    std::wcerr << L"FAIL: " << probe
               << L" is missing (the build's shaders must be beside the core module)\n";
    return false;
}
IOfpsCore *ScenarioVersionAndCreate(FakeHost &hostA, FakeHost &hostB) {
    OfpsVersion v{};
    v.size = sizeof(v);
    api.version(&v);
    Check(v.abi == OFPS_ABI_VERSION, "OfpsCoreVersion reports the ABI this test was built against");
    Check(std::strcmp(v.release, OFPS_ADDON_VERSION_STRING) == 0, "OfpsCoreVersion reports the release of this build");
    IOfpsCore *core = nullptr;
    Check(api.create(OFPS_ABI_VERSION + 1u, &hostA, &core) == OFPS_E_ABI && core == nullptr,
          "another ABI version is refused with OFPS_E_ABI");
    Check(api.create(OFPS_ABI_VERSION, &hostA, &core) == OFPS_OK && core != nullptr,
          "the first host creates the core");
    IOfpsCore *again = nullptr;
    Check(api.create(OFPS_ABI_VERSION, &hostB, &again) == OFPS_S_EXISTING && again == core,
          "the second host gets the same core with OFPS_S_EXISTING");
    return core;
}
void ScenarioSettings(IOfpsCore *core, FakeHost &host) {
    OfpsSettingsValues v = CurrentSettings(core);
    Check(v.count == OFPS_SET_COUNT, "GetSettings answers every schema id");
    const std::uint32_t before = host.events[OFPS_EVENT_SETTINGS_CHANGED];
    SetInt(v, OFPS_SET_TEMPORAL_EVERY, 99);
    SetInt(v, OFPS_SET_DEBUG_WARP_PATH, 2);
    SetFloat(v, OFPS_SET_CENTER_X, 80.0f);
    Check(core->SetSettings(&v) == OFPS_OK, "SetSettings accepts an out-of-range TemporalEvery (clamped, not refused)");
    const OfpsSettingsValues after = CurrentSettings(core);
    Check(after.v[OFPS_SET_TEMPORAL_EVERY].i == 8, "TemporalEvery 99 is clamped to 8 in GetSettings");
    Check(host.events[OFPS_EVENT_SETTINGS_CHANGED] == before + 1, "one SETTINGS_CHANGED event per SetSettings");
    Check(host.lastSettings.count == OFPS_SET_COUNT && host.lastSettings.v[OFPS_SET_TEMPORAL_EVERY].i == 8,
          "the SETTINGS_CHANGED payload carries the effective (clamped) values");
    float lo = 0.0f, hi = 0.0f;
    core->GetSettingRange(OFPS_SET_OFFSET_X, &lo, &hi);
    Check(Near(lo, -9.5f, 1.0e-3f) && Near(hi, 9.5f, 1.0e-3f), "GetSettingRange(OffsetX) at Center 80 is -9.5..9.5");
}
IOfpsFeature *ScenarioPassthrough(WarpDevice &w, IOfpsCore *core, FakeHost &host, FakeModelHost &m, HostFrame &f) {
    OfpsSettingsValues v = CurrentSettings(core);
    SetInt(v, OFPS_SET_MODE, 0);
    SetInt(v, OFPS_SET_COLOR_FILTER, 0);
    Check(core->SetSettings(&v) == OFPS_OK, "Mode Off is accepted");
    IOfpsFeature *feature = nullptr;
    Check(CreateFeatureOnList(w, core, &m, &feature) == OFPS_OK && feature != nullptr,
          "CreateFeature in Mode Off succeeds");
    if (feature == nullptr)
        return nullptr;
    Check(m.createCalls == 1 && m.lastCreate.w == kW && m.lastCreate.h == kH && m.lastCreate.withholdUi == 0,
          "Mode Off creates the model at the frame size with the UI inputs kept");
    Check(feature->CurrentModelHandle() == m.lastHandle, "CurrentModelHandle is the handle the model host returned");
    Check(host.events[OFPS_EVENT_FEATURE_CREATED] == 1 && host.createdHandle == m.lastHandle,
          "FEATURE_CREATED carries the host's handle");
    Check(host.LogContains("feature 18 created, native 640x360, model 640x360 (pass-through)"),
          "the creation log line names the pass-through model");
    const EvalRun run = RunEvaluate(w, core, feature, f, f.output.Get(), 1);
    Check(run.ok && run.result == OFPS_OK, "Evaluate in Mode Off returns OFPS_OK");
    Check(run.eval.path == OFPS_PATH_PASSTHROUGH && run.eval.warpPath == OFPS_WARP_NONE,
          "Mode Off evaluates as PASSTHROUGH without a warp path");
    Check(m.runCalls == 1 && m.endFrameCalls == 1,
          "the pass-through frame runs the model once and ends the frame once");
    Check(m.lastInputs.color.res == f.color.Get() && m.lastInputs.output.res == f.output.Get() &&
              m.lastInputs.width == kW && m.lastInputs.height == kH && SameRect(m.lastInputs.color.rect, 0, 0, kW, kH),
          "the pass-through model reads the host's colour and writes the host's output on the frame grid");
    Check(MatchesSource(run.output), "Mode Off output equals the colour exactly (the fake model copied it)");
    const OfpsStatus s = CurrentStatus(core);
    Check(s.featureCreated == 1 && s.active == 0 && s.passthroughs == 1 && s.evaluations == 0,
          "Status counts the frame as a pass-through");
    Check(host.events[OFPS_EVENT_FIRST_WARPED_FRAME] == 0, "no FIRST_WARPED_FRAME event before a warped frame");
    return feature;
}
void ScenarioWarped(WarpDevice &w, IOfpsCore *core, FakeHost &host, FakeModelHost &m, HostFrame &f,
                    IOfpsFeature *feature) {
    OfpsSettingsValues v = CurrentSettings(core);
    SetInt(v, OFPS_SET_MODE, 2);
    SetFloat(v, OFPS_SET_CENTER_X, 80.0f);
    SetFloat(v, OFPS_SET_CENTER_Y, 80.0f);
    SetFloat(v, OFPS_SET_WORK_X, 90.0f);
    SetFloat(v, OFPS_SET_WORK_Y, 90.0f);
    Check(core->SetSettings(&v) == OFPS_OK, "Mode Peripheral 80/90 is accepted");
    const EvalRun run = RunEvaluate(w, core, feature, f, f.output.Get(), 0);
    Check(run.ok && run.result == OFPS_OK, "the first Peripheral evaluate returns OFPS_OK");
    Check(run.eval.path == OFPS_PATH_WARPED && run.eval.warpPath == OFPS_WARP_PIXEL && run.eval.modelResult == OFPS_OK,
          "Mode Peripheral evaluates as WARPED on the pixel path");
    Check(m.createCalls == 2 && m.lastCreate.w == kWorkW && m.lastCreate.h == kWorkH && m.lastCreate.withholdUi == 1,
          "the layout re-creates the model at 576x324 with the UI withheld");
    Check(feature->CurrentModelHandle() == m.lastHandle && m.lastRunHandle == m.lastHandle,
          "CurrentModelHandle follows the re-created model");
    Check(host.LogContains("GPU path ready") && host.LogContains("layout changed, model re-created at 576x324"),
          "the GPU path and the re-creation are logged with the fixed lines");
    Check(m.runCalls == 2 && m.endFrameCalls == 2, "the warped frame runs the model once and ends the frame");
    const OfpsModelInputs &mi = m.lastInputs;
    Check(mi.width == kWorkW && mi.height == kWorkH && SameRect(mi.color.rect, 0, 0, kWorkW, kWorkH) &&
              SameRect(mi.output.rect, 0, 0, kWorkW, kWorkH),
          "the model runs on the packed grid");
    Check(mi.color.res != nullptr && mi.color.res != f.color.Get() && mi.output.res != nullptr &&
              mi.output.res != f.output.Get(),
          "the model reads the packed colour and writes the core's work output, not the host's textures");
    Check(mi.withholdUi == 1 && mi.ui.res == nullptr && mi.uiAlpha.res == nullptr && mi.backbuffer.res == nullptr,
          "UI inputs are withheld from the warped model");
    Check(mi.reset == 1, "the first frame of a re-created model carries reset");
    Check(MatchesSdkReference(run.output), "every warped pixel matches SDK Unpack(Pack(colour))");
    Check(coretest::MatchesImage(m.colorCapture, coretest::Reference{}.packed, kWorkW, kWorkH),
          "every model colour pixel matches SDK Pack");
    Check(coretest::MatchesPackedGuides(m.depthCapture, m.motionCapture), "packed depth and motion match SDK");
    Check(mi.depth.res && mi.depth.res != f.depth.Get() && mi.motion.res && mi.motion.res != f.motion.Get() &&
              SameRect(mi.depth.rect, 0, 0, kWorkW, kWorkH) && SameRect(mi.motion.rect, 0, 0, kWorkW, kWorkH),
          "depth and motion use the packed grid");
    Check(mi.color.restState == coretest::kInputRest && mi.depth.restState == coretest::kInputRest &&
              mi.motion.restState == coretest::kInputRest && mi.output.restState == coretest::kOutputRest &&
              mi.mvScaleX == 1.0f && mi.mvScaleY == 1.0f,
          "model resource states and packed-pixel motion scales are explicit");
    Check(host.events[OFPS_EVENT_FIRST_WARPED_FRAME] == 1,
          "FIRST_WARPED_FRAME fired once on the first warped evaluate");
    Check(host.LogContains("first warped evaluate completed (model 576x324)"),
          "the first warped evaluate is logged with the fixed line");
    const OfpsStatus s = CurrentStatus(core);
    Check(s.active == 1 && s.evaluations == 1 && s.nativeW == kW && s.nativeH == kH && s.workW == kWorkW &&
              s.workH == kWorkH && s.warpPath == OFPS_WARP_PIXEL,
          "Status reports the warped feature's extents and path");
    Check(s.reason != nullptr && s.reason[0] == 0, "Status.reason is empty after a warped frame");
    OfpsStatusRow warpRows[64] = {};
    const auto warpRowCount = core->StatusLines(warpRows, 64);
    bool pixelReason = false;
    for (std::uint32_t i = 0; i < warpRowCount; ++i)
        if (std::strcmp(warpRows[i].label, "Warp path") == 0 &&
            std::strstr(warpRows[i].value, "forced by DebugWarpPath") &&
            std::strstr(warpRows[i].value, "graphics state on the host's list is not restored"))
            pixelReason = true;
    Check(pixelReason, "StatusLines explains the forced pixel path and graphics-state contract");
    const EvalRun second = RunEvaluate(w, core, feature, f, f.output.Get(), 0);
    Check(second.ok && second.eval.path == OFPS_PATH_WARPED && m.runCalls == 3 && m.lastInputs.reset == 0,
          "the second warped frame runs the model without reset");
    Check(host.events[OFPS_EVENT_FIRST_WARPED_FRAME] == 1, "FIRST_WARPED_FRAME does not fire again");
}
void ScenarioStatusAndPreview(IOfpsCore *core) {
    OfpsStatusRow rows[64] = {};
    const std::uint32_t n = core->StatusLines(rows, 64);
    Check(n > 0 && n <= 64, "StatusLines returns at least one row within the capacity");
    bool rowsOk = true;
    for (std::uint32_t i = 0; i < n && i < 64; ++i)
        rowsOk = rowsOk && rows[i].label != nullptr && rows[i].value != nullptr && rows[i].group < OFPS_GROUP_COUNT &&
                 rows[i].severity <= 2;
    Check(rowsOk, "every status row has a label, a value, a schema group and a severity of 0..2");
    OfpsLayoutPreview p{};
    p.size = sizeof(p);
    core->LayoutPreview(&p);
    Check(p.nativeW == kW && p.nativeH == kH && p.rawWorkW == kWorkW && p.rawWorkH == kWorkH && p.modelW == kWorkW &&
              p.modelH == kWorkH,
          "LayoutPreview reports the frame, raw Work and model extents");
    Check(p.frame.w > 0.0f && p.frame.h > 0.0f && Inside(p.center, p.frame) && Inside(p.rawWork, p.frame) &&
              Inside(p.center, p.rawWork),
          "the centre band lies inside raw Work, which lies inside the frame");
    Check(Near(p.center.w, 0.8f * p.frame.w, 1.0e-3f) && Near(p.rawWork.w, 0.9f * p.frame.w, 1.0e-3f),
          "the centre band is 80 % and raw Work 90 % of the frame width");
}
void ScenarioPaddedOutput(WarpDevice &w, IOfpsCore *core, FakeHost &host, HostFrame &f, IOfpsFeature *feature) {
    const EvalRun run = RunEvaluate(w, core, feature, f, f.outputPadded.Get(), 0);
    Check(run.ok && run.result == OFPS_OK && run.eval.path == OFPS_PATH_WARPED,
          "a frame-sized region at 0,0 of a larger output texture is warped");
    Check(host.events[OFPS_EVENT_HOST_SHAPE_REJECTED] == 0, "the padded output is not a shape rejection");
    Check(MatchesSdkReference(run.output), "every pixel in the padded output region matches SDK");
    Check(PixelIs(run.output, kW, kH - 1, coretest::kMarker, 1.0e-6f) &&
              PixelIs(run.output, kW - 1, kH, coretest::kMarker, 1.0e-6f) &&
              PixelIs(run.output, 800, 100, coretest::kMarker, 1.0e-6f) &&
              PixelIs(run.output, 100, 450, coretest::kMarker, 1.0e-6f),
          "the padding around the region keeps its marker");
}
void ScenarioForeign(WarpDevice &w, IOfpsCore *core, FakeHost &host, FakeModelHost &m3, HostFrame &f,
                     IOfpsFeature *first) {
    FakeModelHost m2;
    IOfpsFeature *second = reinterpret_cast<IOfpsFeature *>(static_cast<std::uintptr_t>(1));
    Check(CreateFeatureOnList(w, core, &m2, &second) == OFPS_S_FOREIGN && second == nullptr,
          "a second CreateFeature while a warped feature lives is OFPS_S_FOREIGN with a null feature");
    Check(m2.createCalls == 0 && host.events[OFPS_EVENT_FEATURE_CREATED] == 1,
          "the foreign feature reaches neither its model host nor the events");
    const std::uint32_t releasedBefore = host.events[OFPS_EVENT_FEATURE_RELEASED];
    void *const handle = host.createdHandle;
    first->Release();
    Check(host.events[OFPS_EVENT_FEATURE_RELEASED] == releasedBefore, "release is deferred until retirement");
    DrainReleasedModels(w, core);
    Check(host.events[OFPS_EVENT_FEATURE_RELEASED] == releasedBefore + 1 && host.releasedHandle == handle,
          "Release sends FEATURE_RELEASED with the host's handle");
    const auto created = std::find(host.eventOrder.begin(), host.eventOrder.end(), OFPS_EVENT_FEATURE_CREATED);
    const auto warped = std::find(host.eventOrder.begin(), host.eventOrder.end(), OFPS_EVENT_FIRST_WARPED_FRAME);
    const auto released = std::find(host.eventOrder.begin(), host.eventOrder.end(), OFPS_EVENT_FEATURE_RELEASED);
    Check(created < warped && warped < released && released != host.eventOrder.end(),
          "events preserve FEATURE_CREATED -> FIRST_WARPED_FRAME -> FEATURE_RELEASED order");
    Check(CurrentStatus(core).featureCreated == 0, "Status shows no feature after the release");
    IOfpsFeature *third = nullptr;
    Check(CreateFeatureOnList(w, core, &m3, &third) == OFPS_OK && third != nullptr,
          "after the warped feature is released a new CreateFeature succeeds");
    Check(m3.createCalls == 1 && m3.lastCreate.w == kWorkW && m3.lastCreate.h == kWorkH &&
              m3.lastCreate.withholdUi == 1,
          "the new feature is created warped straight away");
    Check(host.LogContains("feature 18 created, native 640x360, model 576x324 (warped)"),
          "the creation log line names the warped model");
    if (third == nullptr)
        return;
    const EvalRun run = RunEvaluate(w, core, third, f, f.output.Get(), 1);
    Check(run.ok && run.eval.path == OFPS_PATH_WARPED && m3.runCalls == 1 && MatchesSdkReference(run.output),
          "the new feature warps");
    third->Release();
    DrainReleasedModels(w, core);
}
void ScenarioDeferred(WarpDevice &w, IOfpsCore *core, FakeModelHost &m, HostFrame &f) {
    m.createResult = OFPS_S_MODEL_NEXT_FRAME;
    m.ready = 0;
    IOfpsFeature *feature = nullptr;
    Check(CreateFeatureOnList(w, core, &m, &feature) == OFPS_OK && feature != nullptr,
          "CreateFeature succeeds when CreateModel answers OFPS_S_MODEL_NEXT_FRAME");
    if (feature == nullptr)
        return;
    Check(m.createCalls == 1 && m.lastCreate.w == kWorkW && m.lastCreate.h == kWorkH,
          "the deferred model is created at the work extent");
    const EvalRun first = RunEvaluate(w, core, feature, f, f.output.Get(), 1);
    Check(first.ok && first.result == OFPS_OK && first.eval.path == OFPS_PATH_CREATION_FRAME &&
              first.eval.modelResult == 0,
          "the first evaluate of a not-yet-ready model is a CREATION_FRAME");
    Check(m.runCalls == 0 && m.endFrameCalls == 1, "the creation frame does not run the model but ends the frame");
    Check(MatchesSource(first.output), "the creation frame shows the host's colour in its output");
    const EvalRun still = RunEvaluate(w, core, feature, f, f.output.Get(), 0);
    Check(still.ok && still.eval.path == OFPS_PATH_CREATION_FRAME && m.runCalls == 0 && m.readyCalls >= 1,
          "while ModelReady answers 0 every evaluate stays a creation frame and asks again");
    m.ready = 1;
    const EvalRun ready = RunEvaluate(w, core, feature, f, f.output.Get(), 0);
    Check(ready.ok && ready.eval.path == OFPS_PATH_WARPED && ready.eval.warpPath == OFPS_WARP_PIXEL,
          "once ModelReady answers 1 the next evaluate is WARPED");
    Check(m.runCalls == 1 && m.lastInputs.reset == 1,
          "the first full frame after the creation frame runs the model with reset");
    Check(MatchesSdkReference(ready.output), "the deferred model's first warped frame carries the colour");
    const EvalRun next = RunEvaluate(w, core, feature, f, f.output.Get(), 0);
    Check(next.ok && next.eval.path == OFPS_PATH_WARPED && m.runCalls == 2 && m.lastInputs.reset == 0,
          "the following frame runs without reset");
    feature->Release();
    DrainReleasedModels(w, core);
}
void ScenarioFailureAndUnfit(WarpDevice &w, IOfpsCore *core, FakeHost &host, HostFrame &f) {
    FakeModelHost model;
    IOfpsFeature *feature = nullptr;
    Check(CreateFeatureOnList(w, core, &model, &feature) == OFPS_OK && feature, "failure test feature");
    if (!feature)
        return;
    model.runResult = OFPS_E_STATE;
    const auto failed = RunEvaluate(w, core, feature, f, f.output.Get(), 0);
    Check(failed.ok && failed.result == OFPS_E_STATE && failed.eval.modelResult == OFPS_E_STATE &&
              failed.eval.path == OFPS_PATH_FALLBACK && failed.eval.warpPath == OFPS_WARP_NONE,
          "RunModel failure is reported as fallback with its actual result");
    Check(model.runCalls == 1 && model.endFrameCalls == 1 && MatchesSource(failed.output),
          "failed RunModel still writes host colour to Output and ends the frame");
    const auto failureStatus = CurrentStatus(core);
    Check(failureStatus.fallbackReason && failureStatus.fallbackReason[0], "RunModel failure has a fallback reason");
    model.runResult = OFPS_OK;
    f.outputRect = {32, 16, kW, kH};
    const auto rejected = RunEvaluate(w, core, feature, f, f.outputPadded.Get(), 0);
    Check(rejected.ok && rejected.result == OFPS_OK && rejected.eval.path == OFPS_PATH_PASSTHROUGH &&
              rejected.eval.warpPath == OFPS_WARP_NONE && model.lastInputs.color.res == f.color.Get() &&
              model.lastInputs.output.res == f.outputPadded.Get() && model.lastInputs.output.rect.x == 32 &&
              model.lastCreate.w == kW && model.lastCreate.h == kH,
          "unfit output region runs the native model with the original resources and region");
    Check(MatchesSource(rejected.output, 32, 16) && PixelIs(rejected.output, 31, 16, coretest::kMarker, 1.0e-6f),
          "unfit pass-through writes only the host output region");
    const auto rejectStatus = CurrentStatus(core);
    Check(rejectStatus.reason && rejectStatus.reason[0] &&
              std::strstr(rejectStatus.reason, "32,16") &&
              host.lastRejectReason.find("32,16") != std::string::npos,
          "nonzero output origin remains pass-through with a precise status reason");
    const auto creates = model.createCalls;
    const auto again = RunEvaluate(w, core, feature, f, f.outputPadded.Get(), 0);
    Check(again.ok && again.eval.path == OFPS_PATH_PASSTHROUGH && model.createCalls == creates &&
              host.events[OFPS_EVENT_HOST_SHAPE_REJECTED] == 1,
          "unfit shape is latched without repeated creation or rejection events");
    f.outputRect = {0, 0, kW, kH};
    feature->Release();
    DrainReleasedModels(w, core);
    Check(model.releaseCalls == model.createCalls && model.live.empty(),
          "failed and unfit models drain before host dies");
}
void ScenarioRetirement(WarpDevice &w, IOfpsCore *core) {
    coretest::RetirementProbe probe;
    coretest::ComPtr<ID3D12Fence> fence;
    Check(SUCCEEDED(w.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))), "retirement extra fence");
    if (!fence)
        return;
    OfpsFencePoint point{sizeof(OfpsFencePoint), fence.Get(), 1};
    core->RetireResource(&probe, &point);
    Check(probe.references == 2, "RetireResource retains its own reference");
    DrainReleasedModels(w, core);
    Check(probe.references == 2, "submission drain cannot bypass the extra fence");
    Check(SUCCEEDED(fence->Signal(1)), "release the extra retirement gate");
    core->Housekeeping();
    Check(probe.references == 1, "RetireResource releases exactly once after all gates drain");
}
} // namespace
int main(int argc, char **argv) {
    if (!api.Open(argc, argv)) return EXIT_FAILURE;
#ifdef OFPS_TEST_DLL
    if (argc == 3 && std::strcmp(argv[1], "--version") == 0) {
        OfpsVersion version{};
        version.size = sizeof(version);
        const auto abi = api.version(&version);
        std::cout << "ABI=" << abi << ";release=" << version.release << '\n';
        return abi == version.abi ? EXIT_SUCCESS : EXIT_FAILURE;
    }
#endif
    WarpDevice w;
#ifdef OFPS_TEST_DLL
    const bool hardware = false;
#else
    const bool hardware = argc == 2 && std::strcmp(argv[1], "--hardware") == 0;
#endif
    Check(coretest::CreateWarpDevice(w, hardware), "D3D12 device, queue and command list are available");
    if (!w.device)
        return EXIT_FAILURE;
    if (!ShadersBesideCoreModule())
        return EXIT_FAILURE;
    FakeHost hostA, hostB;
    IOfpsCore *core = ScenarioVersionAndCreate(hostA, hostB);
    if (core == nullptr)
        return EXIT_FAILURE;
    core->RegisterQueue(w.device.Get(), w.queue.Get());
    OfpsHostCaps caps{};
    caps.size = sizeof(caps);
    caps.flags = OFPS_CAP_QUEUES;
    core->SetHostCaps(&hostA, &caps);
    ScenarioSettings(core, hostA);
    Check(hostB.events[OFPS_EVENT_SETTINGS_CHANGED] == 1 && hostB.lastSettings.v[OFPS_SET_TEMPORAL_EVERY].i == 8,
          "second host receives SETTINGS_CHANGED");
    HostFrame frame;
    Check(coretest::CreateHostFrame(w, frame), "host colour/depth/motion/output textures allocate");
    if (!frame.outputPadded)
        return EXIT_FAILURE;
    FakeModelHost m1, m3, m4;
    IOfpsFeature *f1 = ScenarioPassthrough(w, core, hostA, m1, frame);
    if (f1 != nullptr) {
        ScenarioWarped(w, core, hostA, m1, frame, f1);
        ScenarioStatusAndPreview(core);
        ScenarioPaddedOutput(w, core, hostA, frame, f1);
        ScenarioForeign(w, core, hostA, m3, frame, f1);
    }
    ScenarioDeferred(w, core, m4, frame);
    ScenarioFailureAndUnfit(w, core, hostA, frame);
    ScenarioRetirement(w, core);
    coretest::ScenarioExtensions(w, core, hostA, frame);
    coretest::ScenarioComputeWarp(w, core, frame);
#ifndef OFPS_TEST_DLL
    coretest::ScenarioComputeDirect(w, frame);
#endif
    core->Housekeeping();
    core->UnregisterHost(&hostB);
    core->UnregisterHost(&hostA);
    core->Release();
    Check(m1.releaseCalls == m1.createCalls && m1.live.empty() && m3.releaseCalls == m3.createCalls &&
              m3.live.empty() && m4.releaseCalls == m4.createCalls && m4.live.empty(),
          "every model created through the core is released through ReleaseModel by the time the core is gone");
    Check(hostA.events[OFPS_EVENT_FIRST_WARPED_FRAME] == 1, "FIRST_WARPED_FRAME fired exactly once in the process");
    Check(hostA.events[OFPS_EVENT_HOST_SHAPE_REJECTED] == 2 && hostA.events[OFPS_EVENT_DEVICE_REMOVED] == 0,
          "both deliberate shape rejections and no device removal");
    std::unordered_set<void *> liveFeatures;
    bool ordered = true;
    for (const auto &[kind, handle] : hostA.featureEvents) {
        if (kind == OFPS_EVENT_FEATURE_CREATED)
            ordered = liveFeatures.insert(handle).second && ordered;
        if (kind == OFPS_EVENT_FEATURE_RELEASED)
            ordered = liveFeatures.erase(handle) == 1 && ordered;
    }
    Check(ordered && liveFeatures.empty(), "all feature event handles are paired and closed");
    Check(hostB.events[OFPS_EVENT_SETTINGS_CHANGED] == hostA.events[OFPS_EVENT_SETTINGS_CHANGED],
          "both hosts receive every settings change");
    Check(hostA.errors == 0, "host A received no error logs");
    Check(w.device->GetDeviceRemovedReason() == S_OK, "the WARP device is healthy at the end");
    Check(!coretest::HasDebugErrors(w.device.Get()), "the D3D12 debug layer reports no errors or corruption");
    if (failures != 0) {
        std::cerr << failures << " core API test failure(s); the core's log follows\n";
        hostA.Dump(std::cerr);
        return EXIT_FAILURE;
    }
    std::cout << "Optimizer FPS core API test passed\n";
    return EXIT_SUCCESS;
}
