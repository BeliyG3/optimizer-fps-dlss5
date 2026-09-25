#include "hosts/reshade/model_host_ngx.h"
#include "hosts/reshade/ngx_params.h"
#include "core/gpu/barriers.h"
#include <cstdio>
#include <cstring>
#include <dxgi1_4.h>
#include <map>
#include <string>
#include <wrl/client.h>
OfpsModelInputs TestModelInputs(void *params)
{
    OfpsFrameInputs frame{};
    ofps::reshade::ReadFrameInputs(params, &frame, nullptr);
    OfpsModelInputs in{};
    in.size = sizeof(in);
    in.color = frame.color;
    in.depth = frame.depth;
    in.motion = frame.motion;
    in.output = frame.output;
    in.ui = frame.ui;
    in.uiAlpha = frame.uiAlpha;
    in.backbuffer = frame.backbuffer;
    in.mvScaleX = frame.mvScaleX;
    in.mvScaleY = frame.mvScaleY;
    in.depthInverted = frame.depthInverted;
    in.reset = frame.hostReset;
    return in;
}
using Microsoft::WRL::ComPtr;
using namespace ofps::reshade;
namespace
{
int g_failures = 0;
void Check(bool ok, const char *what)
{
    if (ok)
        return;
    ++g_failures;
    std::printf("FAIL: %s\n", what);
}
struct FakeBlock
{
    void **vtable;
    std::map<std::string, unsigned long long> keys;
    unsigned sets = 0;
    unsigned scaleSets = 0;
};
FakeBlock &Self(void *p)
{
    return *static_cast<FakeBlock *>(p);
}
void SetRaw(void *p, const char *name, unsigned long long v)
{
    Self(p).keys[name] = v;
    if (std::strncmp(name, "DLSSNR.MVecScale", 16) == 0)
        ++Self(p).scaleSets;
    if (std::strncmp(name, "DLSSNR.", 7) == 0)
        ++Self(p).sets;
}
void SetFloatSlot(void *p, const char *name, float v)
{
    unsigned long long raw = 0;
    std::memcpy(&raw, &v, sizeof(v));
    SetRaw(p, name, raw);
}
void SetUIntSlot(void *p, const char *name, unsigned int v)
{
    SetRaw(p, name, v);
}
int GetRaw(void *p, const char *name, unsigned long long *v)
{
    const auto it = Self(p).keys.find(name);
    if (it == Self(p).keys.end())
        return 0;
    *v = it->second;
    return kNgxSuccess;
}
int GetUIntSlot(void *p, const char *name, unsigned int *v)
{
    unsigned long long raw = 0;
    if (GetRaw(p, name, &raw) != kNgxSuccess)
        return 0;
    *v = static_cast<unsigned int>(raw);
    return kNgxSuccess;
}
void UnusedSet(void *, const char *, unsigned long long)
{
}
int UnusedGet(void *, const char *, void *)
{
    return 0;
}
void *g_vtable[16] = {reinterpret_cast<void *>(&SetRaw),       reinterpret_cast<void *>(&UnusedSet),
                      reinterpret_cast<void *>(&UnusedSet),    reinterpret_cast<void *>(&SetUIntSlot),
                      reinterpret_cast<void *>(&UnusedSet),    reinterpret_cast<void *>(&UnusedSet),
                      reinterpret_cast<void *>(&SetFloatSlot), reinterpret_cast<void *>(&UnusedSet),
                      reinterpret_cast<void *>(&GetRaw),       reinterpret_cast<void *>(&UnusedGet),
                      reinterpret_cast<void *>(&UnusedGet),    reinterpret_cast<void *>(&GetUIntSlot),
                      reinterpret_cast<void *>(&UnusedGet),    reinterpret_cast<void *>(&UnusedGet),
                      reinterpret_cast<void *>(&GetRaw),       reinterpret_cast<void *>(&UnusedGet)};
std::map<std::string, unsigned long long> Keys(const FakeBlock &b)
{
    std::map<std::string, unsigned long long> m = b.keys;
    m.erase("PeripheralWarp.FloatProbe");
    return m;
}
struct Seen
{
    std::map<std::string, unsigned long long> keys;
    void *handle = nullptr;
    int releases = 0;
};
Seen g_seen;
int g_evaluateResult = kNgxSuccess;
void *const kHandle = reinterpret_cast<void *>(0x18);
void *const kCallback = reinterpret_cast<void *>(0xCB);
int FakeCreate(ID3D12GraphicsCommandList *, int featureId, void *params, void **handle)
{
    Check(featureId == kFeatureNeuralRendering, "create asks for feature 18");
    g_seen.keys = Keys(Self(params));
    *handle = kHandle;
    return kNgxSuccess;
}
int FakeEvaluate(ID3D12GraphicsCommandList *, void *handle, void *params, void *callback)
{
    g_seen.keys = Keys(Self(params));
    g_seen.handle = handle;
    Check(callback == kCallback, "the frame's callback reaches the evaluate");
    return g_evaluateResult;
}
int FakeRelease(void *handle)
{
    g_seen.handle = handle;
    ++g_seen.releases;
    return kNgxSuccess;
}
unsigned long long Ptr(ID3D12Resource *r)
{
    return reinterpret_cast<unsigned long long>(r);
}
unsigned long long FloatBits(float f)
{
    unsigned long long raw = 0;
    std::memcpy(&raw, &f, sizeof(f));
    return raw;
}
unsigned long long Seen(const char *name)
{
    const auto it = g_seen.keys.find(name);
    return it == g_seen.keys.end() ? ~0ull : it->second;
}
OfpsResource Res(ID3D12Resource *r, unsigned x, unsigned y, unsigned w, unsigned h)
{
    OfpsResource o{};
    o.size = sizeof(o);
    o.res = r;
    o.view = DXGI_FORMAT_UNKNOWN;
    o.rect = {x, y, w, h};
    o.restState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    o.subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    return o;
}
} // namespace
namespace ofps::reshade
{
int CallCreate(ID3D12GraphicsCommandList *c, int id, void *p, void **h)
{
    return FakeCreate(c, id, p, h);
}
int CallEvaluate(ID3D12GraphicsCommandList *c, void *h, void *p, void *cb)
{
    return FakeEvaluate(c, h, p, cb);
}
int CallRelease(void *h)
{
    return FakeRelease(h);
}
} // namespace ofps::reshade
int main()
{
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter> warp;
    ComPtr<ID3D12Device> device;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || FAILED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))) ||
        FAILED(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))))
    {
        std::printf("FAIL: WARP device\n");
        return 1;
    }
    ID3D12Resource *color = nullptr, *depth = nullptr, *motion = nullptr, *output = nullptr, *ui = nullptr;
    ID3D12Resource *pColor = nullptr, *pDepth = nullptr, *pMotion = nullptr, *nrOutput = nullptr;
    Check(ofps::core::gpu::CreateTexture(device.Get(), 64, 32, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_NONE,
                                         &color) &&
              ofps::core::gpu::CreateTexture(device.Get(), 64, 32, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_NONE,
                                             &depth) &&
              ofps::core::gpu::CreateTexture(device.Get(), 32, 16, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_NONE,
                                             &motion) &&
              ofps::core::gpu::CreateTexture(device.Get(), 64, 32, DXGI_FORMAT_R8G8B8A8_UNORM,
                                             D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, &output) &&
              ofps::core::gpu::CreateTexture(device.Get(), 64, 32, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_NONE,
                                             &ui) &&
              ofps::core::gpu::CreateTexture(device.Get(), 40, 20, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_NONE,
                                             &pColor) &&
              ofps::core::gpu::CreateTexture(device.Get(), 40, 20, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_NONE,
                                             &pDepth) &&
              ofps::core::gpu::CreateTexture(device.Get(), 40, 20, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_NONE,
                                             &pMotion) &&
              ofps::core::gpu::CreateTexture(device.Get(), 40, 20, DXGI_FORMAT_R8G8B8A8_UNORM,
                                             D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, &nrOutput),
          "textures are created on WARP");
    if (g_failures)
        return 1;
    FakeBlock block;
    block.vtable = g_vtable;
    SetResource(&block, "DLSSNR.Color", color);
    SetResource(&block, "DLSSNR.Depth", depth);
    SetResource(&block, "DLSSNR.MVec", motion);
    SetResource(&block, "DLSSNR.Output", output);
    SetResource(&block, "DLSSNR.UI", ui);
    SetUInt(&block, "DLSSNR.Width", 64);
    SetUInt(&block, "DLSSNR.Height", 32);
    SetUInt(&block, "DLSSNR.InputWidth", 64);
    SetUInt(&block, "DLSSNR.InputHeight", 32);
    WriteSubrect(&block, "Color", 0, 0, 64, 32);
    WriteSubrect(&block, "Depth", 0, 0, 64, 32);
    WriteSubrect(&block, "MVec", 0, 0, 32, 16);
    WriteSubrect(&block, "Output", 0, 0, 64, 32);
    SetFloat(&block, "DLSSNR.MVecScaleX", 0.5f);
    SetFloat(&block, "DLSSNR.MVecScaleY", -0.5f);
    SetUInt(&block, "DLSSNR.DepthInverted", 1);
    SetUInt(&block, "DLSSNR.Reset", 0);
    SetUInt(&block, "DLSSNR.UICorrection", 1);
    ModelHostNgx host(&block);
    host.BeginFrame(&block, kCallback);
    const auto before = Keys(block);
    OfpsModelInputs own = TestModelInputs(&block);
    own.width = 64;
    own.height = 32;
    block.sets = 0;
    Check(host.RunModel(nullptr, kHandle, &own) == OFPS_OK, "pass-through evaluate returns the snippet's result");
    Check(g_seen.handle == kHandle, "the handle reaches the evaluate");
    Check(block.sets == 0, "a pass-through evaluate writes nothing into the block");
    Check(g_seen.keys == before, "the model saw the block as the host left it");
    Check(own.mvScaleX == 0.5f && own.depthInverted == 1 && own.motion.rect.w == 32,
          "TestModelInputs reads scale, flag and rects");
    // A smaller colour region and distinct output size must not normalize the host block.
    WriteSubrect(&block, "Color", 0, 0, 48, 24);
    SetUInt(&block, "DLSSNR.Width", 48);
    SetUInt(&block, "DLSSNR.Height", 24);
    SetUInt(&block, "DLSSNR.OutputWidth", 64);
    SetUInt(&block, "DLSSNR.OutputHeight", 32);
    host.BeginFrame(&block, kCallback);
    auto unfit = TestModelInputs(&block);
    unfit.width = unfit.color.rect.w;
    unfit.height = unfit.color.rect.h;
    const auto unfitBefore = Keys(block);
    block.sets = 0;
    Check(host.RunModel(nullptr, kHandle, &unfit) == OFPS_OK && block.sets == 0,
          "unfit host grid writes no keys, including sizes");
    Check(g_seen.keys == unfitBefore && Keys(block) == unfitBefore,
          "unfit evaluate preserves distinct input/output sizes during and after evaluate");
    // Some hosts retain the create extent while shrinking only the colour subrect.
    SetUInt(&block, "DLSSNR.Width", 64);
    SetUInt(&block, "DLSSNR.Height", 32);
    host.BeginFrame(&block, kCallback);
    block.sets = 0;
    Check(host.RunModel(nullptr, kHandle, &unfit) == OFPS_OK && block.sets == 0,
          "dynamic colour subrect leaves the create extent untouched");
    SetResource(&block, "DLSSNR.Output", color);
    auto inPlace = TestModelInputs(&block);
    Check(inPlace.color.res == inPlace.output.res &&
              inPlace.color.restState == D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE &&
              inPlace.output.restState == D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
          "in-place shell preserves legacy Pack colour and output states");
    block.keys = before;
    host.BeginFrame(&block, kCallback);
    OfpsModelInputs packed{};
    packed.size = sizeof(packed);
    packed.color = Res(pColor, 0, 0, 40, 20);
    packed.depth = Res(pDepth, 0, 0, 40, 20);
    packed.motion = Res(pMotion, 0, 0, 40, 20);
    packed.output = Res(nrOutput, 0, 0, 40, 20);
    packed.ui = Res(ui, 0, 0, 0, 0);
    packed.withholdUi = 1;
    packed.mvScaleX = packed.mvScaleY = 1.0f;
    packed.depthInverted = 1;
    packed.reset = 1;
    packed.width = 40;
    packed.height = 20;
    block.sets = 0;
    Check(host.RunModel(nullptr, kHandle, &packed) == OFPS_OK, "warped evaluate returns the snippet's result");
    Check(block.sets > 0, "a warped evaluate writes the block");
    Check(Seen("DLSSNR.Color") == Ptr(pColor) && Seen("DLSSNR.Depth") == Ptr(pDepth) &&
              Seen("DLSSNR.MVec") == Ptr(pMotion) && Seen("DLSSNR.Output") == Ptr(nrOutput),
          "the model saw the packed textures");
    Check(Seen("DLSSNR.UI") == 0, "the UI input was withheld");
    Check(Seen("DLSSNR.Width") == 40 && Seen("DLSSNR.Height") == 20 && Seen("DLSSNR.InputWidth") == 40,
          "the model saw the work extent");
    Check(Seen("DLSSNR.ColorSubrectWidth") == 40 && Seen("DLSSNR.MVecSubrectHeight") == 20 &&
              Seen("DLSSNR.OutputSubrectWidth") == 40,
          "the model saw work sub-rects");
    Check(Seen("DLSSNR.MVecScaleX") == FloatBits(1.0f) && Seen("DLSSNR.MVecScaleY") == FloatBits(1.0f),
          "the model saw scale 1");
    Check(Seen("DLSSNR.Reset") == 1 && Seen("DLSSNR.DepthInverted") == 1 && Seen("DLSSNR.UICorrection") == 0,
          "reset written and UI correction withheld");
    Check(Keys(block) == before, "the block goes back to the host exactly as it came");
    for (const char *k : {"DLSSNR.ColorSubrectBaseX", "DLSSNR.ColorSubrectBaseY", "DLSSNR.ColorSubrectWidth",
                          "DLSSNR.ColorSubrectHeight", "DLSSNR.MVecScaleX", "DLSSNR.MVecScaleY"})
        block.keys.erase(k);
    host.BeginFrame(&block, kCallback);
    block.scaleSets = 0;
    Check(host.RunModel(nullptr, kHandle, &packed) == OFPS_OK, "warped evaluate without host rects");
    Check(Seen("DLSSNR.ColorSubrectWidth") == 40, "the model saw the work colour rect");
    Check(block.keys["DLSSNR.ColorSubrectWidth"] == 64 && block.keys["DLSSNR.ColorSubrectHeight"] == 32 &&
              block.keys["DLSSNR.ColorSubrectBaseX"] == 0,
          "an absent rect is handed back as the whole texture");
    Check(block.scaleSets == 2, "absent scales are written only for the model, never by Restore");
    host.BeginFrame(&block, kCallback);
    own = TestModelInputs(&block);
    own.width = 64;
    own.height = 32;
    block.sets = 0;
    Check(host.RunModel(nullptr, kHandle, &own) == OFPS_OK && block.sets == 0,
          "the frame's own inputs match the block again");
    SetUInt(&block, "DLSSNR.Reset", 0);
    host.BeginFrame(&block, kCallback);
    const auto beforeCreate = Keys(block);
    void *handle = nullptr;
    Check(host.CreateModel(nullptr, 40, 20, 1, &handle) == OFPS_OK && handle == kHandle,
          "create returns the snippet's handle");
    Check(Seen("DLSSNR.Width") == 40 && Seen("DLSSNR.InputHeight") == 20, "the model was created at the work extent");
    Check(Seen("DLSSNR.UICorrection") == 0, "a warped model is created with UI correction off");
    Check(Keys(block) == beforeCreate, "create hands the block back");
    block.sets = 0;
    Check(host.CreateModel(nullptr, 64, 32, 0, &handle) == OFPS_OK, "native create");
    Check(Seen("DLSSNR.UICorrection") == 1 && block.sets == 0, "a native create leaves the block alone");
    g_evaluateResult = -7;
    Check(host.RunModel(nullptr, kHandle, &packed) == OFPS_E_DEVICE && host.LastNgxResult() == -7,
          "the snippet's failure is returned");
    Check(Keys(block) == beforeCreate, "the block is restored after a failed evaluate");
    block.sets = 0;
    host.EndFrame(nullptr, nullptr, nullptr);
    host.EndFrame(nullptr, nullptr, nullptr);
    Check(block.sets == 0, "EndFrame after a restored frame writes nothing");
    g_evaluateResult = kNgxSuccess;
    Check(host.ReleaseModel(kHandle) == OFPS_OK && g_seen.releases == 1 && g_seen.handle == kHandle,
          "release goes to the snippet");
    Check(host.PrepareModelInput(nullptr, nullptr, nullptr, nullptr) == OFPS_S_IDENTITY, "identity codec");
    Check(host.ResolveAnswer(nullptr, nullptr, nullptr) == OFPS_S_IDENTITY, "identity answer");
    Check(host.ModelReady(kHandle) == 1, "models are ready as soon as they are created");
    char text[640];
    const uint32_t n = host.DescribeInputs(text, sizeof(text));
    Check(n < sizeof(text) && std::strstr(text, "read, float setter slot 6 getter slot 14\n") == text &&
              std::strstr(text, "(pointers may rotate every frame; logged on a "
                                "change of formats/rects/scale)\n") != nullptr &&
              std::strstr(text, "UICorrection 1 (set), Enabled 0, Style 0, AutoMask 0, "
                                "Intensity 0.000, LocalTone 0.000, LocalStructure "
                                "0.000, PaperWhite 0.000 (unset)") != nullptr,
          "DescribeInputs preserves the three complete diagnostic tails");
    block.keys.erase("DLSSNR.MVecScaleX");
    char unread[2048] = {};
    host.DescribeInputs(unread, sizeof(unread));
    char expectedProbe[512] = {};
    ProbeKey(&block, "DLSSNR.MVecScaleX", expectedProbe, sizeof(expectedProbe));
    const std::string probeLine = std::string("\nOptimizer FPS NGX hook: DLSSNR.MVecScaleX getter probe: ") + expectedProbe;
    Check(std::strstr(unread, "NOT READ, defaulted to 1") == unread &&
              std::strstr(unread, probeLine.c_str()) != nullptr,
          "unread motion scale supplies the exact legacy getter probe line");
    Check(host.LastNgxResult() == kNgxSuccess, "native success stays distinct from OFPS_OK");
    for (ID3D12Resource *r : {color, depth, motion, output, ui, pColor, pDepth, pMotion, nrOutput})
        r->Release();
    std::printf("%s\n", g_failures ? "FAILED" : "model host ngx: all checks passed");
    return g_failures ? 1 : 0;
}
