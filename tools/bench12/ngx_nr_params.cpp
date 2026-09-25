#include "ngx_nr_params.h"
#include <cstdio>

void NgxParameterMap::Put(const char *name, const Value &value)
{
    if(!name) return;
    std::lock_guard<std::mutex> lock(mutex);
    values[name]=value;
    if(!audit) return;
    if(value.kind==Kind::Pointer) std::printf("[nr parameter] %s = resource %p\n",name,value.pointer);
    else if(value.kind==Kind::Real) std::printf("[nr parameter] %s = %.9g\n",name,value.real);
    else if(value.kind==Kind::Signed) std::printf("[nr parameter] %s = %lld\n",name,static_cast<long long>(value.bits));
    else std::printf("[nr parameter] %s = %llu\n",name,value.bits);
}
void NgxParameterMap::Set(const char *n, unsigned long long v) { Put(n,{Kind::Unsigned,v,double(v),nullptr,"uint64"}); }
void NgxParameterMap::Set(const char *n, float v) { Put(n,{Kind::Real,0,double(v),nullptr,"float"}); }
void NgxParameterMap::Set(const char *n, double v) { Put(n,{Kind::Real,0,v,nullptr,"double"}); }
void NgxParameterMap::Set(const char *n, unsigned int v) { Put(n,{Kind::Unsigned,v,double(v),nullptr,"uint32"}); }
void NgxParameterMap::Set(const char *n, int v) { Put(n,{Kind::Signed,static_cast<unsigned long long>(static_cast<long long>(v)),double(v),nullptr,"int32"}); }
void NgxParameterMap::Set(const char *n, ID3D11Resource *v) { Put(n,{Kind::Pointer,0,0,v,"d3d11"}); }
void NgxParameterMap::Set(const char *n, ID3D12Resource *v) { Put(n,{Kind::Pointer,0,0,v,"d3d12"}); }
void NgxParameterMap::Set(const char *n, void *v) { Put(n,{Kind::Pointer,0,0,v,"pointer"}); }

template<class T> NVSDK_NGX_Result NgxParameterMap::Number(const char *name, T *out) const
{
    if(!name || !out) return NVSDK_NGX_Result_FAIL_InvalidParameter;
    std::lock_guard<std::mutex> lock(mutex);
    auto found=values.find(name);
    if(found==values.end() || found->second.kind==Kind::Pointer) return NVSDK_NGX_Result_FAIL_UnsupportedParameter;
    const auto &v=found->second;
    if(v.kind==Kind::Real) *out=static_cast<T>(v.real);
    else if(v.kind==Kind::Signed) *out=static_cast<T>(static_cast<long long>(v.bits));
    else *out=static_cast<T>(v.bits);
    return NVSDK_NGX_Result_Success;
}
template<class T> NVSDK_NGX_Result NgxParameterMap::Pointer(const char *name, T **out) const
{
    if(!name || !out) return NVSDK_NGX_Result_FAIL_InvalidParameter;
    std::lock_guard<std::mutex> lock(mutex);
    auto found=values.find(name);
    if(found==values.end() || found->second.kind!=Kind::Pointer) return NVSDK_NGX_Result_FAIL_UnsupportedParameter;
    *out=static_cast<T *>(found->second.pointer);
    return NVSDK_NGX_Result_Success;
}
NVSDK_NGX_Result NgxParameterMap::Get(const char *n, unsigned long long *v) const { return Number(n,v); }
NVSDK_NGX_Result NgxParameterMap::Get(const char *n, float *v) const { return Number(n,v); }
NVSDK_NGX_Result NgxParameterMap::Get(const char *n, double *v) const { return Number(n,v); }
NVSDK_NGX_Result NgxParameterMap::Get(const char *n, unsigned int *v) const { return Number(n,v); }
NVSDK_NGX_Result NgxParameterMap::Get(const char *n, int *v) const { return Number(n,v); }
NVSDK_NGX_Result NgxParameterMap::Get(const char *n, ID3D11Resource **v) const { return Pointer(n,v); }
NVSDK_NGX_Result NgxParameterMap::Get(const char *n, ID3D12Resource **v) const { return Pointer(n,v); }
NVSDK_NGX_Result NgxParameterMap::Get(const char *n, void **v) const { return Pointer(n,v); }
void NgxParameterMap::Reset() { std::lock_guard<std::mutex> lock(mutex); values.clear(); }
