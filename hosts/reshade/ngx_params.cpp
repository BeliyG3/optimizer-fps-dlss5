#include "hosts/reshade/ngx_params.h"
#include <cmath>
#include <cstdio>
#include <cstring>

namespace ofps::reshade
{
void **VTable(void *params);
bool GetFloatVia(void *p, int slot, const char *name, float *v);
namespace
{
// Parameter block. Declared vtable: Set(ULL, float, double, uint, int, D3D11
// res, D3D12 res, void*) = 0..7, Get(same) = 8..15. The compiler emits adjacent
// overloads in reverse order, so the float pair is found by round-tripping a
// value; on driver 310.8 it is setter 6 / getter 14. The 64-bit slots 0 / 8
// answer every pointer-typed key.
constexpr int kSetPointer = 0;
constexpr int kSetUInt = 3;
constexpr int kGetPointer = 8;
constexpr int kGetUInt = 11;
using PFN_SetULL = void (*)(void *, const char *, unsigned long long);
using PFN_SetFloatT = void (*)(void *, const char *, float);
using PFN_SetUIntT = void (*)(void *, const char *, unsigned int);
using PFN_GetULL = int (*)(void *, const char *, unsigned long long *);
using PFN_GetUIntT = int (*)(void *, const char *, unsigned int *);
int g_floatSlot = -1;
int g_floatGetterSlot = -1;

void FindFloatSlot(void *p)
{
    if (g_floatSlot >= 0)
        return;
    static const int setters[] = {6, 5, 1, 2, 7, 4};
    static const int getters[] = {14, 13, 9, 10, 15, 12};
    const float expected = 0.375f;
    for (int setter : setters)
    {
        reinterpret_cast<PFN_SetFloatT>(VTable(p)[setter])(p, "PeripheralWarp.FloatProbe", expected);
        for (int getter : getters)
        {
            float readBack = 0.0f;
            if (GetFloatVia(p, getter, "PeripheralWarp.FloatProbe", &readBack) && readBack == expected)
            {
                g_floatSlot = setter;
                g_floatGetterSlot = getter;
                return;
            }
        }
    }
    g_floatSlot = 1;
    g_floatGetterSlot = 9;
}

} // namespace
void **VTable(void *params)
{
    return *reinterpret_cast<void ***>(params);
}

void SetUInt(void *p, const char *name, unsigned int v)
{
    reinterpret_cast<PFN_SetUIntT>(VTable(p)[kSetUInt])(p, name, v);
}

bool GetUInt(void *p, const char *name, unsigned int *v)
{
    return reinterpret_cast<PFN_GetUIntT>(VTable(p)[kGetUInt])(p, name, v) == kNgxSuccess;
}

void SetPointer(void *p, const char *name, void *v)
{
    reinterpret_cast<PFN_SetULL>(VTable(p)[kSetPointer])(p, name, reinterpret_cast<unsigned long long>(v));
}

void *GetPointer(void *p, const char *name)
{
    unsigned long long v = 0;
    if (reinterpret_cast<PFN_GetULL>(VTable(p)[kGetPointer])(p, name, &v) != kNgxSuccess)
        return nullptr;
    return reinterpret_cast<void *>(v);
}

bool GetFloatVia(void *p, int slot, const char *name, float *v)
{
    unsigned long long raw = 0;
    if (reinterpret_cast<PFN_GetULL>(VTable(p)[slot])(p, name, &raw) != kNgxSuccess)
        return false;
    if ((raw >> 32) == 0)
    {
        float f = 0.0f;
        std::memcpy(&f, &raw, sizeof(f));
        *v = f;
    }
    else
    {
        double d = 0.0;
        std::memcpy(&d, &raw, sizeof(d));
        *v = static_cast<float>(d);
    }
    return true;
}

void SetFloat(void *p, const char *name, float v)
{
    FindFloatSlot(p);
    reinterpret_cast<PFN_SetFloatT>(VTable(p)[g_floatSlot])(p, name, v);
}

bool GetFloat(void *p, const char *name, float *v)
{
    FindFloatSlot(p);
    return GetFloatVia(p, g_floatGetterSlot >= 0 ? g_floatGetterSlot : 9, name, v);
}

int FloatSetterSlot()
{
    return g_floatSlot;
}
int FloatGetterSlot()
{
    return g_floatGetterSlot;
}

void ProbeKey(void *p, const char *name, char *out, std::size_t outSize)
{
    std::size_t used = 0;
    for (int slot = 8; slot <= 15; ++slot)
    {
        unsigned long long raw = 0;
        const int rc = reinterpret_cast<PFN_GetULL>(VTable(p)[slot])(p, name, &raw);
        float f = 0.0f;
        double d = 0.0;
        std::memcpy(&f, &raw, sizeof(f));
        std::memcpy(&d, &raw, sizeof(d));
        const int n = std::snprintf(out + used, outSize > used ? outSize - used : 0,
                                    "[%d rc=%d raw=%016llx f=%.4g d=%.4g u=%llu] ", slot, rc, raw, f, d, raw);
        if (n < 0)
            break;
        used += static_cast<std::size_t>(n);
        if (used >= outSize)
            break;
    }
}

void SetResource(void *p, const char *name, ID3D12Resource *v)
{
    SetPointer(p, name, v);
}
ID3D12Resource *GetResource(void *p, const char *name)
{
    return static_cast<ID3D12Resource *>(GetPointer(p, name));
}

const SizeKeys kSizeKeys[3] = {{"DLSSNR.Width", "DLSSNR.Height"},
                               {"DLSSNR.InputWidth", "DLSSNR.InputHeight"},
                               {"DLSSNR.OutputWidth", "DLSSNR.OutputHeight"}};

void WriteSizes(void *params, std::uint32_t w, std::uint32_t h)
{
    for (const auto &k : kSizeKeys)
    {
        unsigned int probe = 0;
        if (GetUInt(params, k.w, &probe))
            SetUInt(params, k.w, w);
        if (GetUInt(params, k.h, &probe))
            SetUInt(params, k.h, h);
    }
}

Subrect ReadSubrect(void *params, const char *name, unsigned int defaultW, unsigned int defaultH)
{
    char key[64];
    Subrect r;
    r.w = defaultW;
    r.h = defaultH;
    std::snprintf(key, sizeof(key), "DLSSNR.%sSubrectWidth", name);
    if (GetUInt(params, key, &r.w) && r.w != 0)
    {
        r.present = true;
        std::snprintf(key, sizeof(key), "DLSSNR.%sSubrectHeight", name);
        GetUInt(params, key, &r.h);
        if (r.h == 0)
            r.h = defaultH; // the shaders divide by the rect
        std::snprintf(key, sizeof(key), "DLSSNR.%sSubrectBaseX", name);
        GetUInt(params, key, &r.x);
        std::snprintf(key, sizeof(key), "DLSSNR.%sSubrectBaseY", name);
        GetUInt(params, key, &r.y);
    }
    else
    {
        r.w = defaultW;
        r.h = defaultH;
    }
    return r;
}

void DescribeExtents(void *params, char *out, std::size_t size)
{
    std::size_t used = 0;
    auto put = [&](const char *text) {
        if (used + 1 >= size)
            return;
        const int n = std::snprintf(out + used, size - used, "%s", text);
        if (n > 0)
            used += static_cast<std::size_t>(n) < size - used ? static_cast<std::size_t>(n) : size - used - 1;
    };
    char item[96];
    for (const auto &k : kSizeKeys)
    {
        unsigned int w = 0, h = 0;
        const bool hw = GetUInt(params, k.w, &w), hh = GetUInt(params, k.h, &h);
        if (!hw && !hh)
            continue;
        std::snprintf(item, sizeof(item), "%s %ux%u, ", k.w, w, h);
        put(item);
    }
    unsigned int upscaling = 0;
    std::snprintf(item, sizeof(item), "DLSSNR.Upscaling %s, ",
                  GetUInt(params, "DLSSNR.Upscaling", &upscaling) ? (upscaling != 0 ? "1" : "0") : "absent");
    put(item);
    for (const char *name : kSubrectNames)
    {
        char key[64];
        unsigned int x = 0, y = 0, w = 0, h = 0;
        std::snprintf(key, sizeof(key), "DLSSNR.%sSubrectWidth", name);
        if (!GetUInt(params, key, &w))
        {
            std::snprintf(item, sizeof(item), "%s rect absent; ", name);
            put(item);
            continue;
        }
        std::snprintf(key, sizeof(key), "DLSSNR.%sSubrectHeight", name);
        GetUInt(params, key, &h);
        std::snprintf(key, sizeof(key), "DLSSNR.%sSubrectBaseX", name);
        GetUInt(params, key, &x);
        std::snprintf(key, sizeof(key), "DLSSNR.%sSubrectBaseY", name);
        GetUInt(params, key, &y);
        std::snprintf(item, sizeof(item), "%s rect %u,%u %ux%u; ", name, x, y, w, h);
        put(item);
    }
    float sx = 1.0f, sy = 1.0f;
    const bool read = ReadMotionScale(params, &sx, &sy);
    char details[384];
    std::snprintf(details, sizeof(details), "MVecScale %s (float setter slot %d getter slot %d); ",
                  read ? "read" : "NOT READ, defaulted to 1", FloatSetterSlot(), FloatGetterSlot());
    put(details);
    DescribeModelKeys(params, details, sizeof(details));
    put(details);
}

void WriteSubrect(void *params, const char *name, unsigned int x, unsigned int y, unsigned int w, unsigned int h)
{
    char key[64];
    std::snprintf(key, sizeof(key), "DLSSNR.%sSubrectBaseX", name);
    SetUInt(params, key, x);
    std::snprintf(key, sizeof(key), "DLSSNR.%sSubrectBaseY", name);
    SetUInt(params, key, y);
    std::snprintf(key, sizeof(key), "DLSSNR.%sSubrectWidth", name);
    SetUInt(params, key, w);
    std::snprintf(key, sizeof(key), "DLSSNR.%sSubrectHeight", name);
    SetUInt(params, key, h);
}

} // namespace ofps::reshade

namespace ofps::reshade
{
bool ReadMotionScale(void *params, float *x, float *y)
{
    float sx = 1.0f, sy = 1.0f;
    const bool readX = GetFloat(params, "DLSSNR.MVecScaleX", &sx) && std::isfinite(sx) && sx != 0.0f;
    const bool readY = GetFloat(params, "DLSSNR.MVecScaleY", &sy) && std::isfinite(sy) && sy != 0.0f;
    *x = readX ? sx : 1.0f;
    *y = readY ? sy : 1.0f;
    return readX && readY;
}
UINT DepthPlaneSubresource(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
    case DXGI_FORMAT_X24_TYPELESS_G8_UINT:
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
    case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:
        return 0u;
    default:
        return D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    }
}
namespace
{
OfpsResource FrameResource(void *params, const char *pointerKey, const char *name, D3D12_RESOURCE_STATES restState)
{
    OfpsResource r{};
    r.size = sizeof(r);
    r.res = static_cast<ID3D12Resource *>(GetResource(params, pointerKey));
    r.view = DXGI_FORMAT_UNKNOWN;
    r.restState = restState;
    r.subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    if (r.res == nullptr)
        return r;
    const D3D12_RESOURCE_DESC desc = r.res->GetDesc();
    const unsigned int texW = static_cast<unsigned int>(desc.Width), texH = desc.Height;
    if (name != nullptr)
    {
        const Subrect s = ReadSubrect(params, name, texW, texH);
        r.rect = OfpsRect{s.x, s.y, s.w, s.h};
    }
    else
    {
        r.rect = OfpsRect{0, 0, texW, texH};
    }
    return r;
}
} // namespace
bool ReadFrameInputs(void *params, OfpsFrameInputs *out, ShellFrameInfo *info)
{
    constexpr D3D12_RESOURCE_STATES kInput = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    constexpr D3D12_RESOURCE_STATES kOutput = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    if (!out)
        return false;
    OfpsFrameInputs f{};
    f.size = sizeof(f);
    // Pack assumes NPSR for colour even when the host output aliases it (26.28).
    f.color = FrameResource(params, "DLSSNR.Color", "Color", kInput);
    f.depth = FrameResource(params, "DLSSNR.Depth", "Depth", kInput);
    f.motion = FrameResource(params, "DLSSNR.MVec", "MVec", kInput);
    f.output = FrameResource(params, "DLSSNR.Output", "Output", kOutput);
    f.ui = FrameResource(params, "DLSSNR.UI", nullptr, kInput);
    f.uiAlpha = FrameResource(params, "DLSSNR.UIAlpha", nullptr, kInput);
    f.backbuffer = FrameResource(params, "DLSSNR.Backbuffer", nullptr, kInput);
    for (OfpsResource &c : f.codecInputs)
    {
        c = OfpsResource{};
        c.size = sizeof(c);
        c.view = DXGI_FORMAT_UNKNOWN;
        c.subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    }
    if (f.depth.res != nullptr)
        f.depth.subresource = DepthPlaneSubresource(f.depth.res->GetDesc().Format);
    const bool read = ReadMotionScale(params, &f.mvScaleX, &f.mvScaleY);
    ShellFrameInfo details{};
    details.motionScaleRead = read;
    details.floatGetterSlot = FloatGetterSlot();
    details.floatSetterSlot = FloatSetterSlot();
    if (f.motion.res)
    {
        const auto md = f.motion.res->GetDesc();
        details.motionTexW = static_cast<uint32_t>(md.Width);
        details.motionTexH = md.Height;
    }
    details.motionRectW = f.motion.rect.w;
    details.motionRectH = f.motion.rect.h;
    details.scaleX = f.mvScaleX;
    details.scaleY = f.mvScaleY;
    if (info)
        *info = details;
    unsigned int depthInverted = 0, reset = 0;
    GetUInt(params, "DLSSNR.DepthInverted", &depthInverted);
    GetUInt(params, "DLSSNR.Reset", &reset);
    f.depthInverted = depthInverted;
    f.hostReset = reset;
    f.colorDomain = OFPS_COLOR_DISPLAY_REFERRED;
    *out = f;
    return f.color.res != nullptr && f.depth.res != nullptr && f.motion.res != nullptr && f.output.res != nullptr;
}

} // namespace ofps::reshade

namespace ofps::reshade
{
void DescribeModelKeys(void *params, char *out, std::size_t size)
{
    unsigned int uiCorrection = 0, enabled = 0, style = 0, autoMask = 0;
    float intensity = 0.0f, localTone = 0.0f, localStructure = 0.0f, paperWhite = 0.0f;
    const bool hasUiCorrection = GetUInt(params, "DLSSNR.UICorrection", &uiCorrection);
    GetUInt(params, "DLSSNR.Enabled", &enabled);
    GetUInt(params, "DLSSNR.Style", &style);
    GetUInt(params, "DLSSNR.UseAutoMask", &autoMask);
    GetFloat(params, "DLSSNR.Intensity", &intensity);
    GetFloat(params, "DLSSNR.LocalToneStrength", &localTone);
    GetFloat(params, "DLSSNR.LocalStructureStrength", &localStructure);
    const bool hasPaperWhite =
        GetFloat(params, "DLSSNR.PaperWhiteNits", &paperWhite) || GetFloat(params, "DLSSNR.PaperWhite", &paperWhite);
    std::snprintf(out, size,
                  "UICorrection %u (%s), Enabled %u, Style %u, AutoMask %u, Intensity "
                  "%.3f, LocalTone %.3f, LocalStructure %.3f, PaperWhite %.3f (%s)",
                  uiCorrection, hasUiCorrection ? "set" : "unset", enabled, style, autoMask, intensity, localTone,
                  localStructure, paperWhite, hasPaperWhite ? "set" : "unset");
}
} // namespace ofps::reshade

namespace ofps::reshade {
bool ReadFeatureDesc(void *params, OfpsFeatureDesc *out) {
    if (!params || !out) return false;
    *out = {}; out->size = sizeof(*out);
    if (!GetUInt(params, "DLSSNR.Width", &out->width) || !GetUInt(params, "DLSSNR.Height", &out->height) || !out->width || !out->height) return false;
    auto *resource = GetResource(params, "DLSSNR.Color");
    if (!resource) resource = GetResource(params, "DLSSNR.Output");
    if (!resource || FAILED(resource->GetDevice(IID_PPV_ARGS(&out->resourceDevice)))) return false;
    out->adapterLuid = out->resourceDevice->GetAdapterLuid(); return true;
}
}
