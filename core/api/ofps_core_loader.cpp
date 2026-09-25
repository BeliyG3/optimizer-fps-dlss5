#include "core/api/ofps_core_loader.h"
#include "ofps_version.h"
#include <windows.h>
#include <cstdio>
#include <cstring>
namespace ofps {
namespace {
constexpr wchar_t DllName[] = L"optimizer-fps-dlss5-core.dll";
using VersionFn = decltype(&OfpsCoreVersion);
using CreateFn = decltype(&OfpsCreateCore);
struct LoadLock {
    HANDLE handle = nullptr;
    bool acquired = false;
    LoadLock() {
        wchar_t name[96];
        swprintf_s(name, L"Local\\OptimizerFpsCoreLoad_%lu", GetCurrentProcessId());
        handle = CreateMutexW(nullptr, FALSE, name);
        if (handle) {
            const DWORD result = WaitForSingleObject(handle, 10000);
            acquired = result == WAIT_OBJECT_0 || result == WAIT_ABANDONED;
        }
    }
    ~LoadLock() {
        if (acquired) ReleaseMutex(handle);
        if (handle) CloseHandle(handle);
    }
};
// The handle intentionally survives until process exit, just like the pinned DLL.
// No pointer or ownership counter is shared between independently linked CRTs.
struct OwnerInfo { DWORD format; char name[128]; };
OwnerInfo *Owner() {
    static OwnerInfo *view = [] {
        wchar_t name[96];
        swprintf_s(name, L"Local\\OptimizerFpsCoreOwner_%lu", GetCurrentProcessId());
        HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr,
            PAGE_READWRITE, 0, sizeof(OwnerInfo), name);
        if (!mapping) return static_cast<OwnerInfo *>(nullptr);
        auto *data = static_cast<OwnerInfo *>(MapViewOfFile(mapping,
            FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(OwnerInfo)));
        if (!data) CloseHandle(mapping);
        return data;
    }();
    return view;
}
} // namespace
int CoreLoader::Attach(const std::filesystem::path &hostFile,
                       const char *hostName, IOfpsHost *host) {
    if (core_) return host == host_ ? OFPS_S_EXISTING : OFPS_E_STATE;
    error_.clear();
    if (hostFile.empty()) {
        error_ = "add-on module path unavailable";
        return OFPS_E_ARG;
    }
    if (!host || !hostName || !hostFile.is_absolute()) return OFPS_E_ARG;
    LoadLock lock;
    if (!lock.acquired) {
        error_ = "core load mutex could not be acquired";
        return OFPS_E_STATE;
    }
    HMODULE module = GetModuleHandleW(DllName);
    const bool loadedHere = module == nullptr;
    if (loadedHere) module = LoadLibraryW((hostFile.parent_path() / DllName).c_str());
    if (!module) {
        error_ = "core DLL could not be loaded: " + std::to_string(GetLastError());
        return OFPS_E_STATE;
    }
    const auto versionFn = reinterpret_cast<VersionFn>(GetProcAddress(module, "OfpsCoreVersion"));
    const auto createFn = reinterpret_cast<CreateFn>(GetProcAddress(module, "OfpsCreateCore"));
    const auto diagnosticsFn = reinterpret_cast<ofps::temporal::SetTemporalDiagnosticsV1>(
        GetProcAddress(module, "OfpsSetTemporalDiagnosticsV1"));
    const auto motionSourceFn = reinterpret_cast<ofps::core::flow::SetTemporalMotionSourceV1>(
        GetProcAddress(module, "OfpsSetTemporalMotionSourceV1"));
    if (!versionFn || !createFn) {
        error_ = "core DLL next to the add-on has no core exports";
        if (loadedHere) FreeLibrary(module);
        return OFPS_E_STATE;
    }
    OfpsVersion version{};
    version.size = sizeof(version);
    static_assert(sizeof(OFPS_ADDON_VERSION_STRING) <= sizeof(version.release));
    const bool compatible = versionFn(&version) == OFPS_ABI_VERSION &&
        version.abi == OFPS_ABI_VERSION &&
        std::memchr(version.release, 0, sizeof(version.release)) &&
        std::strncmp(version.release, OFPS_ADDON_VERSION_STRING, sizeof(version.release)) == 0;
    if (!compatible) {
        if (loadedHere) {
            const std::string release(version.release, strnlen_s(version.release, sizeof(version.release)));
            error_ = "core next to the add-on is version " + release +
                ", expected " OFPS_ADDON_VERSION_STRING;
            FreeLibrary(module);
        } else {
            const auto *owner = Owner();
            error_ = "core of another version is loaded by ";
            error_ += owner && owner->format == 1 &&
                std::memchr(owner->name, 0, sizeof(owner->name)) ? owner->name : "unknown host";
        }
        return OFPS_E_ABI;
    }
    HMODULE pinned = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(versionFn), &pinned)) {
        error_ = "core DLL could not be pinned";
        if (loadedHere) FreeLibrary(module);
        return OFPS_E_STATE;
    }
    if (loadedHere) {
        if (auto *owner = Owner()) {
            std::snprintf(owner->name, sizeof(owner->name), "%s", hostName);
            owner->format = 1;
        }
        FreeLibrary(module);
    }
    IOfpsCore *core = nullptr;
    const int result = createFn(OFPS_ABI_VERSION, host, &core);
    if (result < 0 || !core) {
        error_ = "OfpsCreateCore failed: " + std::to_string(result);
        return result < 0 ? result : OFPS_E_STATE;
    }
    host_ = host;
    temporalDiagnostics_ = diagnosticsFn;
    temporalMotionSource_ = motionSourceFn;
    if (!diagnosticsFn) error_ = "core DLL has no OfpsSetTemporalDiagnosticsV1 export; default temporal profile remains active";
    core_ = core;
    return result;
}
bool CoreLoader::Detach(bool serialize) {
    auto *core = core_.load();
    if (!core) return true;
    if (!serialize) {
        core->UnregisterHost(host_);
        core_ = nullptr;
        host_ = nullptr;
        temporalDiagnostics_ = nullptr;
        temporalMotionSource_ = nullptr;
        return true;
    }
    LoadLock lock;
    if (!lock.acquired) {
        error_ = "core detach mutex could not be acquired";
        return false;
    }
    core->UnregisterHost(host_);
    core->Release(); // C18: only the last unregistered host performs shutdown.
    core_ = nullptr;
    host_ = nullptr;
    temporalDiagnostics_ = nullptr;
    temporalMotionSource_ = nullptr;
    return true;
}
} // namespace ofps
