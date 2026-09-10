#ifndef PERIPHERAL_WARP_COMMON_HLSLI
#define PERIPHERAL_WARP_COMMON_HLSLI

#ifndef PW_WARP_CONSTANTS_REGISTER
#define PW_WARP_CONSTANTS_REGISTER b0
#endif

cbuffer PwWarpConstants : register(PW_WARP_CONSTANTS_REGISTER)
{
    float4 PwNativeWorkSize;       // native width/height, work width/height
    float4 PwCenterWorkFractions;  // center X/Y, work X/Y (symmetric values)
    float4 PwCompressionEdgeSlope; // c X/Y, edge slope X/Y (harder side)
    uint4  PwOptions;              // mode, color filter, flags, reserved
    // Per-side mapping around the (possibly offset) centre band; pw::ShaderConstantsV3.
    float4 PwBandCenter;           // band centre native px X/Y, work centre work px X/Y
    float4 PwSideNeg;              // half-span X/Y, centre fraction X/Y (negative side)
    float4 PwSidePos;              // the same for the positive side
    float4 PwSideWorkNeg;          // work fraction X/Y, compression X/Y (negative side)
    float4 PwSideWorkPos;          // the same for the positive side
    float4 PwSideEdgeSlope;        // edge slope negative X/Y, positive X/Y
    float4 PwWorkScale;            // work px per native-scaled px X/Y, centre offset fraction X/Y
};

#ifndef PW_DIAGNOSTIC_CONSTANTS_REGISTER
#define PW_DIAGNOSTIC_CONSTANTS_REGISTER b2
#endif

cbuffer PwDiagnosticConstants : register(PW_DIAGNOSTIC_CONSTANTS_REGISTER)
{
    uint4 PwDiagnosticOptions; // x outline flags, y output gain (float bits, 0 = 1), z output 1/gamma (float bits, 0 = 1)
};

static const uint PW_MODE_OFF = 0;
static const uint PW_MODE_UNIFORM = 1;
static const uint PW_MODE_PERIPHERAL = 2;
static const uint PW_FILTER_BILINEAR = 0;
static const uint PW_FILTER_ADAPTIVE_FOUR_TAP = 1;
static const uint PW_FLAG_EXTEND_MOTION_AT_EDGE = 1u;
static const uint PW_DIAGNOSTIC_OUTLINE_CENTER = 1u;
static const uint PW_DIAGNOSTIC_OUTLINE_RAW_WORK = 2u;

float PwRectangleSignedDistance(float2 pixel, float2 lo, float2 hi)
{
    return max(max(lo.x - pixel.x, pixel.x - hi.x),
               max(lo.y - pixel.y, pixel.y - hi.y));
}

// Native-pixel rectangles of the 1:1 centre band and of the raw Work region, per side.
void PwCenterRectangle(out float2 lo, out float2 hi)
{
    lo = PwBandCenter.xy - PwSideNeg.zw * PwSideNeg.xy;
    hi = PwBandCenter.xy + PwSidePos.zw * PwSidePos.xy;
}

void PwWorkRectangle(out float2 lo, out float2 hi)
{
    lo = PwBandCenter.xy - PwSideWorkNeg.xy * PwSideNeg.xy;
    hi = PwBandCenter.xy + PwSideWorkPos.xy * PwSidePos.xy;
}

bool PwDiagnosticOutlineColor(float2 nativePixel, out float4 color)
{
    color = 0.0;
    if (PwOptions.x != PW_MODE_PERIPHERAL)
        return false;

    const bool centerEnabled =
        (PwDiagnosticOptions.x & PW_DIAGNOSTIC_OUTLINE_CENTER) != 0u;
    const bool workEnabled =
        (PwDiagnosticOptions.x & PW_DIAGNOSTIC_OUTLINE_RAW_WORK) != 0u;
    float2 centerLo, centerHi, workLo, workHi;
    PwCenterRectangle(centerLo, centerHi);
    PwWorkRectangle(workLo, workHi);
    const float centerDistance = centerEnabled
        ? PwRectangleSignedDistance(nativePixel, centerLo, centerHi) : 2.0;
    const float workDistance = workEnabled
        ? PwRectangleSignedDistance(nativePixel, workLo, workHi) : 2.0;

    // Bright pixels are resolved before either dark outer pixel so two nearby
    // boundaries remain visible. Center wins only if the bright bands overlap.
    if (centerDistance <= 0.0 && centerDistance >= -2.0)
    {
        color = float4(0.0, 0.82, 1.0, 1.0);
        return true;
    }
    if (workDistance <= 0.0 && workDistance >= -2.0)
    {
        color = float4(1.0, 0.45, 0.0, 1.0);
        return true;
    }
    if ((centerDistance > 0.0 && centerDistance <= 1.0) ||
        (workDistance > 0.0 && workDistance <= 1.0))
    {
        color = float4(0.025, 0.025, 0.025, 1.0);
        return true;
    }
    return false;
}

float PwSignNotZero(float value)
{
    return value < 0.0 ? -1.0 : 1.0;
}

float PwPackRadius(float radius, float center, float work, float compression, float edgeSlope)
{
    radius = abs(radius);
    float result = radius;
    if (work < 1.0 && center < 1.0)
    {
        if (radius > 1.0)
            result = work + (radius - 1.0) * edgeSlope;
        else if (radius > center)
        {
            float t = (radius - center) / (1.0 - center);
            result = center + (work - center) * t / (compression + (1.0 - compression) * t);
        }
    }
    return result;
}

float PwUnpackRadius(float packedRadius, float center, float work, float compression, float edgeSlope)
{
    packedRadius = abs(packedRadius);
    float result = packedRadius;
    if (work < 1.0 && center < 1.0)
    {
        if (packedRadius > work)
            result = 1.0 + (packedRadius - work) / edgeSlope;
        else if (packedRadius > center)
        {
            float y = (packedRadius - center) / (work - center);
            float t = compression * y / (1.0 - (1.0 - compression) * y);
            result = center + (1.0 - center) * t;
        }
    }
    return result;
}

// Per-side curve parameters of one axis: .x negative side, .y positive side.
struct PwAxisSides
{
    float nativeExtent;
    float workExtent;
    float bandCenter;   // native px
    float workCenter;   // work px
    float2 halfSpan;
    float2 center;
    float2 work;
    float2 compression;
    float2 edgeSlope;
    float scale;
};

PwAxisSides PwSidesX()
{
    PwAxisSides s;
    s.nativeExtent = PwNativeWorkSize.x;
    s.workExtent = PwNativeWorkSize.z;
    s.bandCenter = PwBandCenter.x;
    s.workCenter = PwBandCenter.z;
    s.halfSpan = float2(PwSideNeg.x, PwSidePos.x);
    s.center = float2(PwSideNeg.z, PwSidePos.z);
    s.work = float2(PwSideWorkNeg.x, PwSideWorkPos.x);
    s.compression = float2(PwSideWorkNeg.z, PwSideWorkPos.z);
    s.edgeSlope = float2(PwSideEdgeSlope.x, PwSideEdgeSlope.z);
    s.scale = PwWorkScale.x;
    return s;
}

PwAxisSides PwSidesY()
{
    PwAxisSides s;
    s.nativeExtent = PwNativeWorkSize.y;
    s.workExtent = PwNativeWorkSize.w;
    s.bandCenter = PwBandCenter.y;
    s.workCenter = PwBandCenter.w;
    s.halfSpan = float2(PwSideNeg.y, PwSidePos.y);
    s.center = float2(PwSideNeg.w, PwSidePos.w);
    s.work = float2(PwSideWorkNeg.y, PwSideWorkPos.y);
    s.compression = float2(PwSideWorkNeg.w, PwSideWorkPos.w);
    s.edgeSlope = float2(PwSideEdgeSlope.y, PwSideEdgeSlope.w);
    s.scale = PwWorkScale.y;
    return s;
}

float PwPackAxis(float nativePixel, PwAxisSides s)
{
    float result = nativePixel;
    if (PwOptions.x == PW_MODE_OFF || PwOptions.x == PW_MODE_UNIFORM)
        result = nativePixel * s.workExtent / s.nativeExtent;
    else if (PwOptions.x == PW_MODE_PERIPHERAL)
    {
        float delta = nativePixel - s.bandCenter;
        bool positive = delta >= 0.0;
        float halfSpan = positive ? s.halfSpan.y : s.halfSpan.x;
        float center = positive ? s.center.y : s.center.x;
        float work = positive ? s.work.y : s.work.x;
        float compression = positive ? s.compression.y : s.compression.x;
        float edgeSlope = positive ? s.edgeSlope.y : s.edgeSlope.x;
        float radius = abs(delta) / halfSpan;
        float packed = PwPackRadius(radius, center, work, compression, edgeSlope);
        result = s.workCenter + PwSignNotZero(delta) * packed * halfSpan * s.scale;
    }
    return result;
}

float PwUnpackAxis(float workPixel, PwAxisSides s)
{
    float result = workPixel;
    if (PwOptions.x == PW_MODE_OFF || PwOptions.x == PW_MODE_UNIFORM)
        result = workPixel * s.nativeExtent / s.workExtent;
    else if (PwOptions.x == PW_MODE_PERIPHERAL)
    {
        float delta = workPixel - s.workCenter;
        bool positive = delta >= 0.0;
        float halfSpan = positive ? s.halfSpan.y : s.halfSpan.x;
        float center = positive ? s.center.y : s.center.x;
        float work = positive ? s.work.y : s.work.x;
        float compression = positive ? s.compression.y : s.compression.x;
        float edgeSlope = positive ? s.edgeSlope.y : s.edgeSlope.x;
        float packed = abs(delta) / (halfSpan * s.scale);
        float radius = PwUnpackRadius(packed, center, work, compression, edgeSlope);
        result = s.bandCenter + PwSignNotZero(delta) * radius * halfSpan;
    }
    return result;
}

float2 PwPackNativePixel(float2 nativePixel)
{
    return float2(PwPackAxis(nativePixel.x, PwSidesX()), PwPackAxis(nativePixel.y, PwSidesY()));
}

float2 PwUnpackWorkPixel(float2 workPixel)
{
    return float2(PwUnpackAxis(workPixel.x, PwSidesX()), PwUnpackAxis(workPixel.y, PwSidesY()));
}

float2 PwPackMotionPixels(float2 nativeCurrentPixel, float2 nativeMotionPixels)
{
    float2 nativePreviousPixel = nativeCurrentPixel + nativeMotionPixels;
    if ((PwOptions.z & PW_FLAG_EXTEND_MOTION_AT_EDGE) == 0u)
    {
        nativeCurrentPixel = clamp(nativeCurrentPixel, 0.5.xx, PwNativeWorkSize.xy - 0.5.xx);
        nativePreviousPixel = clamp(nativePreviousPixel, 0.5.xx, PwNativeWorkSize.xy - 0.5.xx);
    }
    float2 packedCurrent = PwPackNativePixel(nativeCurrentPixel);
    float2 packedPrevious = PwPackNativePixel(nativePreviousPixel);
    return packedPrevious - packedCurrent;
}

float2 PwUnpackMotionPixels(float2 workCurrentPixel, float2 workMotionPixels)
{
    float2 workPreviousPixel = workCurrentPixel + workMotionPixels;
    if ((PwOptions.z & PW_FLAG_EXTEND_MOTION_AT_EDGE) == 0u)
    {
        workCurrentPixel = clamp(workCurrentPixel, 0.5.xx, PwNativeWorkSize.zw - 0.5.xx);
        workPreviousPixel = clamp(workPreviousPixel, 0.5.xx, PwNativeWorkSize.zw - 0.5.xx);
    }
    float2 nativeCurrent = PwUnpackWorkPixel(workCurrentPixel);
    float2 nativePrevious = PwUnpackWorkPixel(workPreviousPixel);
    return nativePrevious - nativeCurrent;
}

float2 PwSourceFootprint(float2 workPixel)
{
    float2 lo = PwUnpackWorkPixel(workPixel - 0.5);
    float2 hi = PwUnpackWorkPixel(workPixel + 0.5);
    return max(abs(hi - lo), 1.0.xx);
}

#endif
