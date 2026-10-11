#ifndef PW_LOCAL_EXPOSURE_HLSLI
#define PW_LOCAL_EXPOSURE_HLSLI
// --local-exposure: UE5's local exposure (PostProcessLocalExposure.ush), shared with the CPU checks (hlsl_compat.h).
// A bilateral grid of the frame's log2 luminance (after the global exposure): LE_CELL pixels square per cell, LE_SLICES
// luminance slices from LE_LOG_LOW to LE_LOG_HIGH (half a stop each). Sliced at a pixel's own luminance it gives the mean
// log luminance of its neighbours that are about as bright (an edge-aware blur); mixed with a plain wide blur, that is
// the pixel's local base. The base is pulled towards mid grey by the highlight or shadow contrast scale, the detail
// (pixel minus base) is kept: sunlit and shaded regions move closer together without flattening their texture.
#ifdef __cplusplus
#define PW_INLINE inline
#define PW_UINT unsigned
#else
#define PW_INLINE
#define PW_UINT uint
#endif
#define LE_CELL 64
#define LE_SLICES 32
#define LE_LOG_LOW (-10.0f)
#define LE_LOG_HIGH (6.0f)
#define LE_MIDDLE_GREY (-2.4739312f) // log2(0.18)
PW_INLINE float LeLuminance(float3 c) { return dot(c,float3(0.2126f,0.7152f,0.0722f)); }
// The grid's depth coordinate (0..1) of a log2 luminance.
PW_INLINE float LocalExposureDepth(float log2Luminance)
{
    return saturate((log2Luminance-LE_LOG_LOW)/(LE_LOG_HIGH-LE_LOG_LOW));
}
PW_INLINE PW_UINT LocalExposureSlice(float log2Luminance)
{
    return PW_UINT(min(LocalExposureDepth(log2Luminance)*LE_SLICES,LE_SLICES-0.5f));
}
// The factor a pixel of log2 luminance `pixel` is multiplied by, given the grid's edge-aware base `bilateral` and the
// wide blur `blurred` there (both log2). UE's defaults: blend 0.6 of the blur, detail 1, contrast scales 1 (none); the
// bench's defaults (RASTER.md, "Local exposure") are 0.8 for both, which brings sun and shade 20 % closer in stops.
PW_INLINE float LocalExposureFactor(float pixel, float bilateral, float blurred, float highlights, float shadows, float detail, float blend)
{
    float base=lerp(bilateral,blurred,blend);
    float contrast=base>LE_MIDDLE_GREY ? highlights : shadows;
    float local=LE_MIDDLE_GREY+(base-LE_MIDDLE_GREY)*contrast+(pixel-base)*detail;
    return exp2(local-pixel);
}
#undef PW_UINT
#undef PW_INLINE
#endif
