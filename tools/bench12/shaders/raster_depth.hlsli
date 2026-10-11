#ifndef PW_RASTER_DEPTH_HLSLI
#define PW_RASTER_DEPTH_HLSLI
#ifdef __cplusplus
#define PW_INLINE inline
#else
#define PW_INLINE
#endif
// View depth (clip w, positive in front of the camera) from a hardware depth value written with math.h's
// Perspective: near 0.1, far 300; standard z = f(w-n)/((f-n)w), reverse z = n(f-w)/((f-n)w).
// Shared with the CPU checks, so the resolve inverts the projection the bench actually uses.
PW_INLINE float ViewDepthFromHardware(float z, bool reverse)
{
    const float n=0.1f, f=300.0f;
    return reverse ? n*f/(z*(f-n)+n) : n*f/(f-z*(f-n));
}
#undef PW_INLINE
#endif
