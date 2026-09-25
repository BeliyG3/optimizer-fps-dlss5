#pragma once
#include "core/api/ofps_settings_schema.h"
#include "test_core_api_frame.h"
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ostream>
#include <utility>
#include <string>
#include <vector>
namespace coretest {
constexpr std::uint32_t kEventKinds = 8;
struct FakeHost : IOfpsHost {
    std::vector<std::string> logs;
    std::vector<OfpsEvent> eventOrder;
    std::vector<std::pair<OfpsEvent, void *>> featureEvents;
    std::uint32_t warnings = 0, errors = 0;
    std::uint32_t events[kEventKinds] = {};
    OfpsSettingsValues lastSettings{};
    void *createdHandle = nullptr;
    void *releasedHandle = nullptr;
    std::string lastRejectReason;
    void Log(OfpsLogLevel level, const char *text) override {
        logs.emplace_back(text != nullptr ? text : "(null)");
        if (level == OFPS_LOG_WARN)
            ++warnings;
        if (level == OFPS_LOG_ERROR)
            ++errors;
    }
    void OnEvent(OfpsEvent kind, const OfpsEventData *data) override {
        eventOrder.push_back(kind);
        if (static_cast<std::uint32_t>(kind) < kEventKinds)
            ++events[static_cast<std::uint32_t>(kind)];
        if (data == nullptr)
            return;
        switch (kind) {
        case OFPS_EVENT_SETTINGS_CHANGED:
            if (data->payload != nullptr) {
                const auto *values = static_cast<const OfpsSettingsValues *>(data->payload);
                const std::size_t bytes = values->size < sizeof(lastSettings) ? values->size : sizeof(lastSettings);
                std::memcpy(&lastSettings, values, bytes);
            }
            break;
        case OFPS_EVENT_FEATURE_CREATED:
            createdHandle = data->handle;
            featureEvents.emplace_back(kind, data->handle);
            break;
        case OFPS_EVENT_FEATURE_RELEASED:
            releasedHandle = data->handle;
            featureEvents.emplace_back(kind, data->handle);
            break;
        case OFPS_EVENT_HOST_SHAPE_REJECTED:
            lastRejectReason = data->text != nullptr ? data->text : "";
            break;
        default:
            break;
        }
    }
    bool LogContains(const char *needle) const {
        for (const std::string &line : logs)
            if (line.find(needle) != std::string::npos)
                return true;
        return false;
    }
    void Dump(std::ostream &out) const {
        for (const std::string &line : logs)
            out << "  log: " << line << '\n';
    }
};
// Stack-owned probe: the graveyard owns one counted reference until every gate settles.
struct RetirementProbe : IUnknown {
    ULONG references = 1;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override {
        if (!out)
            return E_POINTER;
        *out = nullptr;
        if (iid != __uuidof(IUnknown))
            return E_NOINTERFACE;
        *out = this;
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
    ULONG STDMETHODCALLTYPE Release() override { return --references; }
};
struct ModelCreate {
    std::uint32_t w = 0, h = 0, withholdUi = 0;
};
struct FakeModelHost : IOfpsModelHost {
    int createResult = OFPS_OK;
    int runResult = OFPS_OK;
    int prepareResult = OFPS_S_IDENTITY;
    std::uint32_t ready = 1;
    std::uint32_t createCalls = 0, releaseCalls = 0, runCalls = 0, readyCalls = 0, endFrameCalls = 0, describeCalls = 0, prepareCalls = 0;
    ModelCreate lastCreate;
    void *lastHandle = nullptr;
    void *lastRunHandle = nullptr;
    OfpsModelInputs lastInputs{};
    std::vector<void *> live;
    ReadbackCapture colorCapture, depthCapture, motionCapture;
    int CreateModel(ID3D12GraphicsCommandList *cmd, std::uint32_t w, std::uint32_t h, std::uint32_t withholdUi,
                    void **handle) override {
        (void)cmd;
        ++createCalls;
        lastCreate = ModelCreate{w, h, withholdUi};
        if (handle == nullptr)
            return OFPS_E_ARG;
        *handle = reinterpret_cast<void *>(static_cast<std::uintptr_t>(0x1000u + createCalls));
        lastHandle = *handle;
        live.push_back(*handle);
        return createResult;
    }
    int ReleaseModel(void *handle) override {
        ++releaseCalls;
        for (std::size_t i = 0; i < live.size(); ++i) {
            if (live[i] != handle)
                continue;
            live.erase(live.begin() + static_cast<std::ptrdiff_t>(i));
            return OFPS_OK;
        }
        return OFPS_E_ARG;
    }
    int RunModel(ID3D12GraphicsCommandList *cmd, void *handle, const OfpsModelInputs *inputs) override {
        ++runCalls;
        lastRunHandle = handle;
        if (cmd == nullptr || inputs == nullptr)
            return OFPS_E_ARG;
        const std::size_t bytes = inputs->size < sizeof(lastInputs) ? inputs->size : sizeof(lastInputs);
        std::memcpy(&lastInputs, inputs, bytes);
        if (inputs->color.res == nullptr || inputs->output.res == nullptr)
            return OFPS_E_ARG;
        if (runResult != OFPS_OK)
            return runResult;
        ComPtr<ID3D12Device> device;
        if (FAILED(cmd->GetDevice(IID_PPV_ARGS(&device))))
            return OFPS_E_DEVICE;
        const auto capture = [&](const OfpsResource &r, ReadbackCapture &out) {
            out = CreateReadback(device.Get(), r.res);
            if (!out.buffer)
                return false;
            Barrier(cmd, r, r.restState, D3D12_RESOURCE_STATE_COPY_SOURCE);
            RecordReadback(cmd, r.res, out);
            Barrier(cmd, r, D3D12_RESOURCE_STATE_COPY_SOURCE, r.restState);
            return true;
        };
        if (!capture(inputs->color, colorCapture) || !capture(inputs->depth, depthCapture) ||
            !capture(inputs->motion, motionCapture))
            return OFPS_E_DEVICE;
        Barrier(cmd, inputs->color, inputs->color.restState, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Barrier(cmd, inputs->output, inputs->output.restState, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = inputs->output.res;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = inputs->color.res;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        const OfpsRect &c = inputs->color.rect;
        const D3D12_BOX box{c.x, c.y, 0, c.x + c.w, c.y + c.h, 1};
        cmd->CopyTextureRegion(&dst, inputs->output.rect.x, inputs->output.rect.y, 0, &src, &box);
        Barrier(cmd, inputs->output, D3D12_RESOURCE_STATE_COPY_DEST, inputs->output.restState);
        Barrier(cmd, inputs->color, D3D12_RESOURCE_STATE_COPY_SOURCE, inputs->color.restState);
        return OFPS_OK;
    }
    int PrepareModelInput(ID3D12GraphicsCommandList *, const OfpsFrameInputs *, OfpsResource *,
                          OfpsResource *) override {
        ++prepareCalls;
        return prepareResult;
    }
    int ResolveAnswer(ID3D12GraphicsCommandList *, const OfpsResource *, const OfpsFrameInputs *) override {
        return OFPS_S_IDENTITY;
    }
    std::uint32_t DescribeInputs(char *out, std::uint32_t size) override {
        ++describeCalls;
        static const char kText[] = "fake model host (copy)";
        if (out == nullptr || size == 0)
            return 0;
        const std::uint32_t n = size - 1 < sizeof(kText) - 1 ? size - 1 : static_cast<std::uint32_t>(sizeof(kText) - 1);
        std::memcpy(out, kText, n);
        out[n] = 0;
        return n;
    }
    std::uint32_t ModelReady(void *handle) override {
        (void)handle;
        ++readyCalls;
        return ready;
    }
    void EndFrame(ID3D12GraphicsCommandList *, const OfpsFrameInputs *, const OfpsEvalResult *) override {
        ++endFrameCalls;
    }

  private:
    static void Barrier(ID3D12GraphicsCommandList *cmd, const OfpsResource &r, D3D12_RESOURCE_STATES before,
                        D3D12_RESOURCE_STATES after) {
        if (before == after)
            return;
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = r.res;
        b.Transition.Subresource = r.subresource;
        b.Transition.StateBefore = before;
        b.Transition.StateAfter = after;
        cmd->ResourceBarrier(1, &b);
    }
};
} // namespace coretest
