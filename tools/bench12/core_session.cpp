#include "core_session.h"
#include <cstdio>
#include <stdexcept>
#include <exception>
#include <tlhelp32.h>
#include <cwchar>
namespace {
void CheckModules() {
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snapshot == INVALID_HANDLE_VALUE) throw std::runtime_error("cannot inspect core host modules");
    MODULEENTRY32W entry{}; entry.dwSize = sizeof(entry);
    unsigned cores = 0;
    bool forbidden = false;
    wchar_t system[MAX_PATH]{};
    if (!GetSystemDirectoryW(system, MAX_PATH)) {
        CloseHandle(snapshot);
        throw std::runtime_error("cannot locate system DXGI directory");
    }
    const auto dxgi = std::filesystem::path(system) / L"dxgi.dll";
    if (!Module32FirstW(snapshot, &entry)) {
        CloseHandle(snapshot);
        throw std::runtime_error("cannot enumerate core host modules");
    }
    do {
        const auto extension = std::filesystem::path(entry.szExePath).extension();
        forbidden |= _wcsicmp(extension.c_str(), L".addon64") == 0;
        if (_wcsicmp(entry.szModule, L"dxgi.dll") == 0)
            forbidden |= _wcsicmp(entry.szExePath, dxgi.c_str()) != 0;
        if (_wcsicmp(entry.szModule, L"optimizer-fps-dlss5-core.dll") == 0) ++cores;
    } while (Module32NextW(snapshot, &entry));
    CloseHandle(snapshot);
    if (forbidden || cores != 1) throw std::runtime_error("core host requires one core and no ReShade/addon modules");
    std::puts("[core host] modules: one core, zero addons, system DXGI");
}
}
CoreSession::~CoreSession() {
    if (!Close()) std::terminate(); // Do not destroy a host with live callbacks.
}
void CoreSession::Announce() {
    if (event) return;
    wchar_t name[96];
    swprintf_s(name, L"Local\\OptimizerFpsDirectHost_%lu", GetCurrentProcessId());
    event = CreateEventW(nullptr, TRUE, FALSE, name);
    if (!event) throw std::runtime_error("direct host event creation failed");
    if (!SetEvent(event)) {
        CloseHandle(event); event = nullptr;
        throw std::runtime_error("direct host event signal failed");
    }
}
void CoreSession::Open(const std::filesystem::path &exe, ID3D12Device *device,
                        ID3D12CommandQueue *q, int mode, int temporal, int warpPath) {
    if (loader.Get()) {
        if (device_ == device && queue == q && mode_ == mode && temporal_ == temporal && warpPath_ == warpPath) return;
        if (!Close()) throw std::runtime_error("previous core session is still active");
    }
    Announce();
    if (loader.Attach(exe, "bench12", this) < 0) {
        ResetEvent(event); CloseHandle(event); event = nullptr;
        throw std::runtime_error(loader.Error());
    }
    auto *core = loader.Get();
    CheckModules();
    core->SetDirectHost(this, 1);
    queue = q;
    core->RegisterQueue(device, queue);
    OfpsHostCaps caps{sizeof(OfpsHostCaps), OFPS_CAP_QUEUES};
    core->SetHostCaps(this, &caps);
    OfpsSettingsValues values{}; values.size = sizeof(values);
    core->GetSettings(&values);
    const auto setInt = [&](uint32_t id, int value) {
        values.v[id].i = value; values.explicitMask[id / 64] |= 1ull << (id % 64);
    };
    const auto setFloat = [&](uint32_t id, float value) {
        values.v[id].f = value; values.explicitMask[id / 64] |= 1ull << (id % 64);
    };
    setInt(OFPS_SET_MODE, mode); setInt(OFPS_SET_TEMPORAL_MODE, temporal);
    setInt(OFPS_SET_DEBUG_WARP_PATH, warpPath);
    setInt(OFPS_SET_TEMPORAL_EVERY, 4); setInt(OFPS_SET_MODEL_PASSES, 1);
    setInt(OFPS_SET_SPREAD_PASSES, 0); setInt(OFPS_SET_COLOR_FILTER, 1);
    // The reference INI disables motion extension; the schema default enables it.
    setInt(OFPS_SET_FLAGS, 0);
    setFloat(OFPS_SET_CENTER_X, 80); setFloat(OFPS_SET_CENTER_Y, 80);
    setFloat(OFPS_SET_WORK_X, 90); setFloat(OFPS_SET_WORK_Y, 90);
    setFloat(OFPS_SET_GLOBAL_SCALE, 100);
    if (core->SetSettings(&values) != OFPS_OK) {
        if (!Close()) std::terminate();
        throw std::runtime_error("core settings rejected");
    }
    OfpsSettingsValues effective{}; effective.size = sizeof(effective);
    core->GetSettings(&effective);
    for (uint32_t id = 0; id < OFPS_SET_COUNT; ++id) {
        if ((values.explicitMask[id / 64] & (1ull << (id % 64))) &&
            values.v[id].i != effective.v[id].i) throw std::runtime_error("core effective setting mismatch");
        std::printf("[core settings] id=%u bits=%d\n", id, effective.v[id].i);
    }
    device_ = device; mode_ = mode; temporal_ = temporal; warpPath_ = warpPath;
    reportedWarpPath_ = false;
}
int CoreSession::Create(ID3D12GraphicsCommandList *cmd, ID3D12Device *device, unsigned w, unsigned h) {
    OfpsFeatureDesc desc{sizeof(OfpsFeatureDesc), w, h, device, device->GetAdapterLuid()};
    featureReleased = false;
    const int result = loader.Get()->CreateFeature(cmd, &desc, &model, &feature);
    hadFeature = feature != nullptr;
    return result;
}
int CoreSession::Evaluate(ID3D12GraphicsCommandList *cmd, const OfpsFrameInputs &frame) {
    if (!feature) return OFPS_E_STATE;
    OfpsEvalResult result{}; result.size = sizeof(result);
    const int status = feature->Evaluate(cmd, &frame, &result);
    if (status == OFPS_OK && result.path == OFPS_PATH_WARPED) {
        if ((warpPath_ == 1 && result.warpPath != OFPS_WARP_COMPUTE) ||
            (warpPath_ == 2 && result.warpPath != OFPS_WARP_PIXEL))
            throw std::runtime_error("forced core warp path did not reach OfpsEvalResult");
        if (!reportedWarpPath_) {
            std::printf("[core eval] warpPath=%s\n",
                result.warpPath == OFPS_WARP_COMPUTE ? "Compute" : "Pixel");
            reportedWarpPath_ = true;
        }
    }
    return status;
}
void CoreSession::Submitted(ID3D12CommandQueue *q, ID3D12CommandList *list) {
    if (auto *core = loader.Get()) core->OnCommandListExecuted(q, list);
}
bool CoreSession::Close() {
    auto *core = loader.Get();
    if (!core) {
        if (event) { ResetEvent(event); CloseHandle(event); event = nullptr; }
        return true;
    }
    if (feature) {
        core->Housekeeping();
        OfpsStatus status{}; status.size = sizeof(status);
        core->Status(&status);
        std::printf("[core status] evaluations=%llu fallbackFrames=%llu fullFrames=%llu interpFrames=%llu\n",
            static_cast<unsigned long long>(status.evaluations),
            static_cast<unsigned long long>(status.fallbackFrames),
            static_cast<unsigned long long>(status.fullFrames),
            static_cast<unsigned long long>(status.interpFrames));
        OfpsStatusRow rows[64]{};
        const auto count = core->StatusLines(rows, 64);
        for (uint32_t i = 0; i < count; ++i)
            std::printf("[core status] %s=%s\n", rows[i].label, rows[i].value);
        feature->Release(); feature = nullptr;
    }
    const auto deadline = GetTickCount64() + 10000;
    while ((model.live || (hadFeature && !featureReleased)) && GetTickCount64() < deadline) {
        core->Housekeeping(); Sleep(1);
    }
    core->Housekeeping();
    if (model.live || (hadFeature && !featureReleased)) return false;
    hadFeature = false;
    std::puts("[core host] drain completed: zero live models, feature released");
    if (queue) core->UnregisterQueue(queue);
    queue = nullptr; device_ = nullptr; mode_ = temporal_ = warpPath_ = -1;
    core->SetDirectHost(this, 0);
    if (!loader.Detach()) return false;
    ResetEvent(event); CloseHandle(event); event = nullptr;
    return true;
}
void CoreSession::Log(OfpsLogLevel, const char *text) { std::puts(text); }
void CoreSession::OnEvent(OfpsEvent eventKind, const OfpsEventData *) {
    if (eventKind == OFPS_EVENT_FEATURE_RELEASED) featureReleased = true;
}
