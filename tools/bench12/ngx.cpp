#include "ngx.h"
#include <cstring>

namespace {
decltype(&NVSDK_NGX_D3D12_CreateFeature) createFeature=nullptr;
decltype(&NVSDK_NGX_D3D12_EvaluateFeature) evaluateFeature=nullptr;
bool audit=false;
template<class T> T Export(HMODULE library, const char *name)
{
    FARPROC address=GetProcAddress(library,name);
    if(!address) throw std::runtime_error(std::string("Missing NGX export: ")+name);
    static_assert(sizeof(T)==sizeof(address)); T result;
    std::memcpy(&result,&address,sizeof(result)); return result;
}
std::filesystem::path DriverNgx()
{
    const std::filesystem::path store=L"C:\\Windows\\System32\\DriverStore\\FileRepository";
    WIN32_FIND_DATAW data{};
    HANDLE search=FindFirstFileW((store/L"nv_dispi.inf_amd64_*").c_str(),&data);
    if(search==INVALID_HANDLE_VALUE) throw std::runtime_error("No NVIDIA nv_dispi.inf_amd64_* driver-store directory found");
    // Old driver packages stay in the store after an update: the live core is the newest one.
    std::filesystem::path result; std::filesystem::file_time_type newest{};
    do {
        if(!(data.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)) continue;
        auto candidate=store/data.cFileName/L"_nvngx.dll"; std::error_code error;
        auto written=std::filesystem::last_write_time(candidate,error);
        if(!error && (result.empty() || written>newest)) { result=candidate; newest=written; }
    } while(FindNextFileW(search,&data));
    FindClose(search);
    if(result.empty()) throw std::runtime_error("No _nvngx.dll in NVIDIA driver store; install an NVIDIA driver");
    return result;
}
void NVSDK_CONV NgxLog(const char *message, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature)
{
    std::fprintf(stderr,"[ngx] %s\n",message);
}
}
void AuditNgxParameters(bool enabled) { audit=enabled; }
void CheckNgx(NVSDK_NGX_Result result, const char *operation)
{
    if(NVSDK_NGX_FAILED(result)) {
        char message[256]; std::snprintf(message,sizeof(message),"%s failed (NGX 0x%08X); see [ngx] log",operation,unsigned(result));
        throw std::runtime_error(message);
    }
}
// The SDK helpers call these C exports; the driver DLL is deliberately loaded at runtime.
NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_D3D12_CreateFeature(ID3D12GraphicsCommandList *list,
    NVSDK_NGX_Feature id, NVSDK_NGX_Parameter *parameters, NVSDK_NGX_Handle **handle)
{
    return createFeature ? createFeature(list,id,parameters,handle) : NVSDK_NGX_Result_FAIL_NotInitialized;
}
NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_D3D12_EvaluateFeature(ID3D12GraphicsCommandList *list,
    const NVSDK_NGX_Handle *handle, const NVSDK_NGX_Parameter *parameters, PFN_NVSDK_NGX_ProgressCallback callback)
{
    return evaluateFeature ? evaluateFeature(list,handle,parameters,callback) : NVSDK_NGX_Result_FAIL_NotInitialized;
}
NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_D3D12_EvaluateFeature_C(ID3D12GraphicsCommandList *list,
    const NVSDK_NGX_Handle *handle, const NVSDK_NGX_Parameter *parameters, PFN_NVSDK_NGX_ProgressCallback_C callback)
{
    // Every used helper supplies null; do not reinterpret the two different callback ABIs.
    if(callback) return NVSDK_NGX_Result_FAIL_InvalidParameter;
    return NVSDK_NGX_D3D12_EvaluateFeature(list,handle,parameters,nullptr);
}
void NVSDK_CONV NVSDK_NGX_Parameter_SetUI(NVSDK_NGX_Parameter *p, const char *name, unsigned value)
{ if(audit) std::printf("[ngx parameter] %s = %u\n",name,value); p->Set(name,value); }
void NVSDK_CONV NVSDK_NGX_Parameter_SetI(NVSDK_NGX_Parameter *p, const char *name, int value)
{ if(audit) std::printf("[ngx parameter] %s = %d\n",name,value); p->Set(name,value); }
void NVSDK_CONV NVSDK_NGX_Parameter_SetF(NVSDK_NGX_Parameter *p, const char *name, float value)
{ if(audit) std::printf("[ngx parameter] %s = %.9g\n",name,double(value)); p->Set(name,value); }
void NVSDK_CONV NVSDK_NGX_Parameter_SetD3d12Resource(NVSDK_NGX_Parameter *p, const char *name, ID3D12Resource *value)
{ if(audit) std::printf("[ngx parameter] %s = resource %p\n",name,static_cast<void *>(value)); p->Set(name,value); }
void NVSDK_CONV NVSDK_NGX_Parameter_SetVoidPointer(NVSDK_NGX_Parameter *p, const char *name, void *value)
{
    if(audit) {
        std::printf("[ngx parameter] %s = %p",name,value);
        if(value && (std::strcmp(name,NVSDK_NGX_Parameter_DLSS_WORLD_TO_VIEW_MATRIX)==0 ||
            std::strcmp(name,NVSDK_NGX_Parameter_DLSS_VIEW_TO_CLIP_MATRIX)==0)) {
            for(unsigned i=0;i<16;++i) std::printf(" %.9g",double(static_cast<float *>(value)[i]));
        }
        std::puts("");
    }
    p->Set(name,value);
}
void Ngx::Initialize(Device &d, const Options &o, unsigned width, unsigned height)
{
    owner=&d; gpu=d.gpu;
    const auto directory=ExecutableDirectory();
    const auto driver=DriverNgx(); library=LoadLibraryExW(driver.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if(!library) throw std::runtime_error("Cannot load "+driver.string()+" (Win32 "+std::to_string(GetLastError())+")");
    d.RetainDriverModule(library);
    // RR can retain worker/TLS callbacks after Shutdown1. Pin the driver core for
    // process lifetime; unloading it can invalidate callbacks even after GPU idle.
    HMODULE pinned=nullptr;
    // Not fatal: with an NGX interposer (OptiScaler as dxgi.dll) the load above is redirected and no
    // module is registered under the driver path; the interposer pins what it needs itself.
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN,driver.c_str(),&pinned))
        std::printf("[info] NGX core is not loaded under its driver path (an interposer answers NGX calls)\n");
    std::printf("[info] NGX driver: %ls\n",driver.c_str());
    // The driver core exports the plain entry points, not the SDK library's wrappers; its ProjectID
    // variant also orders its arguments differently, so the application-id form is used.
    auto init=Export<decltype(&NVSDK_NGX_D3D12_Init)>(library,"NVSDK_NGX_D3D12_Init");
    auto allocate=Export<decltype(&NVSDK_NGX_D3D12_AllocateParameters)>(library,"NVSDK_NGX_D3D12_AllocateParameters");
    release=Export<decltype(release)>(library,"NVSDK_NGX_D3D12_ReleaseFeature");
    destroy=Export<decltype(destroy)>(library,"NVSDK_NGX_D3D12_DestroyParameters");
    shutdown=Export<decltype(shutdown)>(library,"NVSDK_NGX_D3D12_Shutdown1");
    createFeature=Export<decltype(createFeature)>(library,"NVSDK_NGX_D3D12_CreateFeature");
    evaluateFeature=Export<decltype(evaluateFeature)>(library,"NVSDK_NGX_D3D12_EvaluateFeature");
    dataDirectory=directory.wstring(); searchPaths[0]=dataDirectory.c_str(); common={};
    common.PathListInfo={searchPaths,1}; common.LoggingInfo={NgxLog,NVSDK_NGX_LOGGING_LEVEL_ON,true};
    // No feature-specific loading or initialization precedes this common call.
    std::printf("[info] NGX init: appId=0x24480451 sdk=0x%X path=%ls\n",unsigned(NVSDK_NGX_Version_API),directory.c_str());
    auto initResult=init(0x24480451ull,dataDirectory.c_str(),gpu.Get(),&common,NVSDK_NGX_Version_API);
    if(NVSDK_NGX_FAILED(initResult)) {
        // Seen on driver 616.92: the init with a feature-info block (search path, log callback) is
        // refused as OutOfDate on every run but the first in a directory, the bare one is accepted.
        std::fprintf(stderr,"[ngx] init with feature info failed (0x%08X); retrying without it\n",unsigned(initResult));
        initResult=init(0x24480451ull,dataDirectory.c_str(),gpu.Get(),nullptr,NVSDK_NGX_Version_API);
    }
    if(initResult==NVSDK_NGX_Result_FAIL_OutOfDate)
        std::fprintf(stderr,"[ngx] 0xBAD0000C is OutOfDate: check driver/core and feature DLL versions; SR/RR use identical init arguments\n");
    CheckNgx(initResult,"NGX D3D12 init"); started=true;
    rr=o.upscaler=="rr";
    const auto dll=directory/(rr ? L"nvngx_dlssd.dll" : L"nvngx_dlss.dll");
    if(!std::filesystem::is_regular_file(dll)) throw std::runtime_error("Missing DLSS feature DLL: "+dll.string()+"; place it beside pw_bench12.exe");
    CheckNgx(allocate(&parameters),"NGX allocate parameters");
    if(!parameters) throw std::runtime_error("NGX returned a null parameter map");
    output=d.Texture(d.width,d.height,DXGI_FORMAT_R16G16B16A16_FLOAT,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    d.Begin(); AuditNgxParameters(true);
    NVSDK_NGX_Result result;
    if(rr) {
        auto p=RrCreate(width,height,d.width,d.height,o);
        result=NGX_D3D12_CREATE_DLSSD_EXT(d.list.Get(),1,1,&feature,parameters,&p);
    } else {
        NVSDK_NGX_DLSS_Create_Params p{};
        p.Feature={width,height,d.width,d.height,NgxQuality(o.renderScale)}; p.InFeatureCreateFlags=NgxFlags(o.reverse);
        result=NGX_D3D12_CREATE_DLSS_EXT(d.list.Get(),1,1,&feature,parameters,&p);
    }
    AuditNgxParameters(false);
    // Creation may record GPU initialization work. Retire it before testing the return code.
    d.Submit(); CheckNgx(result,rr ? "DLSS RR create" : "DLSS SR create");
    if(!feature) throw std::runtime_error("NGX returned a null feature handle");
    std::printf("[info] NGX %s ready, quality %d, flags %d\n",o.upscaler.c_str(),int(NgxQuality(o.renderScale)),NgxFlags(o.reverse));
}
Ngx::~Ngx()
{
    // Also cover exceptions during creation/render, while the owning Device is alive.
    if(owner && started) {
        try { owner->Wait(); } catch(const std::exception &error) { std::fprintf(stderr,"[ngx cleanup] %s\n",error.what()); }
    }
    Close();
}
void Ngx::Shutdown(Device &d) { d.Wait(); CheckNgx(Close(),"NGX teardown"); }
NVSDK_NGX_Result Ngx::Close() noexcept
{
    NVSDK_NGX_Result status=NVSDK_NGX_Result_Success;
    auto report=[&](NVSDK_NGX_Result result, const char *operation) {
        if(NVSDK_NGX_FAILED(result)) { status=result; std::fprintf(stderr,"[ngx cleanup] %s: 0x%08X\n",operation,unsigned(result)); }
    };
    if(feature && release) report(release(feature),"release feature");
    feature=nullptr;
    if(parameters && destroy) report(destroy(parameters),"destroy parameters");
    parameters=nullptr;
    // Driver 616.92: Shutdown1 itself dies with an access violation once a feature has been evaluated
    // (release and destroy return success first). The core stays pinned for the life of the process, so
    // after an evaluate the call is skipped; a session that never evaluated shuts down normally.
    if(started && shutdown && !evaluated) report(shutdown(gpu.Get()),"shutdown");
    started=false; evaluated=false;
    // COM objects must release their references while the driver module is still loaded.
    output.Reset(); gpu.Reset(); owner=nullptr;
    // Device owns the LoadLibrary reference until its queue/list/resources are destroyed.
    if(library) { createFeature=nullptr; evaluateFeature=nullptr; library=nullptr; }
    return status;
}
