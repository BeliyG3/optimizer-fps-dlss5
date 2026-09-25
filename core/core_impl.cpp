#include "core/core_impl.h"
#include "core/frame/lifecycle.h"
#include "core/frame/model_passes.h"
#include "ofps_version.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>

namespace ofps::core {
Core &CoreInstance() {
    static Core instance;
    return instance;
}
bool Core::ModelResolutionHost() const {
    if (!directHost_) return false;
    for (const auto &entry : hosts_)
        if (entry.host == directHost_)
            return (entry.caps.flags & OFPS_CAP_MODEL_RESOLUTION) != 0;
    return false;
}
int Core::AddHost(IOfpsHost *host) {
    if (InCallback())
        return OFPS_E_STATE;
    std::lock_guard lock(Ctx().mutex);
    for (const auto &entry : hosts_)
        if (entry.host == host)
            return OFPS_S_EXISTING;
    const bool first = hosts_.empty();
    if (first && !initialized_) {
        DefaultSettingsValues(&Ctx().values);
        ValuesToSettings(Ctx().values, &settings_);
        Ctx().config = settings_.config;
        Ctx().temporal = settings_.temporal;
        Ctx().diag = settings_.diag;
        HMODULE module = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                reinterpret_cast<LPCWSTR>(&OfpsCoreVersion), &module))
            return OFPS_E_STATE;
        wchar_t path[32768] = {};
        const DWORD length = GetModuleFileNameW(module, path, static_cast<DWORD>(std::size(path)));
        if (!length || length >= std::size(path))
            return OFPS_E_STATE;
        Ctx().shaderDirectory = std::filesystem::path(path).parent_path().wstring();
        SetLog(LogSink);
        SetEventSink([](OfpsEvent kind, const OfpsEventData &data) { CoreInstance().Emit(kind, data); });
        initialized_ = true;
    }
    hosts_.push_back({host, {sizeof(OfpsHostCaps), 0}});
    released_ = false;
    return first ? OFPS_OK : OFPS_S_EXISTING;
}
int Core::CreateFeature(ID3D12GraphicsCommandList *cmd, const OfpsFeatureDesc *desc, IOfpsModelHost *host,
                        IOfpsFeature **out) try {
    if (out)
        *out = nullptr;
    if (InCallback())
        return OFPS_E_STATE;
    if (!out || !cmd || !desc || desc->size < sizeof(*desc) || !host || !desc->resourceDevice || !desc->width ||
        !desc->height || cmd->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT)
        return OFPS_E_ARG;
    std::lock_guard lock(Ctx().mutex);
    const InsideCoreScope scope;
    ofps::sdk::LayoutV2 layout{};
    const bool warp = DesiredLayout(Ctx().config, desc->width, desc->height, &layout);
    if (warp && !Ctx().features.empty())
        return OFPS_S_FOREIGN;
    auto st = std::make_unique<FeatureState>();
    st->modelHost = host;
    st->adapterLuid = desc->adapterLuid;
    st->nativeWidth = desc->width;
    st->nativeHeight = desc->height;
    st->config = Ctx().config;
    st->layout = layout;
    st->modelResolution = ModelResolutionHost();
    int rc = OFPS_OK;
    if (!st->modelResolution)
        rc = RecreateReal(*st, cmd, warp ? layout.workWidth : desc->width, warp ? layout.workHeight : desc->height);
    if (rc < 0 && warp) {
        Log(true, "Optimizer FPS NGX hook: feature 18 create failed (%d) at %ux%u; creating at native size", rc,
            layout.workWidth, layout.workHeight);
        st->disabled = true;
        rc = RecreateReal(*st, cmd, desc->width, desc->height);
    }
    if (rc < 0) {
        QueueReleased(host, host);
        return rc;
    }
    if (st->modelResolution) {
        if (nextFeatureId_ == UINTPTR_MAX) return OFPS_E_STATE;
        st->hostHandle = reinterpret_cast<void *>(++nextFeatureId_);
    } else {
        st->hostHandle = st->realHandle;
    }
    auto wrapper = std::make_unique<Feature>(*this, st.get(), st->hostHandle, false);
    *out = wrapper.get();
    wrappers_.push_back(std::move(wrapper));
    Ctx().status.featureCreated = true;
    Ctx().status.nativeWidth = desc->width;
    Ctx().status.nativeHeight = desc->height;
    Ctx().status.workWidth = st->createdWidth;
    Ctx().status.workHeight = st->createdHeight;
    if (st->modelResolution)
        Log(false, "Optimizer FPS core: feature 18 created with model grid deferred, native %ux%u", desc->width, desc->height);
    else
        Log(false, "Optimizer FPS NGX hook: feature 18 created, native %ux%u, model %ux%u (%s)", desc->width, desc->height,
            st->createdWidth, st->createdHeight, st->warped ? "warped" : "pass-through");
    const OfpsEventData data{sizeof(OfpsEventData), nullptr, st->hostHandle, nullptr};
    Ctx().features[st->hostHandle] = std::move(st);
    Emit(OFPS_EVENT_FEATURE_CREATED, data);
    return OFPS_OK;
} catch (...) {
    if (out) *out = nullptr;
    return OFPS_E_STATE;
}
int Core::AdoptFeature(ID3D12GraphicsCommandList *cmd, const OfpsFeatureDesc *desc, void *handle, IOfpsModelHost *host,
                       IOfpsFeature **out) try {
    if (out)
        *out = nullptr;
    if (InCallback())
        return OFPS_E_STATE;
    if (!out || !cmd || !desc || desc->size < sizeof(*desc) || !host || !handle || !desc->resourceDevice ||
        !desc->width || !desc->height || cmd->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT)
        return OFPS_E_ARG;
    std::lock_guard lock(Ctx().mutex);
    const InsideCoreScope scope;
    if (!Ctx().features.empty())
        return OFPS_S_FOREIGN;
    auto st = std::make_unique<FeatureState>();
    st->realHandle = st->hostHandle = handle;
    st->modelHost = host;
    st->adapterLuid = desc->adapterLuid;
    st->nativeWidth = st->createdWidth = desc->width;
    st->nativeHeight = st->createdHeight = desc->height;
    st->config = Ctx().config;
    Ctx().status.featureCreated = Ctx().status.adopted = true;
    Ctx().status.nativeWidth = Ctx().status.workWidth = desc->width;
    Ctx().status.nativeHeight = Ctx().status.workHeight = desc->height;
    Log(false, "Optimizer FPS NGX hook: feature 18 adopted (created before the hooks were installed), native %ux%u",
        desc->width, desc->height);
    auto wrapper = std::make_unique<Feature>(*this, st.get(), handle, true);
    *out = wrapper.get();
    wrappers_.push_back(std::move(wrapper));
    Ctx().features[handle] = std::move(st);
    Ctx().foreign.erase(handle);
    return OFPS_OK;
} catch (...) {
    if (out) *out = nullptr;
    return OFPS_E_STATE;
}
void Core::NotifyForeignReleased(void *handle) {
    if (InCallback())
        return;
    std::lock_guard lock(Ctx().mutex);
    Ctx().foreign.erase(handle);
}
// Queue notifications intentionally avoid Ctx().mutex: gpu registries/submissions own their locks.
void Core::RegisterQueue(ID3D12Device *device, ID3D12CommandQueue *queue) {
    if (gpu::RegisterQueue(device, queue) && !Ctx().submission.Track(device, queue)) {
        gpu::UnregisterQueue(queue);
        Log(true, "Optimizer FPS core: submission fence device error; queue registration failed");
    }
}
void Core::UnregisterQueue(ID3D12CommandQueue *queue) {
    Ctx().submission.Untrack(queue);
    gpu::UnregisterQueue(queue);
}
void Core::OnCommandListExecuted(ID3D12CommandQueue *queue, ID3D12CommandList *list) {
    NoteCommandListExecuted(queue, list);
}
void Core::RetireResource(IUnknown *object, const OfpsFencePoint *extra) {
    if (!object)
        return;
    gpu::Grave grave;
    object->AddRef();
    grave.objects.push_back(object);
    if (extra && extra->size >= sizeof(*extra))
        grave.gate.Add(extra->fence, extra->value);
    if (InCallback()) {
        deferredRetirements_.push_back(std::move(grave));
        return;
    }
    std::lock_guard lock(Ctx().mutex);
    grave.gate.Append(gpu::SignalGateAll());
    Ctx().submission.PendingGateAll(&grave.gate);
    Ctx().graveyard.Add(std::move(grave), Ctx().evalCounter);
}
void Core::FlushRetirements() {
    for (auto &g : deferredRetirements_) {
        g.gate.Append(gpu::SignalGateAll());
        Ctx().submission.PendingGateAll(&g.gate);
        Ctx().graveyard.Add(std::move(g), Ctx().evalCounter);
    }
    deferredRetirements_.clear();
}
void Core::Housekeeping() {
    if (InCallback())
        return;
    std::unique_lock lock(Ctx().mutex, std::try_to_lock);
    if (!lock.owns_lock())
        return;
    const InsideCoreScope scope;
    DrainSubmissions(gpu::Submission::Signal::Settled);
    DrainGraveyard(false);
    DrainReleased();
    if (!deviceRemoved_ && DeviceRemovedNow()) {
        deviceRemoved_ = true;
        Emit(OFPS_EVENT_DEVICE_REMOVED, {sizeof(OfpsEventData), nullptr, nullptr, nullptr});
    }
}
void Core::Release() {
    if (InCallback())
        return;
    std::vector<std::unique_ptr<Feature>> wrappers;
    {
        std::lock_guard lock(Ctx().mutex);
        if (!hosts_.empty())
            return;
        wrappers.swap(wrappers_);
    }
    for (auto &wrapper : wrappers)
        wrapper->Release();
    std::lock_guard lock(Ctx().mutex);
    const InsideCoreScope scope;
    DrainSubmissions(gpu::Submission::Signal::All);
    DrainGraveyard(true);
    DrainReleased();
    released_ = true;
}
void Core::ForgetFeature(Feature *feature) {
    std::erase_if(wrappers_, [feature](const auto &p) { return p.get() == feature; });
}
void Core::QueueReleased(IOfpsModelHost *host, void *handle) { pendingReleases_.push_back({host, handle}); }
void Core::DrainReleased() {
    for (auto it = pendingReleases_.begin(); it != pendingReleases_.end();) {
        if (Ctx().graveyard.References(it->host)) {
            ++it;
            continue;
        }
        const OfpsEventData data{sizeof(OfpsEventData), nullptr, it->handle, nullptr};
        it = pendingReleases_.erase(it);
        Emit(OFPS_EVENT_FEATURE_RELEASED, data);
    }
}
} // namespace ofps::core
extern "C" uint32_t OfpsCoreVersion(OfpsVersion *out) {
    if (out && out->size >= sizeof(uint32_t)) {
        OfpsVersion full{};
        full.size = std::min<uint32_t>(out->size, sizeof(full));
        full.abi = OFPS_ABI_VERSION;
        std::snprintf(full.release, sizeof(full.release), "%s", OFPS_ADDON_VERSION_STRING);
        std::memcpy(out, &full, full.size);
    }
    return OFPS_ABI_VERSION;
}
extern "C" int OfpsCreateCore(uint32_t abi, IOfpsHost *host, IOfpsCore **out) try {
    if (!out)
        return OFPS_E_ARG;
    *out = nullptr;
    if (abi != OFPS_ABI_VERSION)
        return OFPS_E_ABI;
    if (!host)
        return OFPS_E_ARG;
    auto &core = ofps::core::CoreInstance();
    const int result = core.AddHost(host);
    if (result >= 0)
        *out = &core;
    return result;
} catch (...) {
    if (out) *out = nullptr;
    return OFPS_E_STATE;
}
