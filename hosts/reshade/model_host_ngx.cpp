#include "hosts/reshade/model_host_ngx.h"
#include "hosts/reshade/ngx_params.h"
#include "hosts/reshade/shell_host.h"
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>
namespace {
// Optional bench-owned diagnostic. No block writes and no dependency on the bench ABI in games.
void AuditBenchBlock(void *params, const char *operation) noexcept {
    using Audit = void (*)(void *, const char *, const char *);
    const auto address = GetProcAddress(GetModuleHandleW(nullptr), "OfpsBenchAuditNgx");
    if (!address) return;
    Audit audit = nullptr;
    static_assert(sizeof(audit) == sizeof(address));
    std::memcpy(&audit, &address, sizeof(audit));
    audit(params, "addon", operation);
}
}
namespace ofps::reshade { ShellStatus &ShellState() { static ShellStatus state;
return state;
} namespace { const char *const kResourceKeys[7] = {"DLSSNR.Color", "DLSSNR.Depth", "DLSSNR.MVec", "DLSSNR.Output", "DLSSNR.UI", "DLSSNR.UIAlpha", "DLSSNR.Backbuffer"};
} void ModelHostNgx::BeginFrame(void *params, void *callback) { modelCalled_ = false;
lastNgxResult_ = kNgxSuccess;
params_ = params;
callback_ = callback;
written_ = 0;
snapshot_ = NgxBlockSnapshot{};
if (params == nullptr) return;
NgxBlockSnapshot &s = snapshot_;
ID3D12Resource **const resources[7] = {&s.color, &s.depth, &s.motion, &s.output, &s.ui, &s.uiAlpha, &s.backbuffer}; for (unsigned i = 0; i < 7; ++i) *resources[i] = GetResource(params, kResourceKeys[i]); for (unsigned i = 0; i < 3; ++i) { s.size[i].hadW = GetUInt(params, kSizeKeys[i].w, &s.size[i].w); s.size[i].hadH = GetUInt(params, kSizeKeys[i].h, &s.size[i].h); } ID3D12Resource *const textures[4] = {s.color, s.depth, s.motion, s.output}; for (unsigned i = 0; i < 4; ++i) {
unsigned int w = 0, h = 0; if (textures[i] != nullptr) { const D3D12_RESOURCE_DESC d = textures[i]->GetDesc(); w = static_cast<unsigned int>(d.Width); h = d.Height; } s.rect[i] = ReadSubrect(params, kSubrectNames[i], w, h); } s.hadMvScaleX = GetFloat(params, "DLSSNR.MVecScaleX", &s.mvScaleX); if (!s.hadMvScaleX) s.mvScaleX = 1.0f;
s.hadMvScaleY = GetFloat(params, "DLSSNR.MVecScaleY", &s.mvScaleY); if (!s.hadMvScaleY) s.mvScaleY = 1.0f; s.hadDepthInverted = GetUInt(params, "DLSSNR.DepthInverted", &s.depthInverted); s.hadReset = GetUInt(params, "DLSSNR.Reset", &s.reset); s.hadUiCorrection = GetUInt(params, "DLSSNR.UICorrection", &s.uiCorrection); }
void ModelHostNgx::PutResource(const char *name, ID3D12Resource *wanted, ID3D12Resource *snapshot, unsigned key) { if (wanted == snapshot) return; SetResource(params_, name, wanted); written_ |= key; } void ModelHostNgx::PutSizes(std::uint32_t w, std::uint32_t h) { for (unsigned i = 0; i < 3; ++i) { const NgxBlockSnapshot::Size &s = snapshot_.size[i];
if (s.hadW && s.w != w) { SetUInt(params_, kSizeKeys[i].w, w); written_ |= KeySizes; } if (s.hadH && s.h != h) { SetUInt(params_, kSizeKeys[i].h, h); written_ |= KeySizes; } } } void ModelHostNgx::PutRect(unsigned index, const OfpsRect &r) { const Subrect &s = snapshot_.rect[index]; if (r.x == s.x && r.y == s.y && r.w == s.w && r.h == s.h) return;
WriteSubrect(params_, kSubrectNames[index], r.x, r.y, r.w, r.h); written_ |= KeyRectColor << index; } void ModelHostNgx::Restore() { if (written_ == 0 || params_ == nullptr) return; const NgxBlockSnapshot &s = snapshot_; ID3D12Resource *const resources[7] = {s.color, s.depth, s.motion, s.output, s.ui, s.uiAlpha, s.backbuffer}; for (unsigned i = 0; i < 7; ++i)
if (written_ & (KeyColor << i)) SetResource(params_, kResourceKeys[i], resources[i]); if (written_ & KeySizes) { for (unsigned i = 0; i < 3; ++i) { if (s.size[i].hadW) SetUInt(params_, kSizeKeys[i].w, s.size[i].w); if (s.size[i].hadH) SetUInt(params_, kSizeKeys[i].h, s.size[i].h); } } for (unsigned i = 0; i < 4; ++i) if (written_ & (KeyRectColor << i)) WriteSubrect(params_, kSubrectNames[i], s.rect[i].x, s.rect[i].y, s.rect[i].w, s.rect[i].h); if (written_ & KeyMvScale) {
// NGX has no per-key erase. Do not write an absent host scale during restore:
// the packed scale (normally 1.0) remains from our SetFloat; the host did not read it.
if (s.hadMvScaleX) SetFloat(params_, "DLSSNR.MVecScaleX", s.mvScaleX);
if (s.hadMvScaleY) SetFloat(params_, "DLSSNR.MVecScaleY", s.mvScaleY); } if (written_ & KeyDepthInverted) SetUInt(params_, "DLSSNR.DepthInverted", s.depthInverted); if (written_ & KeyReset) SetUInt(params_, "DLSSNR.Reset", s.reset); if (written_ & KeyUiCorrection) SetUInt(params_, "DLSSNR.UICorrection", s.uiCorrection); written_ = 0; } int ModelHostNgx::CreateModel(ID3D12GraphicsCommandList *cmd, uint32_t w, uint32_t h, uint32_t withholdUi, void **handle) {
if (params_ == nullptr || handle == nullptr || calls_.create == nullptr) return OFPS_E_STATE;
PutSizes(w, h);
if (withholdUi != 0 && snapshot_.hadUiCorrection) { SetUInt(params_, "DLSSNR.UICorrection", 0);
written_ |= KeyUiCorrection;
} *handle = nullptr;
AuditBenchBlock(params_, "create");
const int result = calls_.create(cmd, kFeatureNeuralRendering, params_, handle);
lastNgxResult_ = result;
ShellState().lastNgxResult = result;
Restore();
return result == kNgxSuccess ? OFPS_OK : OFPS_E_DEVICE;
} int ModelHostNgx::ReleaseModel(void *handle) {
if (handle == nullptr || calls_.release == nullptr) return OFPS_E_ARG; lastNgxResult_ = calls_.release(handle); return lastNgxResult_ == kNgxSuccess ? OFPS_OK : OFPS_E_DEVICE; } int ModelHostNgx::RunModel(ID3D12GraphicsCommandList *cmd, void *handle, const OfpsModelInputs *in) { if (params_ == nullptr || in == nullptr || in->size < sizeof(OfpsModelInputs) || calls_.evaluate == nullptr) return OFPS_E_STATE; const NgxBlockSnapshot &s = snapshot_;
PutResource("DLSSNR.Color", in->color.res, s.color, KeyColor); PutResource("DLSSNR.Depth", in->depth.res, s.depth, KeyDepth); PutResource("DLSSNR.MVec", in->motion.res, s.motion, KeyMotion); PutResource("DLSSNR.Output", in->output.res, s.output, KeyOutput); const bool withhold = in->withholdUi != 0; if (withhold && snapshot_.hadUiCorrection && snapshot_.uiCorrection != 0) {
SetUInt(params_, "DLSSNR.UICorrection", 0); written_ |= KeyUiCorrection; } PutResource("DLSSNR.UI", withhold ? nullptr : in->ui.res, s.ui, KeyUi); PutResource("DLSSNR.UIAlpha", withhold ? nullptr : in->uiAlpha.res, s.uiAlpha, KeyUiAlpha); PutResource("DLSSNR.Backbuffer", withhold ? nullptr : in->backbuffer.res, s.backbuffer, KeyBackbuffer); // An unchanged host colour on its own grid is an untouched evaluate, including
// dynamic resolution and NR upscaling. Preserve all three distinct size pairs.
const bool hostGrid = in->color.res == s.color &&
    in->width == s.rect[0].w && in->height == s.rect[0].h;
if (!hostGrid && (in->width != s.size[0].w || in->height != s.size[0].h))
    PutSizes(in->width, in->height);
const OfpsRect *const rects[4] = {&in->color.rect, &in->depth.rect, &in->motion.rect, &in->output.rect}; for (unsigned i = 0; i < 4; ++i) PutRect(i, *rects[i]); if (in->mvScaleX != s.mvScaleX || in->mvScaleY != s.mvScaleY ||
    ((written_ & (KeyColor | KeyMotion | KeySizes)) && (!s.hadMvScaleX || !s.hadMvScaleY))) { SetFloat(params_, "DLSSNR.MVecScaleX", in->mvScaleX); SetFloat(params_, "DLSSNR.MVecScaleY", in->mvScaleY);
written_ |= KeyMvScale;
} if (in->depthInverted != s.depthInverted) { SetUInt(params_, "DLSSNR.DepthInverted", in->depthInverted);
written_ |= KeyDepthInverted;
} if (in->reset != s.reset) { SetUInt(params_, "DLSSNR.Reset", in->reset);
written_ |= KeyReset;
} modelCalled_ = true;
AuditBenchBlock(params_, "evaluate");
const int result = calls_.evaluate(cmd, handle, params_, callback_);
Restore();
lastNgxResult_ = result;
ShellState().lastNgxResult = result;
return result == kNgxSuccess ? OFPS_OK : OFPS_E_DEVICE;
}
int ModelHostNgx::PrepareModelInput(ID3D12GraphicsCommandList *cmd, const OfpsFrameInputs *frame, OfpsResource *modelColor, OfpsResource *frameBefore) { (void) cmd; (void) frame; (void) modelColor; (void) frameBefore; return OFPS_S_IDENTITY; } int ModelHostNgx::ResolveAnswer(ID3D12GraphicsCommandList *cmd, const OfpsResource *answer, const OfpsFrameInputs *frame) { (void) cmd; (void) answer; (void) frame; return OFPS_S_IDENTITY;
} uint32_t ModelHostNgx::DescribeInputs(char *out, uint32_t size)
{
    if (!out || !size) return 0;
    out[0] = 0;
    if (!params_) return 0;
    float sx = 1.0f, sy = 1.0f;
    const bool read = ReadMotionScale(params_, &sx, &sy);
    char model[384] = {};
    DescribeModelKeys(params_, model, sizeof(model));
    std::snprintf(out, size, "%s, float setter slot %d getter slot %d\n"
        "(pointers may rotate every frame; logged on a change of formats/rects/scale)\n%s",
        read ? "read" : "NOT READ, defaulted to 1", FloatSetterSlot(), FloatGetterSlot(), model);
    if (!read) {
        char probe[512] = {};
        ProbeKey(params_, "DLSSNR.MVecScaleX", probe, sizeof(probe));
        const auto used = std::strlen(out);
        std::snprintf(out + used, size - used,
            "\nOptimizer FPS NGX hook: DLSSNR.MVecScaleX getter probe: %s", probe);
    }
    return static_cast<uint32_t>(std::strlen(out));
}
uint32_t ModelHostNgx::ModelReady(void *handle) { (void) handle; return 1; } void ModelHostNgx::EndFrame(ID3D12GraphicsCommandList *cmd, const OfpsFrameInputs *frame, const OfpsEvalResult *result) { (void) cmd; (void) frame; (void) result; Restore(); } }
