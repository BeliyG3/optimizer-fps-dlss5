#include "core/api/ofps_core.h"
#include "core/api/ofps_settings_schema.h"
#include <cmath>
#include <algorithm>
#include <array>
#include <cstddef>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstring>
struct Host final : IOfpsHost {
    IOfpsCore *core = nullptr;
    int reentrantResult = OFPS_OK;
    unsigned changes = 0;
    OfpsSettingsValues effective{};
    void Log(OfpsLogLevel, const char *) override {}
    void OnEvent(OfpsEvent e, const OfpsEventData *d) override {
        if (e == OFPS_EVENT_SETTINGS_CHANGED) {
            ++changes;
            if (core) reentrantResult = core->SetSettings(static_cast<const OfpsSettingsValues *>(d->payload));
            effective = *static_cast<const OfpsSettingsValues *>(d->payload);
        }
    }
};
struct ModelHost final : IOfpsModelHost {
    unsigned creates = 0, releases = 0;
    int CreateModel(ID3D12GraphicsCommandList *, uint32_t, uint32_t, uint32_t, void **out) override {
        ++creates; *out = this; return OFPS_OK;
    }
    int ReleaseModel(void *) override { ++releases; return OFPS_OK; }
    int RunModel(ID3D12GraphicsCommandList *, void *, const OfpsModelInputs *) override { return OFPS_OK; }
    int PrepareModelInput(ID3D12GraphicsCommandList *, const OfpsFrameInputs *, OfpsResource *, OfpsResource *) override { return OFPS_S_IDENTITY; }
    int ResolveAnswer(ID3D12GraphicsCommandList *, const OfpsResource *, const OfpsFrameInputs *) override { return OFPS_S_IDENTITY; }
    uint32_t DescribeInputs(char *, uint32_t) override { return 0; }
    uint32_t ModelReady(void *) override { return 1; }
    void EndFrame(ID3D12GraphicsCommandList *, const OfpsFrameInputs *, const OfpsEvalResult *) override {}
};
int main() {
    int failed = 0;
    auto check = [&](bool ok, const char *text) {
        if (!ok) {
            std::printf("FAIL: %s\n", text);
            ++failed;
        }
    };
    Host a, b;
    IOfpsCore *core = nullptr, *same = nullptr;
    check(OfpsCreateCore(OFPS_ABI_VERSION + 1, &a, &core) == OFPS_E_ABI && !core, "ABI mismatch");
    check(OfpsCreateCore(OFPS_ABI_VERSION, &a, &core) == OFPS_OK, "first core");
    if (!core)
        return 1;
    check(OfpsCreateCore(OFPS_ABI_VERSION, &b, &same) == OFPS_S_EXISTING && same == core, "shared core");
    a.core = core;
    OfpsSettingsValues v{};
    v.size = sizeof(v);
    core->GetSettings(&v);
    check(v.count == 45, "45 settings");
    v.v[OFPS_SET_TEMPORAL_MODE].i = 2;
    v.v[OFPS_SET_TEMPORAL_EVERY].i = 99;
    v.explicitMask[0] |= 1ull << OFPS_SET_TEMPORAL_EVERY;
    check(core->SetSettings(&v) == OFPS_OK, "clamp accepted");
    check(a.reentrantResult == OFPS_E_STATE, "event reentry refused");
    core->GetSettings(&v);
    check(v.v[OFPS_SET_TEMPORAL_MODE].i == 1 && v.v[OFPS_SET_TEMPORAL_EVERY].i == 8, "temporal clamp");
    check(a.changes == 1 && b.changes == 1 && a.effective.count == 45, "effective event to both hosts");
    float lo = 0, hi = 0;
    core->GetSettingRange(OFPS_SET_OFFSET_X, &lo, &hi);
    check(std::abs(lo + 9.5f) < 0.001f && std::abs(hi - 9.5f) < 0.001f, "dynamic range");
    const auto valid = v;
    v.v[OFPS_SET_CENTER_X].f = 101;
    check(core->SetSettings(&v) == OFPS_E_ARG, "invalid layout refused");
    core->GetSettings(&v);
    check(v.v[OFPS_SET_CENTER_X].f == valid.v[OFPS_SET_CENTER_X].f, "transactional settings");
    OfpsStatus st{};
    st.size = sizeof(st);
    core->Status(&st);
    check(st.reason && st.temporalReason && st.modelPassReason && st.fallbackReason, "owned status strings");
    OfpsStatus partial;
    std::memset(&partial, 0xa5, sizeof(partial));
    constexpr uint32_t prefix = offsetof(OfpsStatus, nativeW);
    partial.size = prefix;
    core->Status(&partial);
    check(partial.size == prefix && partial.active == st.active && partial.deviceRemoved == st.deviceRemoved,
          "partial status fields filled");
    const auto *bytes = reinterpret_cast<const unsigned char *>(&partial);
    check(std::all_of(bytes + prefix, bytes + sizeof(partial), [](auto x) { return x == 0xa5; }),
          "partial status tail untouched");
    struct ExtendedStatus { OfpsStatus status; std::array<unsigned char, 16> tail; } extended{};
    extended.tail.fill(0xa5); extended.status.size = sizeof(extended);
    core->Status(&extended.status);
    check(extended.status.size == sizeof(OfpsStatus) && extended.status.reason &&
          std::all_of(extended.tail.begin(), extended.tail.end(), [](auto x) { return x == 0xa5; }),
          "larger status only writes known fields");
    OfpsVersion version{}; version.size = offsetof(OfpsVersion, release);
    version.release[0] = 'x'; OfpsCoreVersion(&version);
    check(version.abi == OFPS_ABI_VERSION && version.release[0] == 'x', "partial version");
    using Microsoft::WRL::ComPtr;
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> cmd;
    const bool ready = SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) &&
        SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter))) &&
        SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))) &&
        SUCCEEDED(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue))) &&
        SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) &&
        SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&cmd)));
    check(ready, "WARP device and command list");
    ModelHost model, second;
    if (ready) {
        core->RegisterQueue(device.Get(), queue.Get());
        OfpsFeatureDesc desc{sizeof(OfpsFeatureDesc), 1920, 1080, device.Get(), device->GetAdapterLuid()};
        IOfpsFeature *first = nullptr, *foreign = nullptr;
        v.v[OFPS_SET_MODE].i = 1;
        check(core->SetSettings(&v) == OFPS_OK, "warped settings");
        check(core->CreateFeature(cmd.Get(), &desc, &model, &first) == OFPS_OK && first, "first warped feature");
        check(core->CreateFeature(cmd.Get(), &desc, &second, &foreign) == OFPS_S_FOREIGN && !foreign && !second.creates,
              "second warped feature stays foreign");
        core->NotifyForeignReleased(&second);
        if (first) first->Release();
        core->Housekeeping();
        check(core->CreateFeature(cmd.Get(), &desc, &second, &foreign) == OFPS_OK && foreign,
              "new feature accepted after releases");
        if (foreign) foreign->Release();
        core->Housekeeping();
    }
    v.v[OFPS_SET_GLOBAL_SCALE].f = 75.0f;
    check(core->SetSettings(&v) == OFPS_OK, "ordinary host accepts global scale");
    core->GetSettings(&v);
    check(std::abs(v.v[OFPS_SET_GLOBAL_SCALE].f - 75.0f) < 0.001f,
          "ordinary host retains global scale");
    const OfpsHostCaps modelGridCaps{sizeof(OfpsHostCaps), OFPS_CAP_MODEL_RESOLUTION};
    core->SetHostCaps(&a, &modelGridCaps);
    core->SetDirectHost(&a, 1);
    check(core->SetSettings(&v) == OFPS_OK, "model-grid host accepts settings");
    core->GetSettings(&v);
    check(std::abs(v.v[OFPS_SET_GLOBAL_SCALE].f - 100.0f) < 0.001f,
          "model-grid host fixes global scale at 100");
    core->SetDirectHost(&a, 0);
    OfpsStatusRow rows[1]{};
    check(core->StatusLines(rows, 1) <= 1, "bounded rows");
    core->Release();
    core->GetSettings(&v);
    check(v.count == 45, "registered hosts keep core alive");
    core->UnregisterHost(&a);
    core->UnregisterHost(&b);
    core->Release();
    if (ready) {
        check(model.releases == 1 && second.releases == 1, "models retired at shutdown");
        core->UnregisterQueue(queue.Get());
    }
    return failed ? 1 : 0;
}
