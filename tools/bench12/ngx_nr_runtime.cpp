#include "ngx_nr_runtime.h"
#include "ngx.h"
#include <cstring>

namespace {
constexpr wchar_t ForwarderName[]=L"nvngx.dll_pwbench12.dll";
constexpr wchar_t RuntimeName[]=L"nvngx_dlssnr.dll";
template<class T> unsigned long long Arg(T *pointer) { return static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(pointer)); }
}
const void *NrRuntime::Entry(const char *name) const
{
    FARPROC address=GetProcAddress(snippet,name);
    if(!address) throw std::runtime_error(std::string("nvngx_dlssnr.dll lacks the export ")+name);
    const void *result=nullptr; static_assert(sizeof(result)==sizeof(address));
    std::memcpy(&result,&address,sizeof(result)); return result;
}
NVSDK_NGX_Result NrRuntime::Call(const void *function, std::initializer_list<unsigned long long> arguments) const
{
    unsigned long long values[6]{}; size_t count=0;
    for(auto value:arguments) if(count<6) values[count++]=value;
    return static_cast<NVSDK_NGX_Result>(call(function,values));
}
void NrRuntime::Load(const std::filesystem::path &directory, ID3D12Device *device, int logLevel)
{
    if(snippet) return;
    const auto forwarderPath=directory/ForwarderName, runtimePath=directory/RuntimeName;
    for(const auto &path:{forwarderPath,runtimePath})
        if(!std::filesystem::is_regular_file(path)) throw std::runtime_error("Missing "+path.string()+"; place it beside pw_bench12.exe");
    forwarder=LoadLibraryExW(forwarderPath.c_str(),nullptr,LOAD_WITH_ALTERED_SEARCH_PATH);
    if(!forwarder) throw std::runtime_error("Cannot load "+forwarderPath.string()+" (Win32 "+std::to_string(GetLastError())+")");
    FARPROC entry=GetProcAddress(forwarder,"pw_bench_ngx_call");
    if(!entry) throw std::runtime_error("The NR forwarder lacks pw_bench_ngx_call; rebuild with build.cmd");
    std::memcpy(&call,&entry,sizeof(call));
    log.Arm(directory,logLevel);
    snippet=LoadLibraryExW(runtimePath.c_str(),nullptr,LOAD_WITH_ALTERED_SEARCH_PATH);
    if(!snippet) {
        const auto error=GetLastError(); log.Attach();
        throw std::runtime_error("Cannot load "+runtimePath.string()+" (Win32 "+std::to_string(error)+")");
    }
    create=Entry("NVSDK_NGX_D3D12_CreateFeature"); evaluate=Entry("NVSDK_NGX_D3D12_EvaluateFeature");
    release=Entry("NVSDK_NGX_D3D12_ReleaseFeature");
    dataPath=directory.wstring();
    // Same application id as the NGX core init of the DLSS path; the data path receives the runtime log.
    const auto result=Call(Entry("NVSDK_NGX_D3D12_Init_Ext"),{0x24480451ull,Arg(dataPath.c_str()),Arg(device),
        static_cast<unsigned long long>(NVSDK_NGX_Version_API),Arg(static_cast<NVSDK_NGX_Parameter *>(&initParameters))});
    log.Attach();
    CheckNgx(result,"DLSSNR runtime init");
    const auto populate=Call(Entry("NVSDK_NGX_D3D12_PopulateParameters_Impl"),{Arg(static_cast<NVSDK_NGX_Parameter *>(&capabilities))});
    if(NVSDK_NGX_FAILED(populate)) std::printf("[nr] PopulateParameters_Impl failed (0x%08X); no scaling-ratio callback\n",unsigned(populate));
    std::printf("[nr] %ls initialized directly through %ls (the NGX core refuses this runtime's signature)\n",RuntimeName,ForwarderName);
}
float NrRuntime::ScalingRatio(int perfQuality)
{
    void *callback=nullptr;
    if(NVSDK_NGX_FAILED(capabilities.Get("DLSSNRComputeScalingRatioCallback",&callback)) || !callback) return -1;
    NgxParameterMap query; query.Set("PerfQualityValue",perfQuality);
    if(NVSDK_NGX_FAILED(Call(callback,{Arg(static_cast<NVSDK_NGX_Parameter *>(&query))}))) return -1;
    float ratio=-1; query.Get("DLSSNR.ScalingRatio",&ratio); return ratio;
}
NVSDK_NGX_Result NrRuntime::Create(ID3D12GraphicsCommandList *list, NVSDK_NGX_Parameter *parameters, NVSDK_NGX_Handle **handle) const
{
    return Call(create,{Arg(list),18,Arg(parameters),Arg(handle)});
}
NVSDK_NGX_Result NrRuntime::Evaluate(ID3D12GraphicsCommandList *list, NVSDK_NGX_Handle *handle, NVSDK_NGX_Parameter *parameters) const
{
    return Call(evaluate,{Arg(list),Arg(handle),Arg(parameters),0});
}
NVSDK_NGX_Result NrRuntime::Release(NVSDK_NGX_Handle *handle) const
{
    return Call(release,{Arg(handle)});
}
