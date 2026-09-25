#pragma once
#include "core/api/ofps_core.h"
#include "test_core_api_reference.h"
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
namespace coretest {
struct HostFrame {
    ComPtr<ID3D12Resource> color, depth, motion, output, outputPadded;
    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 5> rtv{};
    D3D12_RESOURCE_STATES colorRest = kInputRest;
    OfpsRect outputRect{0, 0, kW, kH};
};
inline bool CreateHostFrame(WarpDevice &w, HostFrame &f,
                            std::uint32_t width = kW, std::uint32_t height = kH) {
    ID3D12Device *d = w.device.Get();
    f.color =
        CreateTexture(d, width, height, kColorFormat,
                      D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, kInputRest);
    f.depth = CreateTexture(d, width, height, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, kInputRest);
    f.motion = CreateTexture(d, width, height, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, kInputRest);
    const D3D12_RESOURCE_FLAGS outputFlags =
        D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12_CLEAR_VALUE clear{};
    clear.Format = kColorFormat;
    std::copy(kMarker.begin(), kMarker.end(), clear.Color);
    f.output = CreateTexture(d, width, height, kColorFormat, outputFlags, kOutputRest, &clear);
    f.outputPadded = CreateTexture(d, std::max(kPadW, width + 64), std::max(kPadH, height + 32),
                                   kColorFormat, outputFlags, kOutputRest, &clear);
    f.outputRect = {0, 0, width, height};
    if (!f.color || !f.depth || !f.motion || !f.output || !f.outputPadded)
        return false;
    f.rtvHeap = CreateRtvHeap(d, 5);
    if (!f.rtvHeap)
        return false;
    ID3D12Resource *targets[5] = {f.color.Get(), f.depth.Get(), f.motion.Get(), f.output.Get(), f.outputPadded.Get()};
    for (std::size_t i = 0; i < 5; ++i) {
        f.rtv[i] = RtvAt(d, f.rtvHeap.Get(), static_cast<UINT>(i));
        d->CreateRenderTargetView(targets[i], nullptr, f.rtv[i]);
    }
    return true;
}
inline OfpsResource Resource(ID3D12Resource *res, DXGI_FORMAT view, OfpsRect rect, D3D12_RESOURCE_STATES rest) {
    OfpsResource r{};
    r.size = sizeof(r);
    r.res = res;
    r.view = view;
    r.rect = rect;
    r.restState = rest;
    r.subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    return r;
}
inline OfpsFrameInputs FrameInputs(const HostFrame &f, ID3D12Resource *output, std::uint32_t hostReset) {
    OfpsFrameInputs in{};
    in.size = sizeof(in);
    const OfpsRect full{0, 0, f.outputRect.w, f.outputRect.h};
    in.color = Resource(f.color.Get(), kColorFormat, full, f.colorRest);
    in.depth = Resource(f.depth.Get(), DXGI_FORMAT_R32_FLOAT, full, kInputRest);
    in.motion = Resource(f.motion.Get(), DXGI_FORMAT_R16G16_FLOAT, full, kInputRest);
    in.output = Resource(output, kColorFormat, f.outputRect, kOutputRest);
    const OfpsResource absent = Resource(nullptr, DXGI_FORMAT_UNKNOWN, OfpsRect{}, D3D12_RESOURCE_STATE_COMMON);
    in.ui = absent;
    in.uiAlpha = absent;
    in.backbuffer = absent;
    for (OfpsResource &codec : in.codecInputs)
        codec = absent;
    in.mvScaleX = 2.0f;
    in.mvScaleY = 3.0f;
    in.depthInverted = 0;
    in.hostReset = hostReset;
    in.colorDomain = OFPS_COLOR_DISPLAY_REFERRED;
    return in;
}
struct EvalRun {
    bool ok = false;
    int result = OFPS_E_STATE;
    OfpsEvalResult eval{};
    ReadbackCapture output;
};
inline EvalRun RunEvaluate(WarpDevice &w, IOfpsCore *core, IOfpsFeature *feature, HostFrame &f, ID3D12Resource *output,
                           std::uint32_t hostReset, const OfpsFrameInputs *customInputs = nullptr) {
    EvalRun run;
    if (!BeginList(w))
        return run;
    ID3D12GraphicsCommandList *cmd = w.list.Get();
    std::vector<ComPtr<ID3D12Resource>> uploads;
    if (!UploadPattern(w, f.color.Get(), f.colorRest, 0, uploads) ||
        !UploadPattern(w, f.depth.Get(), kInputRest, 1, uploads) ||
        !UploadPattern(w, f.motion.Get(), kInputRest, 2, uploads))
        return run;
    if (output != f.color.Get()) {
        const std::size_t outputRtv = output == f.output.Get() ? 3 : 4;
        Transition(cmd, output, kOutputRest, D3D12_RESOURCE_STATE_RENDER_TARGET);
        cmd->ClearRenderTargetView(f.rtv[outputRtv], kMarker.data(), 0, nullptr);
        Transition(cmd, output, D3D12_RESOURCE_STATE_RENDER_TARGET, kOutputRest);
    }
    const OfpsFrameInputs in = customInputs ? *customInputs : FrameInputs(f, output, hostReset);
    run.eval.size = sizeof(run.eval);
    run.result = feature->Evaluate(cmd, &in, &run.eval);
    run.output = CreateReadback(w.device.Get(), output);
    if (!run.output.buffer)
        return run;
    Transition(cmd, output, kOutputRest, D3D12_RESOURCE_STATE_COPY_SOURCE);
    RecordReadback(cmd, output, run.output);
    Transition(cmd, output, D3D12_RESOURCE_STATE_COPY_SOURCE, kOutputRest);
    if (!SubmitList(w))
        return run;
    core->OnCommandListExecuted(w.queue.Get(), cmd);
    run.ok = WaitForQueue(w.device.Get(), w.queue.Get());
    return run;
}
inline bool PixelIs(const ReadbackCapture &c, std::uint32_t x, std::uint32_t y, const std::array<float, 4> &expected,
                    float tolerance) {
    std::array<float, 4> got{};
    if (!ReadHalf4(c, x, y, &got))
        return false;
    for (std::size_t i = 0; i < 4; ++i)
        if (!Near(got[i], expected[i], tolerance))
            return false;
    return true;
}
} // namespace coretest
