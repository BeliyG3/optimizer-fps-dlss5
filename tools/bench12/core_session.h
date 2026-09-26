#pragma once
#include "core/api/ofps_core_loader.h"
#include "core_model_host.h"
class CoreSession final : public IOfpsHost {
public:
    CoreSession(NrRuntime &runtime, const NrCreateInfo &create, const NrFrame &frame)
        : model(runtime, create, frame) {}
    ~CoreSession();
    void Announce();
    void Open(const std::filesystem::path &, ID3D12Device *, ID3D12CommandQueue *, int, int, int);
    int Create(ID3D12GraphicsCommandList *, ID3D12Device *, unsigned, unsigned);
    int Evaluate(ID3D12GraphicsCommandList *, const OfpsFrameInputs &);
    void Submitted(ID3D12CommandQueue *, ID3D12CommandList *);
    void RegisterQueue(ID3D12Device *device, ID3D12CommandQueue *extra); // --nr-list compute
    bool Close();
    void Log(OfpsLogLevel, const char *) override;
    void OnEvent(OfpsEvent, const OfpsEventData *) override;
    bool Ready() const { return feature != nullptr; }
    CoreModelHost model;
private:
    ofps::CoreLoader loader;
    IOfpsFeature *feature = nullptr;
    ID3D12CommandQueue *queue = nullptr;
    ID3D12Device *device_ = nullptr;
    int mode_ = -1, temporal_ = -1, warpPath_ = -1;
    HANDLE event = nullptr;
    bool featureReleased = false;
    bool hadFeature = false;
    bool reportedWarpPath_ = false;
};
