#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
namespace ofps::core::gpu {
struct CrashInfo {
    DWORD code = 0;
    void *address = nullptr;
    char module[MAX_PATH] = {};
    int stage = 0;
};
LONG RecordCrash(EXCEPTION_POINTERS *info, int stage, CrashInfo &out);
} // namespace ofps::core::gpu
