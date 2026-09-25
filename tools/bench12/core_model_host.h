#pragma once
#include "core/api/ofps_core.h"
#include "core/api/ofps_settings_schema.h"
#include "ngx_nr_runtime.h"
#include "ngx_nr_contract.h"
class CoreModelHost final : public IOfpsModelHost {
public:
    CoreModelHost(NrRuntime &runtime, const NrCreateInfo &create, const NrFrame &frame)
        : runtime_(runtime), create_(create), frame_(frame) {}
    int CreateModel(ID3D12GraphicsCommandList *, uint32_t, uint32_t, uint32_t, void **) override;
    int ReleaseModel(void *) override;
    int RunModel(ID3D12GraphicsCommandList *, void *, const OfpsModelInputs *) override;
    int PrepareModelInput(ID3D12GraphicsCommandList *, const OfpsFrameInputs *, OfpsResource *, OfpsResource *) override;
    int ResolveAnswer(ID3D12GraphicsCommandList *, const OfpsResource *, const OfpsFrameInputs *) override;
    uint32_t DescribeInputs(char *, uint32_t) override;
    uint32_t ModelReady(void *) override;
    void EndFrame(ID3D12GraphicsCommandList *, const OfpsFrameInputs *, const OfpsEvalResult *) override;
    NVSDK_NGX_Result last = NVSDK_NGX_Result_Success;
    unsigned live = 0;
private:
    NrRuntime &runtime_;
    const NrCreateInfo &create_;
    const NrFrame &frame_;
    NgxParameterMap parameters_;
};
