#pragma once
#include "device.h"
#include "ngx_nr_log.h"
#include "ngx_nr_params.h"
#include <initializer_list>

// Direct host of nvngx_dlssnr.dll (DLSS Neural Rendering, NGX feature 18).
// The NGX core cannot create feature 18 with the available 310.8 runtime: the DLL's Authenticode
// hash does not verify, the core logs "nvLoadSignedLibraryW() failed ... The digital signature of
// the object did not verify", reports DLSSNR.FeatureInitResult 0xBAD00004 and CreateFeature(18)
// returns 0xBAD0000B. The bench therefore loads the DLL itself and calls its NGX entry points
// through the forwarder module nvngx.dll_pwbench12.dll (ngx_nr_forwarder.cpp), as renodx-dlss5 and
// the Optimizer FPS add-on do. Both DLLs are expected beside pw_bench12.exe.
class NrRuntime {
public:
    NrRuntime()=default;
    NrRuntime(const NrRuntime &)=delete;
    NrRuntime &operator=(const NrRuntime &)=delete;
    // Loads and initializes the runtime once per process; the DLLs stay loaded until exit.
    void Load(const std::filesystem::path &directory, ID3D12Device *device, int logLevel);
    bool Loaded() const { return snippet!=nullptr; }
    // DLSSNRComputeScalingRatioCallback for one PerfQualityValue; negative when the runtime refuses it.
    float ScalingRatio(int perfQuality);
    NVSDK_NGX_Result Create(ID3D12GraphicsCommandList *list, NVSDK_NGX_Parameter *parameters, NVSDK_NGX_Handle **handle) const;
    NVSDK_NGX_Result Evaluate(ID3D12GraphicsCommandList *list, NVSDK_NGX_Handle *handle, NVSDK_NGX_Parameter *parameters) const;
    NVSDK_NGX_Result Release(NVSDK_NGX_Handle *handle) const;
    NrLogTail log;
private:
    const void *Entry(const char *name) const;
    NVSDK_NGX_Result Call(const void *function, std::initializer_list<unsigned long long> arguments) const;
    HMODULE forwarder=nullptr, snippet=nullptr;
    int (*call)(const void *, const unsigned long long *)=nullptr;
    const void *create=nullptr, *evaluate=nullptr, *release=nullptr;
    std::wstring dataPath; // the runtime may keep the pointer it was initialized with
    NgxParameterMap initParameters, capabilities;
};
