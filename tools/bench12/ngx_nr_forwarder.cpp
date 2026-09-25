// Built as nvngx.dll_pwbench12.dll. nvngx_dlssnr.dll resolves the module owning the return address
// of every entry call and requires that module's path to contain "nvngx.dll" (the NGX core is
// _nvngx.dll); any other caller gets NVSDK_NGX_Result_FAIL_PlatformError. The bench therefore calls
// the runtime's exports through this module. Up to six integer/pointer arguments are passed; the
// result goes through a volatile so the call cannot become a tail jump that leaves this frame out.
#include <windows.h>

extern "C" __declspec(dllexport) int pw_bench_ngx_call(const void *function, const unsigned long long *arguments)
{
    using Entry=int(__cdecl *)(unsigned long long, unsigned long long, unsigned long long,
        unsigned long long, unsigned long long, unsigned long long);
    const auto *a=arguments;
    volatile int result=reinterpret_cast<Entry>(const_cast<void *>(function))(a[0],a[1],a[2],a[3],a[4],a[5]);
    return result;
}
BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if(reason==DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(module);
    return TRUE;
}
