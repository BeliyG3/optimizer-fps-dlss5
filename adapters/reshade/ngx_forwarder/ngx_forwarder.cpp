// Call forwarder for the NGX feature-18 interposer.
//
// nvngx_dlssnr.dll resolves the module owning the return address of every entry call and requires
// that module's path to contain "nvngx.dll" (the driver core is _nvngx.dll); anything else gets
// FAIL_PlatformError before an argument is looked at. The add-on's hook therefore never calls the
// snippet directly: it hands the target address here, and the call returns through this module,
// whose file name satisfies the check. The result goes through a volatile so the compiler cannot
// turn the call into a tail jump, which would leave this frame out of the return chain.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

struct ID3D12GraphicsCommandList;

extern "C" {

using PFN_Create = int(__cdecl *)(ID3D12GraphicsCommandList *, int, void *, void **);
using PFN_Evaluate = int(__cdecl *)(ID3D12GraphicsCommandList *, void *, void *, void *);
using PFN_Release = int(__cdecl *)(void *);

__declspec(dllexport) int pw_ngx_call_create(PFN_Create fn, ID3D12GraphicsCommandList *cmd, int featureId,
                                             void *params, void **outHandle)
{
    volatile int result = fn(cmd, featureId, params, outHandle);
    return result;
}

__declspec(dllexport) int pw_ngx_call_evaluate(PFN_Evaluate fn, ID3D12GraphicsCommandList *cmd, void *handle,
                                               void *params, void *callback)
{
    volatile int result = fn(cmd, handle, params, callback);
    return result;
}

__declspec(dllexport) int pw_ngx_call_release(PFN_Release fn, void *handle)
{
    volatile int result = fn(handle);
    return result;
}

} // extern "C"

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(module);
    return TRUE;
}
