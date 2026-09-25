#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "hosts/reshade/addon/layout_bridge_v1.h"

#include <cstdint>
#include <cstdio>
#include <type_traits>

static_assert(std::is_same_v<OptimizerFpsSetSettingV1Fn,
    std::uint32_t (*)(std::uint32_t, OfpsSettingValue)>);

int wmain(int argc, wchar_t **argv) {
    if (argc != 2) {
        std::fputs("usage: probe_setting_export <x64 addon path>\n", stderr);
        return 2;
    }
    // Resolve PE exports without running DllMain or calling an export outside ReShade.
    HMODULE module = LoadLibraryExW(argv[1], nullptr, DONT_RESOLVE_DLL_REFERENCES);
    if (module == nullptr) {
        std::fprintf(stderr, "LoadLibraryExW failed: %lu\n", GetLastError());
        return 1;
    }
    const char *names[] = {OFPS_SET_SETTING_V1_EXPORT_NAME,
                           PW_SET_LAYOUT_V1_EXPORT_NAME,
                           PW_SET_TEMPORAL_V1_EXPORT_NAME};
    bool found = true;
    for (const char *name : names) {
        if (GetProcAddress(module, name) == nullptr) {
            std::fprintf(stderr, "missing export: %s\n", name);
            found = false;
        }
    }
    FreeLibrary(module);
    return found ? 0 : 1;
}
