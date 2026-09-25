#pragma once
#include "device.h"
#include "ngx_contract.h"
#include "core_state_sentinel.h"

void CheckNgx(NVSDK_NGX_Result result, const char *operation);
void AuditNgxParameters(bool enabled);

class Ngx {
public:
    Ngx()=default;
    ~Ngx();
    Ngx(const Ngx &)=delete;
    Ngx &operator=(const Ngx &)=delete;
    void Initialize(Device &device, const Options &options, unsigned width, unsigned height);
    void Evaluate(Device &device, NgxFrame &frame);
    void Shutdown(Device &device);
    ID3D12Resource *Output() const { return output.Get(); }
private:
    HMODULE library=nullptr;
    Device *owner=nullptr;
    NVSDK_NGX_Result Close() noexcept;
    ComPtr<ID3D12Device> gpu;
    ComPtr<ID3D12Resource> output;
    NVSDK_NGX_Parameter *parameters=nullptr;
    NVSDK_NGX_Handle *feature=nullptr;
    decltype(&NVSDK_NGX_D3D12_ReleaseFeature) release=nullptr;
    decltype(&NVSDK_NGX_D3D12_DestroyParameters) destroy=nullptr;
    decltype(&NVSDK_NGX_D3D12_Shutdown1) shutdown=nullptr;
    bool started=false, rr=false, evaluated=false;
    // Some driver versions retain init arguments beyond the call.
    std::wstring dataDirectory;
    const wchar_t *searchPaths[1]{};
    NVSDK_NGX_FeatureCommonInfo common{};
    CoreStateSentinel stateSentinel;
};
