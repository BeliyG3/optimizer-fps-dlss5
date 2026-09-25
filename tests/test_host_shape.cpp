#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "core/context.h"
#include "core/frame/frame_inputs.h"
#include "core/frame/host_depth_state.h"
#include "core/frame/host_shape.h"
#include <cstdio>
#include <cstring>
#include <dxgi1_4.h>
#include <iostream>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;
using namespace ofps::core;
namespace
{
int failures = 0;
int rejectedEvents = 0;
void *rejectedHandle = nullptr;
void Event(OfpsEvent event, const OfpsEventData &data)
{
    if (event == OFPS_EVENT_HOST_SHAPE_REJECTED)
    {
        ++rejectedEvents;
        rejectedHandle = data.handle;
    }
}
void Check(bool condition, const char *message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}
struct FakeModelHost final : IOfpsModelHost
{
    int describeCalls = 0;
    int CreateModel(ID3D12GraphicsCommandList *, uint32_t, uint32_t, uint32_t, void **handle) override
    {
        *handle = this;
        return OFPS_OK;
    }
    int ReleaseModel(void *) override
    {
        return OFPS_OK;
    }
    int RunModel(ID3D12GraphicsCommandList *, void *, const OfpsModelInputs *) override
    {
        return OFPS_OK;
    }
    int PrepareModelInput(ID3D12GraphicsCommandList *, const OfpsFrameInputs *, OfpsResource *, OfpsResource *) override
    {
        return OFPS_S_IDENTITY;
    }
    int ResolveAnswer(ID3D12GraphicsCommandList *, const OfpsResource *, const OfpsFrameInputs *) override
    {
        return OFPS_S_IDENTITY;
    }
    uint32_t DescribeInputs(char *out, uint32_t size) override
    {
        ++describeCalls;
        return static_cast<uint32_t>(std::snprintf(out, size, "fake block"));
    }
    uint32_t ModelReady(void *) override
    {
        return 1;
    }
    void EndFrame(ID3D12GraphicsCommandList *, const OfpsFrameInputs *, const OfpsEvalResult *) override
    {
    }
};
ComPtr<ID3D12Resource> Texture(ID3D12Device *device, UINT w, UINT h, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags)
{
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = w;
    desc.Height = h;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Flags = flags;
    ComPtr<ID3D12Resource> r;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                               IID_PPV_ARGS(&r))))
        r.Reset();
    return r;
}
OfpsResource Res(ID3D12Resource *res, std::uint32_t x, std::uint32_t y, std::uint32_t w, std::uint32_t h)
{
    OfpsResource r{};
    r.size = sizeof(r);
    r.res = res;
    r.view = DXGI_FORMAT_UNKNOWN;
    r.rect = OfpsRect{x, y, w, h};
    r.restState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    r.subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    return r;
}
} // namespace
int main()
{
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter> warp;
    ComPtr<ID3D12Device> device;
    Check(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))), "DXGI factory");
    Check(factory && SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))), "WARP adapter");
    Check(warp && SUCCEEDED(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))),
          "WARP device");
    if (!device)
        return 1;
    ComPtr<ID3D12Resource> color =
        Texture(device.Get(), 1984, 1112, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_NONE);
    ComPtr<ID3D12Resource> output =
        Texture(device.Get(), 1984, 1112, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    Check(color && output, "WARP textures");
    OfpsFrameInputs frame{};
    frame.size = sizeof(frame);
    frame.color = Res(color.Get(), 0, 0, 1920, 1080);
    frame.output = Res(output.Get(), 0, 0, 1920, 1080);
    HostShape shape = ReadHostShape(frame);
    Check(shape.complete, "colour and output present -> complete");
    Check(shape.colorTexW == 1984 && shape.colorTexH == 1112 && shape.outputTexW == 1984 && shape.outputTexH == 1112,
          "texture sizes from GetDesc");
    Check(shape.color.w == 1920 && shape.color.h == 1080 && shape.output.x == 0 && shape.output.y == 0,
          "regions from the inputs");
    FakeModelHost modelHost;
    FeatureState st;
    st.nativeWidth = 1920;
    st.nativeHeight = 1080;
    st.modelHost = &modelHost;
    st.hostHandle = &modelHost;
    SetEventSink(Event);
    Check(!JudgeHostShape(st, shape) && st.hostFits,
          "a region of the feature's size at 0,0 in a larger texture fits (no "
          "verdict change)");
    Check(modelHost.describeCalls == 0, "a fitting frame does not ask for the block");
    frame.output = Res(output.Get(), 32, 16, 1920, 1080);
    Check(JudgeHostShape(st, ReadHostShape(frame)) && !st.hostFits,
          "an output region away from 0,0 does not fit (verdict changed)");
    Check(std::strcmp(st.hostReason, "host output region is 1920x1080 at 32,16 in a 1984x1112 "
                                     "texture, feature is 1920x1080") == 0,
          "the reason names the region");
    Check(modelHost.describeCalls == 1, "the unfit verdict is logged with IOfpsModelHost::DescribeInputs");
    Check(rejectedEvents == 1 && rejectedHandle == st.hostHandle, "one rejected event uses the original host handle");
    Check(!JudgeHostShape(st, ReadHostShape(frame)) && rejectedEvents == 1 && modelHost.describeCalls == 1,
          "unchanged unfit frame stays latched without another event or model description");
    frame.output.rect = {0, 0, 1920, 1080};
    Check(!JudgeHostShape(st, ReadHostShape(frame)) && !st.hostFits, "fit remains latched until layout changes");
    st.hostLatched = false;
    Check(JudgeHostShape(st, ReadHostShape(frame)) && st.hostFits, "layout change permits recovery");
    frame.color.rect = {0, 0, 1280, 720};
    Check(JudgeHostShape(st, ReadHostShape(frame)) && !st.hostFits &&
              std::strcmp(st.hostReason, "host colour region is 1280x720, feature is 1920x1080 (dynamic resolution?)") == 0,
          "smaller colour region rejects with dynamic resolution reason");
    frame.color.rect = {0, 0, 1920, 1080};
    auto upscale = ReadHostShape(frame);
    upscale.output = {0, 0, 3840, 2160};
    upscale.outputTexW = 3840; upscale.outputTexH = 2160;
    st.hostLatched = false;
    JudgeHostShape(st, upscale);
    Check(!st.hostFits && std::strcmp(st.hostReason,
              "the host upscales inside NR (colour 1920x1080 -> output 3840x2160, x2.00); this build compresses only same-size frames") == 0,
          "NR upscaling rejects with exact reason");
    SetEventSink(nullptr);
    frame.depth =
        MakeResource(color.Get(), DXGI_FORMAT_R32_FLOAT, {0, 0, 1920, 1080}, D3D12_RESOURCE_STATE_COPY_SOURCE, 2);
    frame.motion = frame.color;
    frame.color.view = DXGI_FORMAT_R16G16B16A16_FLOAT;
    frame.color.restState = D3D12_RESOURCE_STATE_COMMON;
    frame.color.subresource = 3;
    const auto original = frame;
    ResolveFrameViews(frame);
    Check(frame.color.view == original.color.view && frame.depth.view == original.depth.view,
          "explicit typed views survive resolution");
    Check(frame.color.restState == D3D12_RESOURCE_STATE_COMMON && frame.color.subresource == 3,
          "view resolution preserves states and subresources");
    auto model = ModelInputsFrom(frame, 1920, 1080);
    Check(FrameHasModelInputs(frame) && model.depth.restState == frame.depth.restState && model.depth.subresource == 2,
          "frame-to-model conversion preserves caller state and subresource");
    const D3D12_RESOURCE_STATES expected[] = {frame.depth.restState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                              D3D12_RESOURCE_STATE_DEPTH_READ |
                                                  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                              D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_COMMON};
    for (int mode = 0; mode != 5; ++mode)
    {
        Ctx().diag.depthState = mode;
        Check(HostDepthState(frame.depth) == expected[mode], "depth override table");
    }
    Ctx().diag.depthState = 99;
    Check(HostDepthState(frame.depth) == frame.depth.restState, "depth auto follows caller rest state");
    Ctx().diag.depthState = 0;
    frame.color.res = nullptr;
    Check(!ReadHostShape(frame).complete, "no colour -> incomplete");
    if (failures == 0)
        std::cout << "host_shape: ok\n";
    return failures == 0 ? 0 : 1;
}
