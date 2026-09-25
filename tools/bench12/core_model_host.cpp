#include "core_model_host.h"
#include <algorithm>
#include <cstring>
namespace {
int Result(NVSDK_NGX_Result result) { return NVSDK_NGX_SUCCEED(result) ? OFPS_OK : OFPS_E_STATE; }
NrRect Rect(const OfpsRect &r) { return {r.x, r.y, r.w, r.h}; }
}
int CoreModelHost::CreateModel(ID3D12GraphicsCommandList *cmd, uint32_t w,
                               uint32_t h, uint32_t withholdUi, void **out) try {
    if (!out) return OFPS_E_ARG;
    *out = nullptr;
    (void)withholdUi; // NrWriteEvaluate always disables UI correction.
    parameters_.Reset();
    NrCreateInfo create = create_;
    create.width = w; create.height = h;
    NrWriteCreate(parameters_, create);
    // Match the shell's actual NGX vtable slots (int32 sizes, generic pointers).
    parameters_.Set("DLSSNR.Width", static_cast<int>(w));
    parameters_.Set("DLSSNR.Height", static_cast<int>(h));
    NVSDK_NGX_Handle *handle = nullptr;
    parameters_.AuditFinal("core", "create");
    last = runtime_.Create(cmd, &parameters_, &handle);
    if (NVSDK_NGX_FAILED(last) || !handle) return OFPS_E_STATE;
    *out = handle; ++live;
    return OFPS_OK;
}
catch (...) { last = NVSDK_NGX_Result_FAIL_InvalidParameter; return OFPS_E_STATE; }
int CoreModelHost::ReleaseModel(void *handle) {
    last = runtime_.Release(static_cast<NVSDK_NGX_Handle *>(handle));
    if (NVSDK_NGX_SUCCEED(last) && live) --live;
    return Result(last);
}
int CoreModelHost::RunModel(ID3D12GraphicsCommandList *cmd, void *handle, const OfpsModelInputs *in) try {
    if (!in || in->size < sizeof(*in)) return OFPS_E_ARG;
    NrFrame frame = frame_;
    frame.colour = in->color.res; frame.depth = in->depth.res;
    frame.motion = in->motion.res; frame.output = in->output.res;
    frame.colourRect = Rect(in->color.rect); frame.guideRect = Rect(in->depth.rect);
    frame.outputRect = Rect(in->output.rect);
    frame.mvScaleX = in->mvScaleX; frame.mvScaleY = in->mvScaleY;
    frame.depthInverted = in->depthInverted != 0; frame.reset = in->reset != 0;
    NrWriteEvaluate(parameters_, frame);
    parameters_.Set("DLSSNR.Color", static_cast<void *>(frame.colour));
    parameters_.Set("DLSSNR.Depth", static_cast<void *>(frame.depth));
    parameters_.Set("DLSSNR.MVec", static_cast<void *>(frame.motion));
    parameters_.Set("DLSSNR.Output", static_cast<void *>(frame.output));
    NrWriteRect(parameters_, "MVec", Rect(in->motion.rect));
    parameters_.AuditFinal("core", "evaluate");
    last = runtime_.Evaluate(cmd, static_cast<NVSDK_NGX_Handle *>(handle), &parameters_);
    return Result(last);
}
catch (...) { last = NVSDK_NGX_Result_FAIL_InvalidParameter; return OFPS_E_STATE; }
int CoreModelHost::PrepareModelInput(ID3D12GraphicsCommandList *, const OfpsFrameInputs *frame,
                                    OfpsResource *color, OfpsResource *before) {
    if (!frame || !color || !before) return OFPS_E_ARG;
    *color = frame->color; *before = {}; before->size = sizeof(*before);
    return OFPS_S_IDENTITY;
}
int CoreModelHost::ResolveAnswer(ID3D12GraphicsCommandList *, const OfpsResource *, const OfpsFrameInputs *) {
    return OFPS_S_IDENTITY;
}
uint32_t CoreModelHost::DescribeInputs(char *out, uint32_t size) {
    constexpr char text[] = "bench12 identity codec; model calls use nvngx.dll_pwbench12.dll";
    if (!out || !size) return 0;
    const auto count = std::min<uint32_t>(size - 1, sizeof(text) - 1);
    std::memcpy(out, text, count); out[count] = 0;
    return count;
}
uint32_t CoreModelHost::ModelReady(void *handle) { return handle ? 1u : 0u; }
void CoreModelHost::EndFrame(ID3D12GraphicsCommandList *, const OfpsFrameInputs *, const OfpsEvalResult *) {}
