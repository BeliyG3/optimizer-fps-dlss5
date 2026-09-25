#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "hosts/reshade/ngx_params.h"
#include <cstring>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <iostream>
#include <map>
#include <string>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;
using namespace ofps::reshade;
namespace
{
int failures = 0;
void Check(bool condition, const char *message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}
struct FakeBlock
{
    void **vtable;
    std::map<std::string, unsigned long long> values;
};
FakeBlock *Block(void *p)
{
    return static_cast<FakeBlock *>(p);
}
void SetRaw(void *p, const char *name, unsigned long long v)
{
    Block(p)->values[name] = v;
}
void SetNone(void *, const char *, unsigned long long)
{
}
void SetUInt3(void *p, const char *name, unsigned int v)
{
    Block(p)->values[name] = v;
}
void SetFloat6(void *p, const char *name, float v)
{
    unsigned long long raw = 0;
    std::memcpy(&raw, &v, sizeof(v));
    Block(p)->values[name] = raw;
}
int GetRaw(void *p, const char *name, unsigned long long *v)
{
    const auto it = Block(p)->values.find(name);
    if (it == Block(p)->values.end())
        return 0;
    *v = it->second;
    return kNgxSuccess;
}
int GetNone(void *, const char *, unsigned long long *)
{
    return 0;
}
int GetUInt11(void *p, const char *name, unsigned int *v)
{
    unsigned long long raw = 0;
    if (GetRaw(p, name, &raw) != kNgxSuccess)
        return 0;
    *v = static_cast<unsigned int>(raw);
    return kNgxSuccess;
}
void *g_slots[16] = {
    reinterpret_cast<void *>(&SetRaw),    reinterpret_cast<void *>(&SetNone), reinterpret_cast<void *>(&SetNone),
    reinterpret_cast<void *>(&SetUInt3),  reinterpret_cast<void *>(&SetNone), reinterpret_cast<void *>(&SetNone),
    reinterpret_cast<void *>(&SetFloat6), reinterpret_cast<void *>(&SetNone), reinterpret_cast<void *>(&GetRaw),
    reinterpret_cast<void *>(&GetNone),   reinterpret_cast<void *>(&GetNone), reinterpret_cast<void *>(&GetUInt11),
    reinterpret_cast<void *>(&GetNone),   reinterpret_cast<void *>(&GetNone), reinterpret_cast<void *>(&GetRaw),
    reinterpret_cast<void *>(&GetNone)};
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
    ComPtr<ID3D12Resource> depth =
        Texture(device.Get(), 1920, 1080, DXGI_FORMAT_R24G8_TYPELESS, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    ComPtr<ID3D12Resource> motion = Texture(device.Get(), 960, 540, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_NONE);
    ComPtr<ID3D12Resource> output =
        Texture(device.Get(), 1984, 1112, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    Check(color && depth && motion && output, "WARP textures");
    FakeBlock block{g_slots, {}};
    void *params = &block;
    SetResource(params, "DLSSNR.Color", color.Get());
    SetResource(params, "DLSSNR.Depth", depth.Get());
    SetResource(params, "DLSSNR.MVec", motion.Get());
    SetResource(params, "DLSSNR.Output", output.Get());
    SetUInt(params, "DLSSNR.ColorSubrectWidth", 1920);
    SetUInt(params, "DLSSNR.ColorSubrectHeight", 1080);
    SetUInt(params, "DLSSNR.OutputSubrectWidth", 1920);
    SetUInt(params, "DLSSNR.OutputSubrectHeight", 1080);
    SetUInt(params, "DLSSNR.OutputSubrectBaseX", 32);
    SetUInt(params, "DLSSNR.OutputSubrectBaseY", 16);
    SetFloat(params, "DLSSNR.MVecScaleX", -1920.0f);
    SetFloat(params, "DLSSNR.MVecScaleY", 0.0f);
    SetUInt(params, "DLSSNR.DepthInverted", 1);
    SetUInt(params, "DLSSNR.Reset", 1);
    SetUInt(params, "DLSSNR.UICorrection", 0);
    OfpsFrameInputs frame{};
    ShellFrameInfo info{};
    Check(ReadFrameInputs(params, &frame, &info), "ReadFrameInputs succeeds with the four pointers");
    Check(frame.size == sizeof(OfpsFrameInputs), "frame.size");
    Check(frame.color.res == color.Get() && frame.depth.res == depth.Get() && frame.motion.res == motion.Get() &&
              frame.output.res == output.Get(),
          "resources");
    Check(frame.ui.res == nullptr && frame.uiAlpha.res == nullptr && frame.backbuffer.res == nullptr,
          "absent UI inputs are null");
    Check(frame.color.rect.x == 0 && frame.color.rect.y == 0 && frame.color.rect.w == 1920 &&
              frame.color.rect.h == 1080,
          "colour rect from the block");
    Check(frame.output.rect.x == 32 && frame.output.rect.y == 16 && frame.output.rect.w == 1920 &&
              frame.output.rect.h == 1080,
          "output rect from the block");
    Check(frame.motion.rect.x == 0 && frame.motion.rect.w == 960 && frame.motion.rect.h == 540,
          "motion rect defaults to the whole texture");
    Check(frame.depth.rect.w == 1920 && frame.depth.rect.h == 1080, "depth rect defaults to the whole texture");
    Check(frame.color.view == DXGI_FORMAT_UNKNOWN && frame.depth.view == DXGI_FORMAT_UNKNOWN,
          "typed views are left to the core");
    Check(frame.color.restState == D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, "colour rests in NPSR (NGX inputs)");
    Check(frame.motion.restState == D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, "motion rests in NPSR");
    Check(frame.depth.restState == D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
          "depth rests in NPSR (NGX rule, depth-stencil included)");
    Check(frame.output.restState == D3D12_RESOURCE_STATE_UNORDERED_ACCESS, "output rests in UAV");
    Check(frame.depth.subresource == 0, "planar depth-stencil: the depth plane");
    Check(frame.color.subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, "colour: all subresources");
    Check(frame.mvScaleX == -1920.0f, "MVecScaleX read");
    Check(frame.mvScaleY == 1.0f, "MVecScaleY 0 -> 1");
    Check(!info.motionScaleRead, "one axis defaulted: not read");
    Check(frame.depthInverted == 1 && frame.hostReset == 1, "DepthInverted and Reset");
    Check(frame.colorDomain == OFPS_COLOR_DISPLAY_REFERRED, "the ReShade hosts are display-referred");
    Check(DepthPlaneSubresource(DXGI_FORMAT_D32_FLOAT_S8X24_UINT) == 0 &&
              DepthPlaneSubresource(DXGI_FORMAT_R32_TYPELESS) == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
          "DepthPlaneSubresource");
    char described[640];
    DescribeExtents(params, described, sizeof(described));
    Check(std::strstr(described, "Output rect 32,16 1920x1080;") != nullptr, "DescribeExtents: the output rect");
    Check(std::strstr(described, "MVecScale NOT READ, defaulted to 1 (float "
                                 "setter slot 6 getter slot 14)") != nullptr,
          "DescribeExtents: the motion scale read state");
    Check(std::strstr(described, "UICorrection 0 (set), Enabled 0, Style 0, AutoMask 0, "
                                 "Intensity 0.000, LocalTone 0.000, LocalStructure 0.000, "
                                 "PaperWhite 0.000 (unset)") != nullptr,
          "DescribeExtents: the model keys");
    SetResource(params, "DLSSNR.Color", nullptr);
    Check(!ReadFrameInputs(params, &frame, nullptr), "no colour -> false");
    if (failures == 0)
        std::cout << "ngx_params: ok\n";
    return failures == 0 ? 0 : 1;
}
