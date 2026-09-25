#include "core/gpu/crash_guard_seh.h"
#include "core/log.h"

namespace ofps::core::gpu {
LONG RecordCrash(EXCEPTION_POINTERS *info, int stage, CrashInfo &out)
{
    out.code = info != nullptr && info->ExceptionRecord != nullptr ? info->ExceptionRecord->ExceptionCode : 0;
    out.address = info != nullptr && info->ExceptionRecord != nullptr ? info->ExceptionRecord->ExceptionAddress : nullptr;
    out.stage = stage;
    out.module[0] = 0;
    HMODULE module = nullptr;
    if (out.address != nullptr &&
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCWSTR>(out.address), &module)) {
        GetModuleFileNameA(module, out.module, MAX_PATH);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

} // namespace ofps::core::gpu
