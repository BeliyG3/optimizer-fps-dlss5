#pragma once
#include "core/api/ofps_core.h"
#include <cstddef>
#include <cstdint>
#include <d3d12.h>
namespace ofps::reshade
{
constexpr int kNgxSuccess = 1;
constexpr int kFeatureNeuralRendering = 18;
struct Subrect
{
    unsigned int x = 0, y = 0, w = 0, h = 0;
    bool present = false;
};
void SetUInt(void *params, const char *name, unsigned int value);
bool GetUInt(void *params, const char *name, unsigned int *value);
void SetFloat(void *params, const char *name, float value);
bool GetFloat(void *params, const char *name, float *value);
void ProbeKey(void *params, const char *name, char *out, std::size_t outSize);
int FloatSetterSlot();
int FloatGetterSlot();
void SetResource(void *params, const char *name, ID3D12Resource *value);
ID3D12Resource *GetResource(void *params, const char *name);
inline const char *kSubrectNames[] = {"Color", "Depth", "MVec", "Output"};
void WriteSizes(void *params, std::uint32_t w, std::uint32_t h);
void DescribeExtents(void *params, char *out, std::size_t size);
Subrect ReadSubrect(void *params, const char *name, unsigned int defaultW, unsigned int defaultH);
void WriteSubrect(void *params, const char *name, unsigned int x, unsigned int y, unsigned int w, unsigned int h);
} // namespace ofps::reshade

namespace ofps::reshade
{
struct SizeKeys
{
    const char *w, *h;
};
extern const SizeKeys kSizeKeys[3];
struct ShellFrameInfo
{
    bool motionScaleRead = false;
    int floatGetterSlot = -1, floatSetterSlot = -1;
    uint32_t motionTexW = 0, motionTexH = 0, motionRectW = 0, motionRectH = 0;
    float scaleX = 1, scaleY = 1;
};
bool ReadMotionScale(void *params, float *x, float *y);
UINT DepthPlaneSubresource(DXGI_FORMAT format);
bool ReadFrameInputs(void *params, OfpsFrameInputs *out, ShellFrameInfo *info);
} // namespace ofps::reshade

namespace ofps::reshade
{
void DescribeModelKeys(void *params, char *out, std::size_t size);
}

namespace ofps::reshade { bool ReadFeatureDesc(void *params, OfpsFeatureDesc *out); }
