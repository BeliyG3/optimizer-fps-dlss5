#include "hosts/reshade/ngx_forwarder_calls.h"
#include "hosts/reshade/shell_host.h"
#include <cstdio>
#include <cstring>

namespace ofps::reshade {
namespace {

using PFN_FwdCreate = int(__cdecl *)(PFN_Create, void *, int, void *, void **);
using PFN_FwdEvaluate = int(__cdecl *)(PFN_Evaluate, void *, void *, void *, void *);
using PFN_FwdRelease = int(__cdecl *)(PFN_Release, void *);
PFN_FwdCreate g_fwdCreate = nullptr;
PFN_FwdEvaluate g_fwdEvaluate = nullptr;
PFN_FwdRelease g_fwdRelease = nullptr;


} // namespace


bool LoadForwarder(const std::wstring &directory, std::string *error)
{
    if (g_fwdCreate != nullptr) return true;
    const std::wstring path = directory + L"\\nvngx.dll_optimizerfps.dll";
    HMODULE module = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (module == nullptr) {
        if (error) *error = "nvngx.dll_optimizerfps.dll is missing beside the add-on";
        return false;
    }
    g_fwdCreate = reinterpret_cast<PFN_FwdCreate>(GetProcAddress(module, "pw_ngx_call_create"));
    g_fwdEvaluate = reinterpret_cast<PFN_FwdEvaluate>(GetProcAddress(module, "pw_ngx_call_evaluate"));
    g_fwdRelease = reinterpret_cast<PFN_FwdRelease>(GetProcAddress(module, "pw_ngx_call_release"));
    if (g_fwdCreate == nullptr || g_fwdEvaluate == nullptr || g_fwdRelease == nullptr) {
        if (error) *error = "nvngx.dll_optimizerfps.dll lacks its exports";
        g_fwdCreate = nullptr;
        g_fwdEvaluate = nullptr;
        g_fwdRelease = nullptr;
        return false;
    }
    return true;
}

bool ForwarderLoaded() { return g_fwdCreate != nullptr; }

int ForwardCreate(PFN_Create real, void *context, int featureId, void *params, void **outHandle)
{
    return g_fwdCreate(real, context, featureId, params, outHandle);
}

int ForwardEvaluate(PFN_Evaluate real, void *context, void *handle, void *params, void *callback)
{
    return g_fwdEvaluate(real, context, handle, params, callback);
}

int ForwardRelease(PFN_Release real, void *handle) { return g_fwdRelease(real, handle); }

bool DescribeExportPrologue(const char *hookName, const char *exportName, void *fn)
{
    const auto *bytes = static_cast<const unsigned char *>(fn);
    if (bytes[0] != 0xE9) return false;
    std::int32_t rel = 0;
    std::memcpy(&rel, bytes + 1, sizeof(rel));
    void *target = static_cast<unsigned char *>(fn) + 5 + rel;
    HMODULE module = nullptr;
    char owner[MAX_PATH] = "?";
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCWSTR>(target), &module) && GetModuleFileNameA(module, owner, MAX_PATH)) {
        const char *slash = std::strrchr(owner, '\\');
        if (slash) std::memmove(owner, slash + 1, std::strlen(slash + 1) + 1);
    }
    char message[1024];
    std::snprintf(message, sizeof(message), "%s: %s already starts with a jump to %p (%s); that hook will run inside ours", hookName, exportName, target, owner);
    Host().Log(OFPS_LOG_INFO, message);
    return true;
}

HookShim &Shim() { static HookShim shim; return shim; }
int CallCreate(ID3D12GraphicsCommandList *cmd, int featureId, void *params, void **outHandle)
{ return ForwardCreate(Shim().realCreate, cmd, featureId, params, outHandle); }
int CallEvaluate(ID3D12GraphicsCommandList *cmd, void *handle, void *params, void *callback)
{ return ForwardEvaluate(Shim().realEvaluate, cmd, handle, params, callback); }
int CallRelease(void *handle) { return ForwardRelease(Shim().realRelease, handle); }

} // namespace ofps::reshade
