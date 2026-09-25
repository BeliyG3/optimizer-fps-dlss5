#pragma once
#include "core/api/ofps_core.h"
#include "core/context.h"
#include "core/settings/status.h"
#include "core/settings/values.h"
#include <cstdint>
#include <memory>
#include <vector>
namespace ofps::core {
class Core;
class Feature final : public IOfpsFeature {
  public:
    Feature(Core &core, FeatureState *state, void *hostHandle, bool adoptedNow);
    int Evaluate(ID3D12GraphicsCommandList *cmd, const OfpsFrameInputs *frame, OfpsEvalResult *result) override;
    void *CurrentModelHandle() override;
    void RequestModelRebuild() override;
    void Release() override;
    FeatureState *State() const { return state_; }
    void *HostHandle() const { return hostHandle_; }

  private:
    Core &core_;
    FeatureState *state_;
    void *hostHandle_;
};
class Core final : public IOfpsCore {
  public:
    int AddHost(IOfpsHost *host);
    int CreateFeature(ID3D12GraphicsCommandList *cmd, const OfpsFeatureDesc *desc, IOfpsModelHost *modelHost,
                      IOfpsFeature **feature) override;
    int AdoptFeature(ID3D12GraphicsCommandList *cmd, const OfpsFeatureDesc *desc, void *existingHandle,
                     IOfpsModelHost *modelHost, IOfpsFeature **feature) override;
    void NotifyForeignReleased(void *handle) override;
    int SetSettings(const OfpsSettingsValues *values) override;
    int SetTemporalDiagnostics(const ofps::temporal::DiagnosticValuesV1 *values);
    int SetTemporalMotionSource(std::uint32_t source);
    void GetSettings(OfpsSettingsValues *values) override;
    void GetSettingRange(uint32_t settingId, float *lo, float *hi) override;
    void Status(OfpsStatus *status) override;
    uint32_t StatusLines(OfpsStatusRow *rows, uint32_t capacity) override;
    void LayoutPreview(OfpsLayoutPreview *preview) override;
    void RegisterQueue(ID3D12Device *resourceDevice, ID3D12CommandQueue *queue) override;
    void UnregisterQueue(ID3D12CommandQueue *queue) override;
    void OnCommandListExecuted(ID3D12CommandQueue *queue, ID3D12CommandList *list) override;
    void RetireResource(IUnknown *object, const OfpsFencePoint *extra) override;
    void SetDirectHost(IOfpsHost *host, uint32_t on) override;
    void SetHostCaps(IOfpsHost *host, const OfpsHostCaps *caps) override;
    void SetHostMotionGrid(uint32_t blockSize) override;
    void Housekeeping() override;
    void UnregisterHost(IOfpsHost *host) override;
    void Release() override;
    void Emit(OfpsEvent kind, const OfpsEventData &data);
    void ReportAbiException() noexcept;
    void LogAll(OfpsLogLevel level, const char *text);
    static void LogSink(bool warning, const char *message);
    void ApplyTemporal(const TemporalSettings &settings);
    void ForgetFeature(Feature *feature);

  private:
    bool ModelResolutionHost() const;
    struct HostEntry {
        IOfpsHost *host;
        OfpsHostCaps caps;
    };
    std::vector<HostEntry> hosts_;
    IOfpsHost *directHost_ = nullptr;
    std::vector<std::unique_ptr<Feature>> wrappers_;
    CoreSettings settings_;
    StatusStrings statusStrings_{};
    StatusLineBuffer statusLines_{};
    bool deviceRemoved_ = false;
    bool released_ = false;
    std::uintptr_t nextFeatureId_ = 0;
    bool DeviceRemovedNow();
    struct PendingRelease {
        IOfpsModelHost *host;
        void *handle;
    };
    std::vector<PendingRelease> pendingReleases_;
    inline static thread_local bool inCallback_ = false;
    bool initialized_ = false;
    std::vector<gpu::Grave> deferredRetirements_;
    void FlushRetirements();

  public:
    void QueueReleased(IOfpsModelHost *host, void *handle);
    void DrainReleased();
    bool InCallback() const { return inCallback_ || insideCore; }
};
Core &CoreInstance();
} // namespace ofps::core
