#include "test_core_api_scenarios.h"
#include "test_core_api_checks.h"

// Correction C1 of the menu-mode plan: menu mode registers its private queue with the core (RegisterQueue after the queue
// exists, OnCommandListExecuted after each pass). A model re-created, or a feature released, while a menu pass still runs
// on that queue keeps the old model until the pass completed: the retirement gate is signalled on the private queue too.
// (One device here: the key the queue is registered under matches the feature's device. The ReShade host registers it
// under the native device, see menu_pipeline_gpu.cpp and tests/test_submission_identity.cpp.)
namespace coretest {
namespace {
struct MenuQueue {
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> pass;
};

// A menu pass still running: the private list sits behind the returned gate, then signals f2, as menu_submit.cpp does.
ComPtr<ID3D12Fence> HoldPass(WarpDevice &w, IOfpsCore *core, MenuQueue &m, ComPtr<ID3D12Fence> &f2) {
    ComPtr<ID3D12Fence> gate;
    f2.Reset();
    if (FAILED(w.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate))) ||
        FAILED(w.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&f2))) || FAILED(m.queue->Wait(gate.Get(), 1)))
        return nullptr;
    ID3D12CommandList *lists[] = {m.pass.Get()};
    m.queue->ExecuteCommandLists(1, lists);
    core->OnCommandListExecuted(m.queue.Get(), m.pass.Get());
    return SUCCEEDED(m.queue->Signal(f2.Get(), 1)) ? gate : nullptr;
}

void Complete(WarpDevice &w, IOfpsCore *core, MenuQueue &m, ID3D12Fence *gate) {
    Check(gate && SUCCEEDED(gate->Signal(1)) && WaitForQueue(w.device.Get(), m.queue.Get()), "menu queue: the pass completes");
    core->Housekeeping();
}

void SetMode(IOfpsCore *core, int mode) {
    OfpsSettingsValues v = CurrentSettings(core);
    SetInt(v, OFPS_SET_MODE, mode);
    SetFloat(v, OFPS_SET_CENTER_X, 80.0f);
    SetFloat(v, OFPS_SET_CENTER_Y, 80.0f);
    SetFloat(v, OFPS_SET_WORK_X, 90.0f);
    SetFloat(v, OFPS_SET_WORK_Y, 90.0f);
    Check(core->SetSettings(&v) == OFPS_OK, "menu queue: the mode is set");
}
} // namespace

void ScenarioMenuQueueRelease(WarpDevice &w, IOfpsCore *core, FakeHost &host, HostFrame &frame) {
    MenuQueue m;
    D3D12_COMMAND_QUEUE_DESC desc{};
    desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    const bool made = SUCCEEDED(w.device->CreateCommandQueue(&desc, IID_PPV_ARGS(&m.queue))) &&
                      SUCCEEDED(w.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m.allocator))) &&
                      SUCCEEDED(w.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m.allocator.Get(), nullptr,
                                                            IID_PPV_ARGS(&m.pass))) &&
                      SUCCEEDED(m.pass->Close());
    Check(made, "menu queue: the private queue and its list are created");
    if (!made)
        return;
    const OfpsSettingsValues saved = CurrentSettings(core);
    core->RegisterQueue(w.device.Get(), m.queue.Get());
    SetMode(core, 0);
    FakeModelHost model;
    IOfpsFeature *feature = nullptr;
    Check(CreateFeatureOnList(w, core, &model, &feature) == OFPS_OK && feature, "menu queue: the feature is created");
    if (!feature) {
        core->UnregisterQueue(m.queue.Get());
        core->SetSettings(&saved);
        return;
    }
    const EvalRun run = RunEvaluate(w, core, feature, frame, frame.output.Get(), 1);
    Check(run.ok && run.result == OFPS_OK && model.createCalls == 1, "menu queue: the host evaluates once");
    ComPtr<ID3D12Fence> f2;

    // Re-creation: a layout change re-creates the model at the next host evaluate while a menu pass is held.
    ComPtr<ID3D12Fence> gate = HoldPass(w, core, m, f2);
    Check(gate != nullptr, "menu queue: a pass is held for the re-creation");
    SetMode(core, 2);
    const EvalRun recreated = RunEvaluate(w, core, feature, frame, frame.output.Get(), 0);
    DrainReleasedModels(w, core); // the host's queue is idle and drained: only the private queue still holds the old model
    core->Housekeeping();
    Check(recreated.ok && model.createCalls == 2 && model.releaseCalls == 0 && model.live.size() == 2 && f2->GetCompletedValue() == 0,
          "menu queue: a model re-created while a menu pass runs keeps the old model behind the private queue's fence");
    Complete(w, core, m, gate.Get());
    Check(f2->GetCompletedValue() == 1 && model.releaseCalls == 1 && model.live.size() == 1 && model.live[0] == model.lastHandle,
          "menu queue: the old model is released once the private queue passed the pass");

    // Release: the feature is released while a menu pass is held.
    gate = HoldPass(w, core, m, f2);
    Check(gate != nullptr, "menu queue: a pass is held for the release");
    const auto releasedEvents = host.events[OFPS_EVENT_FEATURE_RELEASED];
    const auto releaseCalls = model.releaseCalls;
    feature->Release();
    DrainReleasedModels(w, core);
    core->Housekeeping();
    Check(f2->GetCompletedValue() == 0 && model.releaseCalls == releaseCalls && !model.live.empty() &&
              host.events[OFPS_EVENT_FEATURE_RELEASED] == releasedEvents,
          "menu queue: a feature released while a menu pass runs keeps its model behind the private queue's fence");
    Complete(w, core, m, gate.Get());
    Check(f2->GetCompletedValue() == 1 && model.releaseCalls == model.createCalls && model.live.empty() &&
              host.events[OFPS_EVENT_FEATURE_RELEASED] == releasedEvents + 1,
          "menu queue: the model is released once the private queue passed the pass");
    core->UnregisterQueue(m.queue.Get());
    core->SetSettings(&saved);
}
} // namespace coretest
