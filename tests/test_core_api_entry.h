#pragma once
#include "core/api/ofps_core.h"
#include <windows.h>
#include <filesystem>
#include <cstring>
namespace coretest {
struct ApiEntry {
    decltype(&OfpsCoreVersion) version = nullptr;
    decltype(&OfpsCreateCore) create = nullptr;
    HMODULE module = nullptr;
    bool Open(int argc, char **argv) {
#ifdef OFPS_TEST_DLL
        if (argc != 2 && !(argc == 3 && std::strcmp(argv[1], "--version") == 0)) return false;
        const auto file = std::filesystem::absolute(argv[argc - 1]);
        module = LoadLibraryW(file.c_str());
        if (!module) return false;
        version = reinterpret_cast<decltype(version)>(GetProcAddress(module, "OfpsCoreVersion"));
        create = reinterpret_cast<decltype(create)>(GetProcAddress(module, "OfpsCreateCore"));
        return version && create;
#else
        (void)argc; (void)argv;
        version = &OfpsCoreVersion;
        create = &OfpsCreateCore;
        return true;
#endif
    }
    ~ApiEntry() { if (module) FreeLibrary(module); }
};
inline ApiEntry api;
} // namespace coretest
