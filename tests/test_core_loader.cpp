#include "core/api/ofps_core_loader.h"
#include "core/api/ofps_settings_schema.h"
#include "ofps_version.h"
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <process.h>
#include <thread>
struct Host final : IOfpsHost {
    unsigned changes = 0;
    void Log(OfpsLogLevel, const char *) override {}
    void OnEvent(OfpsEvent event, const OfpsEventData *) override {
        if (event == OFPS_EVENT_SETTINGS_CHANGED) ++changes;
    }
};
int main(int argc, char **argv) {
    unsigned failures = 0;
    auto check = [&](bool ok, const char *name) {
        if (!ok) { ++failures; std::fprintf(stderr, "FAIL: %s\n", name); }
    };
    if (argc != 4) return 2;
    if (std::strcmp(argv[1], "--mismatch") == 0) {
        HMODULE wrongModule = LoadLibraryW(std::filesystem::absolute(argv[2]).c_str());
        if (!wrongModule) return 2;
        // Reproduce the first host's process-local owner record while its DLL is loaded.
        struct OwnerInfo { DWORD format; char name[128]; };
        wchar_t mappingName[96];
        swprintf_s(mappingName, L"Local\\OptimizerFpsCoreOwner_%lu", GetCurrentProcessId());
        HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
            0, sizeof(OwnerInfo), mappingName);
        if (!mapping) return 2;
        auto *owner = static_cast<OwnerInfo *>(MapViewOfFile(mapping,
            FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(OwnerInfo)));
        if (!owner) { CloseHandle(mapping); return 2; }
        strcpy_s(owner->name, "TestHost");
        owner->format = 1;
        Host host;
        ofps::CoreLoader probe;
        const int result = probe.Attach(std::filesystem::absolute(argv[3]), "test", &host);
        const bool ok = result == OFPS_E_ABI && !probe.Get() &&
            probe.Error() == "core of another version is loaded by TestHost" &&
            GetModuleHandleW(L"optimizer-fps-dlss5-core.dll") == wrongModule;
        UnmapViewOfFile(owner);
        CloseHandle(mapping);
        FreeLibrary(wrongModule);
        return ok ? 0 : 1;
    }
    check(_spawnl(_P_WAIT, argv[0], argv[0], "--mismatch", argv[2], argv[1],
        static_cast<char *>(nullptr)) == 0, "version conflict in another process");
    const auto dll = std::filesystem::absolute(argv[1]);
    Host a, b;
    ofps::CoreLoader first, second;
    check(first.Attach("relative/addon.dll", "test", &a) == OFPS_E_ARG, "relative path rejected");
    check(first.Attach({}, "test", &a) == OFPS_E_ARG &&
        first.Error() == "add-on module path unavailable", "unavailable module path");
    const auto missing = dll.parent_path() / ("missing-core-" + std::to_string(GetCurrentProcessId())) / "addon.dll";
    check(first.Attach(missing, "test", &a) == OFPS_E_STATE && !first.Error().empty(), "missing DLL diagnostic");
    check(first.Attach(std::filesystem::absolute(argv[3]), "test", &a) == OFPS_E_STATE &&
        first.Error() == "core DLL next to the add-on has no core exports", "missing exports diagnostic");
    check(first.Attach(std::filesystem::absolute(argv[2]), "test", &a) == OFPS_E_ABI &&
        first.Error() == "core next to the add-on is version 2026.9.0, expected " OFPS_ADDON_VERSION_STRING,
        "adjacent wrong version diagnostic");
    check(!GetModuleHandleW(L"optimizer-fps-dlss5-core.dll"), "rejected DLLs unloaded");
    check(first.Attach(dll, "loader test A", &a) == OFPS_OK, "first attach");
    auto *core = first.Get();
    if (!core) return 1;
    check(second.Attach(dll, "loader test B", &b) == OFPS_S_EXISTING, "second attach");
    check(second.Get() == core, "one singleton");
    HMODULE module = GetModuleHandleW(L"optimizer-fps-dlss5-core.dll");
    auto version = reinterpret_cast<decltype(&OfpsCoreVersion)>(GetProcAddress(module, "OfpsCoreVersion"));
    auto create = reinterpret_cast<decltype(&OfpsCreateCore)>(GetProcAddress(module, "OfpsCreateCore"));
    if (!version || !create) return 1;
    OfpsVersion v{}; v.size = sizeof(v);
    check(version(&v) == 1 && v.abi == 1 && std::strcmp(v.release, OFPS_ADDON_VERSION_STRING) == 0, "version");
    IOfpsCore *wrong = core;
    check(create(2, &a, &wrong) == OFPS_E_ABI && !wrong, "wrong ABI");
    HMODULE extra = LoadLibraryW(dll.c_str());
    check(extra == module, "same module");
    if (extra) FreeLibrary(extra);
    check(GetModuleHandleW(L"optimizer-fps-dlss5-core.dll") == module, "PIN survives FreeLibrary");
    core->SetDirectHost(&a, 1);
    OfpsStatus status{}; status.size = sizeof(status); core->Status(&status);
    check(status.directHost == 1, "direct host registered");
    check(first.Detach(), "first detach");
    core->Status(&status); check(status.directHost == 0, "unregister clears direct host");
    const unsigned beforeA = a.changes, beforeB = b.changes;
    OfpsSettingsValues values{}; values.size = sizeof(values);
    core->GetSettings(&values);
    values.v[OFPS_SET_MODE].i = values.v[OFPS_SET_MODE].i == 0 ? 2 : 0;
    check(core->SetSettings(&values) == OFPS_OK, "remaining host works");
    check(a.changes == beforeA && b.changes == beforeB + 1, "unregistered callback absent");
    check(second.Detach(), "last detach");
    check(first.Attach(dll, "loader test A", &a) == OFPS_OK, "attach after last host");
    check(second.Attach(dll, "loader test B", &b) == OFPS_S_EXISTING, "attach second again");
    check(second.Detach() && first.Detach(), "reverse detach order");
    int resultA = OFPS_E_STATE, resultB = OFPS_E_STATE;
    std::thread threadA([&] { resultA = first.Attach(dll, "thread A", &a); });
    std::thread threadB([&] { resultB = second.Attach(dll, "thread B", &b); });
    threadA.join(); threadB.join();
    check((resultA == OFPS_OK && resultB == OFPS_S_EXISTING) ||
        (resultB == OFPS_OK && resultA == OFPS_S_EXISTING), "concurrent attach");
    check(first.Get() == second.Get(), "concurrent singleton");
    check(first.Detach() && second.Detach(), "concurrent hosts detach");
    check(GetModuleHandleW(L"optimizer-fps-dlss5-core.dll") == module, "module remains pinned");
    std::puts(failures ? "core loader failed" : "core loader passed");
    return failures ? 1 : 0;
}
