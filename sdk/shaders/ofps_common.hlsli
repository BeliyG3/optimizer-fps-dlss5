#ifndef OFPS_COMMON_HLSLI
#define OFPS_COMMON_HLSLI

#ifndef OFPS_WARP_CONSTANTS_REGISTER
#define OFPS_WARP_CONSTANTS_REGISTER b0
#endif

cbuffer OfpsWarpConstants : register(OFPS_WARP_CONSTANTS_REGISTER)
{
    float4 OfpsNativeWorkSize;       // native width/height, work width/height
    float4 OfpsCenterWorkFractions;  // center X/Y, work X/Y (symmetric values)
    float4 OfpsCompressionEdgeSlope; // c X/Y, edge slope X/Y (harder side)
    uint4  OfpsOptions;              // mode, color filter, flags, reserved
    // Per-side mapping around the (possibly offset) centre band; ofps::sdk::ShaderConstantsV3.
    float4 OfpsBandCenter;           // band centre native px X/Y, work centre work px X/Y
    float4 OfpsSideNeg;              // half-span X/Y, centre fraction X/Y (negative side)
    float4 OfpsSidePos;              // the same for the positive side
    float4 OfpsSideWorkNeg;          // work fraction X/Y, compression X/Y (negative side)
    float4 OfpsSideWorkPos;          // the same for the positive side
    float4 OfpsSideEdgeSlope;        // edge slope negative X/Y, positive X/Y
    float4 OfpsWorkScale;            // work px per native-scaled px X/Y, centre offset fraction X/Y
};

#ifndef OFPS_DIAGNOSTIC_CONSTANTS_REGISTER
#define OFPS_DIAGNOSTIC_CONSTANTS_REGISTER b2
#endif

cbuffer OfpsDiagnosticConstants : register(OFPS_DIAGNOSTIC_CONSTANTS_REGISTER)
{
    uint4 OfpsDiagnosticOptions; // x outline flags, y output gain (float bits, 0 = 1), z output 1/gamma (float bits, 0 = 1)
};

static const uint OFPS_MODE_OFF = 0;
static const uint OFPS_MODE_UNIFORM = 1;
static const uint OFPS_MODE_PERIPHERAL = 2;
static const uint OFPS_FILTER_BILINEAR = 0;
static const uint OFPS_FILTER_ADAPTIVE_FOUR_TAP = 1;
static const uint OFPS_FLAG_EXTEND_MOTION_AT_EDGE = 1u;
static const uint OFPS_DIAGNOSTIC_OUTLINE_CENTER = 1u;
static const uint OFPS_DIAGNOSTIC_OUTLINE_RAW_WORK = 2u;

float OfpsRectangleSignedDistance(float2 pixel, float2 lo, float2 hi)
{
    return max(max(lo.x - pixel.x, pixel.x - hi.x),
               max(lo.y - pixel.y, pixel.y - hi.y));
}

// Native-pixel rectangles of the 1:1 centre band and of the raw Work region, per side.
void OfpsCenterRectangle(out float2 lo, out float2 hi)
{
    lo = OfpsBandCenter.xy - OfpsSideNeg.zw * OfpsSideNeg.xy;
    hi = OfpsBandCenter.xy + OfpsSidePos.zw * OfpsSidePos.xy;
}

void OfpsWorkRectangle(out float2 lo, out float2 hi)
{
    lo = OfpsBandCenter.xy - OfpsSideWorkNeg.xy * OfpsSideNeg.xy;
    hi = OfpsBandCenter.xy + OfpsSideWorkPos.xy * OfpsSidePos.xy;
}

bool OfpsDiagnosticOutlineColor(float2 nativePixel, out float4 color)
{
    color = 0.0;
    if (OfpsOptions.x != OFPS_MODE_PERIPHERAL)
        return false;

    const bool centerEnabled =
        (OfpsDiagnosticOptions.x & OFPS_DIAGNOSTIC_OUTLINE_CENTER) != 0u;
    const bool workEnabled =
        (OfpsDiagnosticOptions.x & OFPS_DIAGNOSTIC_OUTLINE_RAW_WORK) != 0u;
    float2 centerLo, centerHi, workLo, workHi;
    OfpsCenterRectangle(centerLo, centerHi);
    OfpsWorkRectangle(workLo, workHi);
    const float centerDistance = centerEnabled
        ? OfpsRectangleSignedDistance(nativePixel, centerLo, centerHi) : 2.0;
    const float workDistance = workEnabled
        ? OfpsRectangleSignedDistance(nativePixel, workLo, workHi) : 2.0;

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

float OfpsSignNotZero(float value)
{
    return value < 0.0 ? -1.0 : 1.0;
}

float OfpsPackRadius(float radius, float center, float work, float compression, float edgeSlope)
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

float OfpsUnpackRadius(float packedRadius, float center, float work, float compression, float edgeSlope)
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
struct OfpsAxisSides
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

OfpsAxisSides OfpsSidesX()
{
    OfpsAxisSides s;
    s.nativeExtent = OfpsNativeWorkSize.x;
    s.workExtent = OfpsNativeWorkSize.z;
    s.bandCenter = OfpsBandCenter.x;
    s.workCenter = OfpsBandCenter.z;
    s.halfSpan = float2(OfpsSideNeg.x, OfpsSidePos.x);
    s.center = float2(OfpsSideNeg.z, OfpsSidePos.z);
    s.work = float2(OfpsSideWorkNeg.x, OfpsSideWorkPos.x);
    s.compression = float2(OfpsSideWorkNeg.z, OfpsSideWorkPos.z);
    s.edgeSlope = float2(OfpsSideEdgeSlope.x, OfpsSideEdgeSlope.z);
    s.scale = OfpsWorkScale.x;
    return s;
}

OfpsAxisSides OfpsSidesY()
{
    OfpsAxisSides s;
    s.nativeExtent = OfpsNativeWorkSize.y;
    s.workExtent = OfpsNativeWorkSize.w;
    s.bandCenter = OfpsBandCenter.y;
    s.workCenter = OfpsBandCenter.w;
    s.halfSpan = float2(OfpsSideNeg.y, OfpsSidePos.y);
    s.center = float2(OfpsSideNeg.w, OfpsSidePos.w);
    s.work = float2(OfpsSideWorkNeg.y, OfpsSideWorkPos.y);
    s.compression = float2(OfpsSideWorkNeg.w, OfpsSideWorkPos.w);
    s.edgeSlope = float2(OfpsSideEdgeSlope.y, OfpsSideEdgeSlope.w);
    s.scale = OfpsWorkScale.y;
    return s;
}

float OfpsPackAxis(float nativePixel, OfpsAxisSides s)
{
    float result = nativePixel;
    if (OfpsOptions.x == OFPS_MODE_OFF || OfpsOptions.x == OFPS_MODE_UNIFORM)
        result = nativePixel * s.workExtent / s.nativeExtent;
    else if (OfpsOptions.x == OFPS_MODE_PERIPHERAL)
    {
        float delta = nativePixel - s.bandCenter;
        bool positive = delta >= 0.0;
        float halfSpan = positive ? s.halfSpan.y : s.halfSpan.x;
        float center = positive ? s.center.y : s.center.x;
        float work = positive ? s.work.y : s.work.x;
        float compression = positive ? s.compression.y : s.compression.x;
        float edgeSlope = positive ? s.edgeSlope.y : s.edgeSlope.x;
        float radius = abs(delta) / halfSpan;
        float packed = OfpsPackRadius(radius, center, work, compression, edgeSlope);
        result = s.workCenter + OfpsSignNotZero(delta) * packed * halfSpan * s.scale;
    }
    return result;
}

float OfpsUnpackAxis(float workPixel, OfpsAxisSides s)
{
    float result = workPixel;
    if (OfpsOptions.x == OFPS_MODE_OFF || OfpsOptions.x == OFPS_MODE_UNIFORM)
        result = workPixel * s.nativeExtent / s.workExtent;
    else if (OfpsOptions.x == OFPS_MODE_PERIPHERAL)
    {
        float delta = workPixel - s.workCenter;
        bool positive = delta >= 0.0;
        float halfSpan = positive ? s.halfSpan.y : s.halfSpan.x;
        float center = positive ? s.center.y : s.center.x;
        float work = positive ? s.work.y : s.work.x;
        float compression = positive ? s.compression.y : s.compression.x;
        float edgeSlope = positive ? s.edgeSlope.y : s.edgeSlope.x;
        float packed = abs(delta) / (halfSpan * s.scale);
        float radius = OfpsUnpackRadius(packed, center, work, compression, edgeSlope);
        result = s.bandCenter + OfpsSignNotZero(delta) * radius * halfSpan;
    }
    return result;
}

float2 OfpsPackNativePixel(float2 nativePixel)
{
    return float2(OfpsPackAxis(nativePixel.x, OfpsSidesX()), OfpsPackAxis(nativePixel.y, OfpsSidesY()));
}

float2 OfpsUnpackWorkPixel(float2 workPixel)
{
    return float2(OfpsUnpackAxis(workPixel.x, OfpsSidesX()), OfpsUnpackAxis(workPixel.y, OfpsSidesY()));
}

float2 OfpsPackMotionPixels(float2 nativeCurrentPixel, float2 nativeMotionPixels)
{
    float2 nativePreviousPixel = nativeCurrentPixel + nativeMotionPixels;
    if ((OfpsOptions.z & OFPS_FLAG_EXTEND_MOTION_AT_EDGE) == 0u)
    {
        nativeCurrentPixel = clamp(nativeCurrentPixel, 0.5.xx, OfpsNativeWorkSize.xy - 0.5.xx);
        nativePreviousPixel = clamp(nativePreviousPixel, 0.5.xx, OfpsNativeWorkSize.xy - 0.5.xx);
    }
    float2 packedCurrent = OfpsPackNativePixel(nativeCurrentPixel);
    float2 packedPrevious = OfpsPackNativePixel(nativePreviousPixel);
    return packedPrevious - packedCurrent;
}

float2 OfpsUnpackMotionPixels(float2 workCurrentPixel, float2 workMotionPixels)
{
    float2 workPreviousPixel = workCurrentPixel + workMotionPixels;
    if ((OfpsOptions.z & OFPS_FLAG_EXTEND_MOTION_AT_EDGE) == 0u)
    {
        workCurrentPixel = clamp(workCurrentPixel, 0.5.xx, OfpsNativeWorkSize.zw - 0.5.xx);
        workPreviousPixel = clamp(workPreviousPixel, 0.5.xx, OfpsNativeWorkSize.zw - 0.5.xx);
    }
    float2 nativeCurrent = OfpsUnpackWorkPixel(workCurrentPixel);
    float2 nativePrevious = OfpsUnpackWorkPixel(workPreviousPixel);
    return nativePrevious - nativeCurrent;
}

float2 OfpsSourceFootprint(float2 workPixel)
{
    float2 lo = OfpsUnpackWorkPixel(workPixel - 0.5);
    float2 hi = OfpsUnpackWorkPixel(workPixel + 0.5);
    return max(abs(hi - lo), 1.0.xx);
}

#endif
