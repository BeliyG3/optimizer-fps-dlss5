#include "debug_readback.h"

#include "hook_context.h"

#include <cmath>
#include <vector>

namespace pwhook {

float HalfToFloat(std::uint16_t h);

// [PeripheralWarp] DebugLayer=1: the D3D12 debug layer's messages after an evaluate (first 200).
void DumpDebugLayer(ID3D12Device *device)
{
    if (!Ctx().diag.debugLayerLog || device == nullptr) return;
    ID3D12InfoQueue *queue = nullptr;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&queue))) || queue == nullptr) return;
    const UINT64 count = queue->GetNumStoredMessages();
    for (UINT64 i = 0; i < count && Ctx().debugLayerPrinted < 200; ++i) {
        SIZE_T size = 0;
        queue->GetMessage(i, nullptr, &size);
        if (size == 0) continue;
        std::vector<char> storage(size);
        auto *message = reinterpret_cast<D3D12_MESSAGE *>(storage.data());
        if (FAILED(queue->GetMessage(i, message, &size))) continue;
        if (message->Severity > D3D12_MESSAGE_SEVERITY_WARNING) continue;
        ++Ctx().debugLayerPrinted;
        Log(message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR, "Optimizer FPS NGX hook: debug layer [%d] %s", (int) message->Severity, message->pDescription);
    }
    queue->ClearStoredMessages();
    queue->Release();
}

void TemporalDebugReadback(FeatureState &st, ID3D12GraphicsCommandList *cmd, const pwtemporal::FrameInputs &tin)
{
    if ((!Ctx().diag.temporalReadback && !Ctx().temporal.debugLog) || !st.temporal || (Ctx().status.interpFrames % 60) != 5) return;
    if (st.motionReadback == nullptr) {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = 1024;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(st.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                      IID_PPV_ARGS(&st.motionReadback))))
            return;
    }
    const std::uint32_t x = st.nativeWidth / 2 + 7, y = st.nativeHeight / 2 + 3;
    st.temporal->RecordDebugCopies(cmd, tin, st.motionReadback, x, y);
    // The copies were just recorded; the wait covers everything submitted before this evaluate, so
    // the values printed are from the previous readback (one call behind), which is fine for a check.
    if (!st.motionReadbackPending) {
        st.motionReadbackPending = true;
        return;
    }
    if (!WaitForGpu(st.realDevice, st.device)) return;
    void *mapped = nullptr;
    const D3D12_RANGE range{0, 1024};
    if (FAILED(st.motionReadback->Map(0, &range, &mapped)) || mapped == nullptr) return;
    const auto *h = static_cast<const std::uint16_t *>(mapped);
    auto px = [&](int row, int i, int ch) { return HalfToFloat(h[row * 128 + i * 4 + ch]); };
    Log(false, "Optimizer FPS NGX hook [temporal debug] mode %d every %d, full %llu / interpolated %llu, at (%u,%u): colour (%.4f %.4f %.4f) residual (%.4f %.4f %.4f) interp (%.4f %.4f %.4f) acc (%.2f %.2f) motion-texture px%s%s%s | colour tolerance %.2f, depth tolerance %.3f, hole fill %s",
        Ctx().temporal.mode, Ctx().temporal.every, static_cast<unsigned long long>(Ctx().status.fullFrames), static_cast<unsigned long long>(Ctx().status.interpFrames),
        x, y, px(0, 0, 0), px(0, 0, 1), px(0, 0, 2), px(1, 0, 0), px(1, 0, 1), px(1, 0, 2), px(2, 0, 0), px(2, 0, 1), px(2, 0, 2),
        HalfToFloat(h[3 * 128 + 0]), HalfToFloat(h[3 * 128 + 1]), Ctx().temporal.flipMotionSign ? ", sign flipped" : "",
        Ctx().temporal.debugRawInterpolation ? ", raw interpolation" : "", Ctx().temporal.debugSingleFrameMotion ? ", single-frame motion to the model" : "",
        Ctx().temporal.colorTolerance, Ctx().temporal.depthTolerance, Ctx().temporal.holeFill ? "on" : "off");
    const D3D12_RANGE none{0, 0};
    st.motionReadback->Unmap(0, &none);
}

float HalfToFloat(std::uint16_t h)
{
    const std::uint32_t sign = (h >> 15) & 1u, exp = (h >> 10) & 0x1Fu, mant = h & 0x3FFu;
    float value;
    if (exp == 0) value = std::ldexp(static_cast<float>(mant), -24);
    else if (exp == 31) value = mant ? NAN : INFINITY;
    else value = std::ldexp(static_cast<float>(mant | 0x400u), static_cast<int>(exp) - 25);
    return sign ? -value : value;
}

} // namespace pwhook
