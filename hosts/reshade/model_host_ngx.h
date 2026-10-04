#pragma once
#include "hosts/reshade/ngx_params.h"
#include "hosts/reshade/ngx_forwarder_calls.h"
#include "core/api/ofps_core.h"
#include <cstdint>
namespace ofps::reshade {
struct NgxModelCalls {
    int (*create)(ID3D12GraphicsCommandList *cmd, int featureId, void *params, void **outHandle) = nullptr;
    int (*evaluate)(ID3D12GraphicsCommandList *cmd, void *handle, void *params, void *callback) = nullptr;
    int (*release)(void *handle) = nullptr;
};
struct NgxBlockSnapshot {
    ID3D12Resource *color = nullptr, *depth = nullptr, *motion = nullptr, *output = nullptr;
    ID3D12Resource *ui = nullptr, *uiAlpha = nullptr, *backbuffer = nullptr;
    struct Size {
        unsigned int w = 0, h = 0;
        bool hadW = false, hadH = false;
    } size[3];
    Subrect rect[4];
    float mvScaleX = 1.0f, mvScaleY = 1.0f;
    bool hadMvScaleX = false, hadMvScaleY = false;
    unsigned int depthInverted = 0;
    bool hadDepthInverted = false;
    unsigned int reset = 0;
    bool hadReset = false;
    unsigned int uiCorrection = 0;
    bool hadUiCorrection = false;
};
class ModelHostNgx final : public IOfpsModelHost {
public:
    explicit ModelHostNgx(void *params) : calls_{&CallCreate, &CallEvaluate, &CallRelease}, params_(params) {}
    int LastNgxResult() const { return lastNgxResult_; }
    bool ModelWasCalled() const { return modelCalled_; }
    void SetHostHandle(void *handle) { hostHandle_ = handle; }
    // Menu mode's traced evaluate (menu_params.h MenuTracedEvaluate): the model evaluate goes through `evaluate` instead of CallEvaluate.
    void SetEvaluate(int (*evaluate)(ID3D12GraphicsCommandList *, void *, void *, void *)) { calls_.evaluate = evaluate; }
    void *HostHandle() const { return hostHandle_; }
    void BeginFrame(void *params, void *callback);
    // After an evaluate that ran on a temporary block (menu mode's exit reset wraps the host's block, menu_params.h):
    // the host's own block is what this host keeps after the evaluate, never the gone wrapper. (After a menu call, menu
    // mode's own block, which is never freed, stays until the next host evaluate's BeginFrame; params_ is read only
    // inside an evaluate or a creation.)
    void KeepBlock(void *params) { params_ = params; }
    // Menu mode, stage 3: while the core evaluates a menu frame, no model is created from menu mode's block (a model the
    // game would keep); the core's creation fails and the game's next evaluate creates it from the game's block.
    void RefuseCreate(bool refuse) { refuseCreate_ = refuse; createRefused_ = false; }
    bool CreateRefused() const { return createRefused_; }
    const NgxBlockSnapshot &Snapshot() const { return snapshot_; }
    int CreateModel(ID3D12GraphicsCommandList *cmd, uint32_t w, uint32_t h, uint32_t withholdUi,
                    void **handle) override;
    int ReleaseModel(void *handle) override;
    int RunModel(ID3D12GraphicsCommandList *cmd, void *handle, const OfpsModelInputs *inputs) override;
    int PrepareModelInput(ID3D12GraphicsCommandList *cmd, const OfpsFrameInputs *frame,
                          OfpsResource *modelColor, OfpsResource *frameBefore) override;
    int ResolveAnswer(ID3D12GraphicsCommandList *cmd, const OfpsResource *answer,
                      const OfpsFrameInputs *frame) override;
    uint32_t DescribeInputs(char *out, uint32_t size) override;
    uint32_t ModelReady(void *handle) override;
    void EndFrame(ID3D12GraphicsCommandList *cmd, const OfpsFrameInputs *frame,
                  const OfpsEvalResult *result) override;

private:
    enum Key : unsigned {
        KeyColor = 1u << 0,
        KeyDepth = 1u << 1,
        KeyMotion = 1u << 2,
        KeyOutput = 1u << 3,
        KeyUi = 1u << 4,
        KeyUiAlpha = 1u << 5,
        KeyBackbuffer = 1u << 6,
        KeySizes = 1u << 7,
        KeyRectColor = 1u << 8,
        KeyMvScale = 1u << 12,
        KeyDepthInverted = 1u << 13,
        KeyReset = 1u << 14,
        KeyUiCorrection = 1u << 15,
    };
    void PutResource(const char *name, ID3D12Resource *wanted, ID3D12Resource *snapshot, unsigned key);
    void PutSizes(std::uint32_t w, std::uint32_t h);
    void PutRect(unsigned index, const OfpsRect &wanted);
    void Restore();
    int lastNgxResult_ = kNgxSuccess;
    bool modelCalled_ = false;
    bool refuseCreate_ = false, createRefused_ = false;
    void *hostHandle_ = nullptr;
    NgxModelCalls calls_;
    void *params_ = nullptr;
    void *callback_ = nullptr;
    NgxBlockSnapshot snapshot_;
    unsigned written_ = 0;
};
} // namespace ofps::reshade
