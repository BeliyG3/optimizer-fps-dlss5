#include "test_core_api_scenarios.h"
#include "test_core_api_checks.h"
#include "test_core_api_codec.h"
#include <bit>
namespace coretest {
namespace {
void CheckOracleSensitivity() {
    const Reference ref;
    const auto source = Source();
    Check(ref.output.size() == source.size(), "CPU reference layout builds");
    if (ref.output.size() != source.size())
        return;
    std::size_t changed = 0;
    for (std::size_t i = 0; i < source.size(); ++i)
        for (std::size_t c = 0; c < 4; ++c)
            if (!Near(source[i][c], ref.output[i][c], kSdkTolerance)) {
                ++changed;
                break;
            }
    Check(changed > 1000, "SDK oracle rejects a plain copy at more than 1000 pixels");
}
void ModelRetirement(WarpDevice &w, IOfpsCore *core, FakeHost &host) {
    FakeModelHost model;
    IOfpsFeature *feature = nullptr;
    Check(CreateFeatureOnList(w, core, &model, &feature) == OFPS_OK && feature, "retirement model creates");
    if (!feature)
        return;
    ComPtr<ID3D12Fence> gate;
    Check(SUCCEEDED(w.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate))), "model retirement gate");
    if (!gate) {
        feature->Release();
        DrainReleasedModels(w, core);
        return;
    }
    const auto released = host.events[OFPS_EVENT_FEATURE_RELEASED];
    // Block all queue signals, including the core's retirement fence, deterministically.
    Check(SUCCEEDED(w.queue->Wait(gate.Get(), 1)), "queue waits for model retirement gate");
    feature->Release();
    Check(BeginList(w) && SubmitList(w), "submit while retirement fence is blocked");
    core->OnCommandListExecuted(w.queue.Get(), w.list.Get());
    core->Housekeeping();
    core->Housekeeping();
    Check(gate->GetCompletedValue() == 0 && model.releaseCalls == 0 && model.live.size() == 1 &&
              host.events[OFPS_EVENT_FEATURE_RELEASED] == released,
          "Housekeeping cannot call ReleaseModel or FEATURE_RELEASED before fence completion");
    Check(SUCCEEDED(gate->Signal(1)) && WaitForQueue(w.device.Get(), w.queue.Get()), "complete model retirement fence");
    core->Housekeeping();
    Check(model.releaseCalls == 1 && model.live.empty() && host.events[OFPS_EVENT_FEATURE_RELEASED] == released + 1,
          "ReleaseModel is called exactly once after fence completion");
    core->Housekeeping();
    Check(model.releaseCalls == 1, "repeated Housekeeping does not release the model twice");
}
void UavColor(WarpDevice &w, IOfpsCore *core, HostFrame &frame) {
    FakeModelHost model;
    IOfpsFeature *feature = nullptr;
    Check(CreateFeatureOnList(w, core, &model, &feature) == OFPS_OK && feature, "UAV colour feature creates");
    if (!feature)
        return;
    Check(BeginList(w), "begin colour state change");
    Transition(w.list.Get(), frame.color.Get(), frame.colorRest, kOutputRest);
    frame.colorRest = kOutputRest;
    Check(SubmitList(w) && WaitForQueue(w.device.Get(), w.queue.Get()), "colour enters UAV rest state");
    for (int i = 0; i < 2; ++i) {
        const auto run = RunEvaluate(w, core, feature, frame, i == 0 ? frame.output.Get() : frame.color.Get(), 0);
        Check(run.ok && run.result == OFPS_OK && run.eval.path == OFPS_PATH_WARPED && MatchesSdkReference(run.output),
              "UAV-rest colour supports separate output and in-place output, restoring resource states");
    }
    Check(BeginList(w), "begin colour state restore");
    Transition(w.list.Get(), frame.color.Get(), frame.colorRest, kInputRest);
    frame.colorRest = kInputRest;
    Check(SubmitList(w) && WaitForQueue(w.device.Get(), w.queue.Get()), "restore ordinary colour rest state");
    feature->Release();
    DrainReleasedModels(w, core);
    Check(model.releaseCalls == model.createCalls && model.live.empty(), "UAV model releases");
}
void CodecScenario(WarpDevice &w, IOfpsCore *core, FakeHost &host, HostFrame &frame, bool reduced) {
    const std::uint32_t width = reduced ? kW / 2 : kW, height = reduced ? kH / 2 : kH;
    OfpsHostCaps caps{};
    caps.size = sizeof(caps);
    caps.flags = OFPS_CAP_QUEUES | (reduced ? OFPS_CAP_MODEL_RESOLUTION : 0);
    core->SetHostCaps(&host, &caps);
    core->SetDirectHost(&host, 1);
    auto settings = CurrentSettings(core);
    SetInt(settings, OFPS_SET_MODE, 0);
    if (reduced) {
        SetInt(settings, OFPS_SET_TEMPORAL_MODE, 0);
        SetInt(settings, OFPS_SET_MODEL_PASSES, 2);
        SetInt(settings, OFPS_SET_SPREAD_PASSES, 1);
    }
    Check(core->SetSettings(&settings) == OFPS_OK, "codec pass-through settings");
    CodecHost model;
    const bool initialized = model.Initialize(w, width, height);
    Check(initialized, "host codec texture and decode pipeline initialize");
    if (!initialized)
        return;
    IOfpsFeature *feature = nullptr;
    Check(CreateFeatureOnList(w, core, &model, &feature) == OFPS_OK && feature, "codec feature creates");
    if (!feature)
        return;
    const void *featureKey = host.createdHandle;
    Check(featureKey && (reduced ? model.createCalls == 0 && feature->CurrentModelHandle() == nullptr
                                 : model.createCalls == 1 && feature->CurrentModelHandle() == model.lastHandle),
          "codec feature follows the creating host's capability");
    for (int warped = 0; warped < 2; ++warped) {
        if (warped) {
            SetInt(settings, OFPS_SET_MODE, 2);
            Check(core->SetSettings(&settings) == OFPS_OK, "codec warp settings");
        }
        OfpsFrameInputs inputs = FrameInputs(frame, frame.output.Get(), 0);
        inputs.mvScaleY = -3.0f;
        const auto preparesBefore = model.prepareCalls;
        const auto creation = RunEvaluate(w, core, feature, frame, frame.output.Get(), 0, &inputs);
        const auto preparesAfterCreation = model.prepareCalls;
        Check(creation.ok && creation.result == OFPS_OK &&
                  creation.eval.path == static_cast<UINT>(reduced ? OFPS_PATH_CREATION_FRAME
                                                                  : warped ? OFPS_PATH_WARPED : OFPS_PATH_PASSTHROUGH) &&
                  model.createCalls == static_cast<UINT>(warped + 1) &&
                  model.runCalls == static_cast<UINT>(reduced ? warped : warped * 2 + 1) &&
                  model.lastCreate.withholdUi == static_cast<UINT>(reduced ? 0 : warped) &&
                  host.createdHandle == featureKey,
              "flagged creation carries one frame; legacy codec runs immediately");
        const auto run = RunEvaluate(w, core, feature, frame, frame.output.Get(), 0, &inputs);
        if (reduced) {
            Check(preparesAfterCreation == preparesBefore + 1 && model.prepareCalls == preparesAfterCreation + 1,
                  "model-grid codec prepares once per creation and running frame");
        }
        const Reference reference(width, height, 0.5f);
        const auto expected = warped ? reference.output : Source(width, height, 0.5f);
        const auto mw = warped ? reference.layout.workWidth : width;
        const auto mh = warped ? reference.layout.workHeight : height;
        Check(run.ok && run.result == OFPS_OK &&
                  run.eval.path == static_cast<UINT>(warped ? OFPS_PATH_WARPED : OFPS_PATH_PASSTHROUGH),
              "non-IDENTITY codec completes both pass-through and warped paths");
        Check(model.prepareCalls == static_cast<UINT>((warped + 1) * 2) &&
                  model.resolveCalls == static_cast<UINT>(reduced ? warped + 1 : (warped + 1) * 2) &&
                  model.before.res == frame.color.Get() && model.before.restState == frame.colorRest &&
                  model.answerSeen.res != frame.output.Get() && model.answerSeen.rect.w == width &&
                  model.answerSeen.rect.h == height,
              "codec supplies frameBefore and receives its own model-grid answer for ResolveAnswer");
        Check(model.lastInputs.width == mw && model.lastInputs.height == mh && model.lastCreate.w == mw &&
                  model.lastCreate.h == mh && SameRect(model.lastInputs.color.rect, 0, 0, mw, mh) &&
                  SameRect(model.lastInputs.output.rect, 0, 0, mw, mh) && model.lastRunHandle == model.lastHandle &&
                  feature->CurrentModelHandle() == model.lastHandle,
              "model dimensions and recreated handle follow the codec's work grid");
        const float mvToWorkX = static_cast<float>(width) / static_cast<float>(kW);
        const float mvToWorkY = static_cast<float>(height) / static_cast<float>(kH);
        Check(std::bit_cast<uint32_t>(model.lastInputs.mvScaleX) ==
                  std::bit_cast<uint32_t>(warped ? 1.0f : reduced ? inputs.mvScaleX * mvToWorkX : inputs.mvScaleX) &&
                  std::bit_cast<uint32_t>(model.lastInputs.mvScaleY) ==
                  std::bit_cast<uint32_t>(warped ? 1.0f : reduced ? inputs.mvScaleY * mvToWorkY : inputs.mvScaleY) &&
                  SameRect(model.lastInputs.depth.rect, 0, 0, warped ? mw : kW, warped ? mh : kH) &&
                  SameRect(model.lastInputs.motion.rect, 0, 0, warped ? mw : kW, warped ? mh : kH),
              "model motion scale and guide grid match packed or uncompressed input");
        Check(warped || model.lastInputs.color.res == model.encoded.Get(),
              "pass-through model receives modelColor texture");
        Check(MatchesImage(model.colorCapture, warped ? reference.packed : Source(width, height, 0.5f), mw, mh),
              "model receives encoded modelColor pixels, including Pack when warped");
        Check(MatchesImage(model.answerCapture, expected, width, height),
              "ResolveAnswer receives encoded SDK round-trip");
        Image decoded(static_cast<std::size_t>(kW) * kH);
        for (std::uint32_t y = 0; y < kH; ++y)
            for (std::uint32_t x = 0; x < kW; ++x) {
                auto p = expected[(y * height / kH) * width + x * width / kW];
                for (auto &v : p)
                    v = Half(v * 2.0f);
                decoded[y * kW + x] = p;
            }
        Check(MatchesImage(run.output, decoded, kW, kH),
              "ResolveAnswer reverses colour/2 and writes every output pixel");
    }
    if (reduced) {
        frame.outputRect = {32, 16, kW, kH};
        const auto creation = RunEvaluate(w, core, feature, frame, frame.outputPadded.Get(), 0);
        const auto resolvesBefore = model.resolveCalls;
        const auto unfit = RunEvaluate(w, core, feature, frame, frame.outputPadded.Get(), 0);
        Check(creation.ok && creation.eval.path == OFPS_PATH_CREATION_FRAME &&
                  unfit.ok && unfit.result == OFPS_OK && unfit.eval.path == OFPS_PATH_PASSTHROUGH &&
                  model.lastInputs.width == width && model.lastInputs.height == height &&
                  model.lastInputs.color.res == model.encoded.Get() &&
                  std::bit_cast<uint32_t>(model.lastInputs.mvScaleX) ==
                      std::bit_cast<uint32_t>(2.0f * static_cast<float>(width) / kW) &&
                  std::bit_cast<uint32_t>(model.lastInputs.mvScaleY) ==
                      std::bit_cast<uint32_t>(3.0f * static_cast<float>(height) / kH) &&
                  model.resolveCalls == resolvesBefore + 1,
              "unfit model-grid frame uses codec dimensions and scale, then resolves its answer");
        frame.outputRect = {0, 0, kW, kH};
    }
    feature->Release();
    DrainReleasedModels(w, core);
    Check(model.releaseCalls == model.createCalls && model.live.empty(), "codec models drain before their host dies");
    Check(host.releasedHandle == featureKey, "deferred feature releases by stable key");
    if (reduced) {
        SetInt(settings, OFPS_SET_TEMPORAL_MODE, 0);
        SetInt(settings, OFPS_SET_MODEL_PASSES, 1);
        SetInt(settings, OFPS_SET_SPREAD_PASSES, 0);
        Check(core->SetSettings(&settings) == OFPS_OK, "restore temporal settings after model-grid codec");
    }
    core->SetDirectHost(&host, 0);
    caps.flags = OFPS_CAP_QUEUES;
    core->SetHostCaps(&host, &caps);
}
void ModelUiScenario(WarpDevice &w, IOfpsCore *core, FakeHost &host, HostFrame &frame) {
    OfpsHostCaps caps{sizeof(OfpsHostCaps), OFPS_CAP_QUEUES | OFPS_CAP_MODEL_RESOLUTION};
    core->SetHostCaps(&host, &caps);
    core->SetDirectHost(&host, 1);
    auto settings = CurrentSettings(core);
    SetInt(settings, OFPS_SET_MODE, 2);
    Check(core->SetSettings(&settings) == OFPS_OK, "UI scenario enables warp");
    FakeModelHost model;
    IOfpsFeature *feature = nullptr;
    Check(CreateFeatureOnList(w, core, &model, &feature) == OFPS_OK && feature && model.createCalls == 0,
          "UI scenario defers model creation");
    if (!feature) return;
    auto workUi = CreateTexture(w.device.Get(), kWorkW, kWorkH, kColorFormat,
                                D3D12_RESOURCE_FLAG_NONE, kInputRest);
    Check(workUi != nullptr, "UI work-grid texture allocates");
    if (workUi) {
        OfpsFrameInputs inputs = FrameInputs(frame, frame.output.Get(), 0);
        inputs.ui = Resource(workUi.Get(), kColorFormat, {0, 0, kWorkW, kWorkH}, kInputRest);
        auto creation = RunEvaluate(w, core, feature, frame, frame.output.Get(), 0, &inputs);
        auto run = RunEvaluate(w, core, feature, frame, frame.output.Get(), 0, &inputs);
        Check(creation.ok && creation.eval.path == OFPS_PATH_CREATION_FRAME && run.ok &&
                  model.createCalls == 1 && model.lastCreate.withholdUi == 0 &&
                  model.lastInputs.withholdUi == 0 && model.lastInputs.ui.res == workUi.Get(),
              "UI already on work grid remains available to model");
        inputs.ui = Resource(frame.color.Get(), kColorFormat, {0, 0, kW, kH}, frame.colorRest);
        creation = RunEvaluate(w, core, feature, frame, frame.output.Get(), 0, &inputs);
        run = RunEvaluate(w, core, feature, frame, frame.output.Get(), 0, &inputs);
        Check(creation.ok && creation.eval.path == OFPS_PATH_CREATION_FRAME && run.ok &&
                  model.createCalls == 2 && model.lastCreate.withholdUi == 1 &&
                  model.lastInputs.withholdUi == 1 && model.lastInputs.ui.res == nullptr,
              "native UI on warped model triggers one rebuild and is withheld");
        const auto repeat = RunEvaluate(w, core, feature, frame, frame.output.Get(), 0, &inputs);
        Check(repeat.ok && model.createCalls == 2, "unchanged UI policy does not rebuild model");
        const void *previousModel = feature->CurrentModelHandle();
        inputs.ui = Resource(workUi.Get(), kColorFormat, {0, 0, kWorkW, kWorkH}, kInputRest);
        model.createResult = OFPS_E_DEVICE;
        const auto failed = RunEvaluate(w, core, feature, frame, frame.output.Get(), 0, &inputs);
        Check(failed.ok && failed.result == OFPS_E_DEVICE && model.createCalls == 3 &&
                  feature->CurrentModelHandle() == previousModel,
              "failed UI-policy rebuild retains the prior model");
        model.createResult = OFPS_OK;
        creation = RunEvaluate(w, core, feature, frame, frame.output.Get(), 0, &inputs);
        run = RunEvaluate(w, core, feature, frame, frame.output.Get(), 0, &inputs);
        Check(creation.ok && creation.eval.path == OFPS_PATH_CREATION_FRAME && run.ok &&
                  model.createCalls == 4 && model.lastCreate.withholdUi == 0 &&
                  model.lastInputs.ui.res == workUi.Get(),
              "failed UI-policy rebuild retries once and restores work-grid UI");
    }
    feature->Release();
    DrainReleasedModels(w, core);
    Check(model.releaseCalls == model.createCalls && model.live.empty(),
          "UI-policy failure and replacement release every model handle");
    caps.flags = OFPS_CAP_QUEUES;
    core->SetHostCaps(&host, &caps);
    core->SetDirectHost(&host, 0);
}
void DeferredGridScenario(WarpDevice &w, IOfpsCore *core, FakeHost &host, HostFrame &frame) {
    OfpsHostCaps caps{sizeof(OfpsHostCaps), OFPS_CAP_QUEUES | OFPS_CAP_MODEL_RESOLUTION};
    core->SetHostCaps(&host, &caps);
    auto settings = CurrentSettings(core);
    SetInt(settings, OFPS_SET_MODE, 0);
    Check(core->SetSettings(&settings) == OFPS_OK, "deferred grid uses Mode Off");
    FakeModelHost nonDirectModel;
    IOfpsFeature *nonDirect = nullptr;
    Check(CreateFeatureOnList(w, core, &nonDirectModel, &nonDirect) == OFPS_OK && nonDirect &&
              nonDirectModel.createCalls == 1 && nonDirect->CurrentModelHandle() == nonDirectModel.lastHandle,
          "capability on a registered non-direct host does not defer model creation");
    if (nonDirect) {
        nonDirect->Release();
        DrainReleasedModels(w, core);
    }
    core->SetDirectHost(&host, 1);
    FakeModelHost firstModel, secondModel;
    IOfpsFeature *first = nullptr, *second = nullptr;
    Check(CreateFeatureOnList(w, core, &firstModel, &first) == OFPS_OK && first && firstModel.createCalls == 0,
          "first deferred feature has no model");
    const void *firstKey = host.createdHandle;
    const auto eventStart = host.featureEvents.size();
    const auto releasedBefore = host.events[OFPS_EVENT_FEATURE_RELEASED];
    if (first) first->Release();
    Check(CreateFeatureOnList(w, core, &secondModel, &second) == OFPS_OK && second && secondModel.createCalls == 0,
          "replacement deferred feature has no model in the release frame");
    const void *secondKey = host.createdHandle;
    Check(firstKey && secondKey && firstKey != secondKey && second &&
              second->CurrentModelHandle() == nullptr &&
              host.events[OFPS_EVENT_FEATURE_RELEASED] == releasedBefore,
          "release and create in one frame keep distinct handles before retirement");
    DrainReleasedModels(w, core);
    Check(host.featureEvents.size() >= eventStart + 2 &&
              host.featureEvents[eventStart].first == OFPS_EVENT_FEATURE_CREATED &&
              host.featureEvents[eventStart].second == secondKey &&
              host.featureEvents[eventStart + 1].first == OFPS_EVENT_FEATURE_RELEASED &&
              host.featureEvents[eventStart + 1].second == firstKey &&
              host.releasedHandle == firstKey && firstModel.createCalls == 0 && second &&
              second->CurrentModelHandle() == nullptr,
          "events are CREATED(A), CREATED(B), RELEASED(A), with B still live");
    if (second) {
        secondModel.prepareResult = OFPS_E_STATE;
        const auto failed = RunEvaluate(w, core, second, frame, frame.output.Get(), 0);
        Check(failed.ok && failed.result == OFPS_E_STATE && secondModel.createCalls == 0 &&
                  secondModel.endFrameCalls == 1 && second->CurrentModelHandle() == nullptr,
              "failed codec preparation creates no model and ends the frame");
        secondModel.prepareResult = OFPS_S_IDENTITY;
        secondModel.createResult = OFPS_S_MODEL_NEXT_FRAME;
        secondModel.ready = 0;
        const auto creation = RunEvaluate(w, core, second, frame, frame.output.Get(), 0);
        const auto pending = RunEvaluate(w, core, second, frame, frame.output.Get(), 0);
        Check(creation.ok && pending.ok && creation.eval.path == OFPS_PATH_CREATION_FRAME &&
                  pending.eval.path == OFPS_PATH_CREATION_FRAME && secondModel.createCalls == 1 &&
                  secondModel.runCalls == 0 && secondModel.endFrameCalls == 3,
              "deferred model waits for readiness without duplicate creation or RunModel");
        secondModel.ready = 1;
        const auto ready = RunEvaluate(w, core, second, frame, frame.output.Get(), 0);
        Check(ready.ok && ready.eval.path == OFPS_PATH_PASSTHROUGH && secondModel.runCalls == 1 &&
                  secondModel.lastInputs.reset == 1 && second->CurrentModelHandle() == secondModel.lastHandle,
              "first submitted deferred model run has reset and exposes model handle");
        second->RequestModelRebuild();
        const auto rebuilding = RunEvaluate(w, core, second, frame, frame.output.Get(), 0);
        const auto rebuilt = RunEvaluate(w, core, second, frame, frame.output.Get(), 0);
        Check(rebuilding.ok && rebuilding.eval.path == OFPS_PATH_CREATION_FRAME && rebuilt.ok &&
                  secondModel.createCalls == 2 && secondModel.runCalls == 2 && secondModel.lastInputs.reset == 1,
              "requested rebuild creates once on codec grid and resets the next model run");
        secondModel.runResult = OFPS_E_DEVICE;
        const auto modelError = RunEvaluate(w, core, second, frame, frame.output.Get(), 0);
        secondModel.runResult = OFPS_OK;
        const auto recovered = RunEvaluate(w, core, second, frame, frame.output.Get(), 0);
        Check(modelError.ok && modelError.result == OFPS_E_DEVICE && recovered.ok &&
                  recovered.result == OFPS_OK && secondModel.createCalls == 2 &&
                  secondModel.endFrameCalls == 8,
              "model error ends its frame and the next frame recovers without another create");
        second->Release();
        DrainReleasedModels(w, core);
        Check(host.releasedHandle == secondKey && secondModel.releaseCalls == secondModel.createCalls &&
                  secondModel.live.empty(),
              "deferred model releases with stable feature key");
    }
    caps.flags = OFPS_CAP_QUEUES;
    core->SetHostCaps(&host, &caps);
    core->SetDirectHost(&host, 0);
}
void FractionalMotionScaleScenario(WarpDevice &w, IOfpsCore *core, FakeHost &host) {
    constexpr UINT width = 8, height = 1920, modelHeight = 1001;
    OfpsHostCaps caps{sizeof(OfpsHostCaps), OFPS_CAP_QUEUES | OFPS_CAP_MODEL_RESOLUTION};
    core->SetHostCaps(&host, &caps);
    core->SetDirectHost(&host, 1);
    auto settings = CurrentSettings(core);
    SetInt(settings, OFPS_SET_MODE, 0);
    Check(core->SetSettings(&settings) == OFPS_OK, "fractional motion scenario uses Mode Off");
    HostFrame frame;
    frame.color = CreateTexture(w.device.Get(), width, height, kColorFormat,
                                D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, kInputRest);
    frame.depth = CreateTexture(w.device.Get(), width, height, DXGI_FORMAT_R32_FLOAT,
                                D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, kInputRest);
    frame.motion = CreateTexture(w.device.Get(), width, height, DXGI_FORMAT_R16G16_FLOAT,
                                 D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, kInputRest);
    frame.output = CreateTexture(w.device.Get(), width, height, kColorFormat,
                                 D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                 kOutputRest);
    frame.rtvHeap = CreateRtvHeap(w.device.Get(), 5);
    const bool resources = frame.color && frame.depth && frame.motion && frame.output && frame.rtvHeap;
    Check(resources, "fractional motion resources allocate");
    if (!resources) return;
    frame.rtv[3] = RtvAt(w.device.Get(), frame.rtvHeap.Get(), 3);
    w.device->CreateRenderTargetView(frame.output.Get(), nullptr, frame.rtv[3]);
    CodecHost model;
    Check(model.Initialize(w, width, modelHeight), "fractional motion codec initializes");
    IOfpsFeature *feature = nullptr;
    if (BeginList(w)) {
        OfpsFeatureDesc desc{};
        desc.size = sizeof(desc);
        desc.width = width; desc.height = height;
        desc.resourceDevice = w.device.Get(); desc.adapterLuid = w.device->GetAdapterLuid();
        const int created = core->CreateFeature(w.list.Get(), &desc, &model, &feature);
        const bool submitted = SubmitList(w);
        if (submitted) core->OnCommandListExecuted(w.queue.Get(), w.list.Get());
        Check(submitted && WaitForQueue(w.device.Get(), w.queue.Get()) &&
                  created == OFPS_OK && feature && model.createCalls == 0,
              "fractional motion feature defers creation");
    }
    if (feature) {
        OfpsFrameInputs inputs = FrameInputs(frame, frame.output.Get(), 0);
        for (OfpsResource *resource : {&inputs.color, &inputs.depth, &inputs.motion, &inputs.output})
            resource->rect = {0, 0, width, height};
        inputs.mvScaleX = 2.0f; inputs.mvScaleY = -3.0f;
        const auto creation = RunEvaluate(w, core, feature, frame, frame.output.Get(), 0, &inputs);
        const auto run = RunEvaluate(w, core, feature, frame, frame.output.Get(), 0, &inputs);
        const float mvToWorkY = static_cast<float>(modelHeight) / static_cast<float>(height);
        Check(creation.ok && creation.eval.path == OFPS_PATH_CREATION_FRAME && run.ok &&
                  model.createCalls == 1 && model.runCalls == 1 &&
                  std::bit_cast<uint32_t>(model.lastInputs.mvScaleX) == std::bit_cast<uint32_t>(inputs.mvScaleX) &&
                  std::bit_cast<uint32_t>(model.lastInputs.mvScaleY) ==
                      std::bit_cast<uint32_t>(inputs.mvScaleY * mvToWorkY) &&
                  SameRect(model.lastInputs.depth.rect, 0, 0, width, height) &&
                  SameRect(model.lastInputs.motion.rect, 0, 0, width, height),
              "1001/1920 motion ratio uses separate float division before negative scale multiplication");
        feature->Release();
        DrainReleasedModels(w, core);
    }
    caps.flags = OFPS_CAP_QUEUES;
    core->SetHostCaps(&host, &caps);
    core->SetDirectHost(&host, 0);
}
void DiscardCreationListScenario(WarpDevice &w, IOfpsCore *core, FakeHost &host, HostFrame &frame) {
    OfpsHostCaps caps{sizeof(OfpsHostCaps), OFPS_CAP_QUEUES | OFPS_CAP_MODEL_RESOLUTION};
    core->SetHostCaps(&host, &caps);
    core->SetDirectHost(&host, 1);
    auto settings = CurrentSettings(core);
    SetInt(settings, OFPS_SET_MODE, 0);
    Check(core->SetSettings(&settings) == OFPS_OK, "discarded list scenario uses Mode Off");
    FakeModelHost model;
    IOfpsFeature *feature = nullptr;
    if (BeginList(w)) {
        OfpsFeatureDesc desc{};
        desc.size = sizeof(desc);
        desc.width = kW; desc.height = kH;
        desc.resourceDevice = w.device.Get(); desc.adapterLuid = w.device->GetAdapterLuid();
        const int created = core->CreateFeature(w.list.Get(), &desc, &model, &feature);
        Check(created == OFPS_OK && feature && model.createCalls == 0 &&
                  SUCCEEDED(w.list->Close()), "discarded creation list has no model commands");
    }
    if (feature) {
        const auto creation = RunEvaluate(w, core, feature, frame, frame.output.Get(), 0);
        const auto run = RunEvaluate(w, core, feature, frame, frame.output.Get(), 0);
        Check(creation.ok && creation.eval.path == OFPS_PATH_CREATION_FRAME && run.ok &&
                  model.createCalls == 1 && model.runCalls == 1 && model.lastInputs.reset == 1,
              "first submitted Evaluate creates once after discarding the CreateFeature list");
        feature->Release();
        DrainReleasedModels(w, core);
    }
    caps.flags = OFPS_CAP_QUEUES;
    core->SetHostCaps(&host, &caps);
    core->SetDirectHost(&host, 0);
}
} // namespace
void ScenarioExtensions(WarpDevice &w, IOfpsCore *core, FakeHost &host, HostFrame &frame) {
    CheckOracleSensitivity();
    ModelRetirement(w, core, host);
    UavColor(w, core, frame);
    CodecScenario(w, core, host, frame, false);
    CodecScenario(w, core, host, frame, true);
    ModelUiScenario(w, core, host, frame);
    DeferredGridScenario(w, core, host, frame);
    FractionalMotionScaleScenario(w, core, host);
    DiscardCreationListScenario(w, core, host, frame);
}
} // namespace coretest
