#ifndef PW_DISPLAY_MATH_HLSLI
#define PW_DISPLAY_MATH_HLSLI
#ifdef __cplusplus
#define PW_INLINE inline
#else
#define PW_INLINE
#endif
PW_INLINE float Filmic(float x)
{
    return (x*(2.51f*x+0.03f))/(x*(2.43f*x+0.59f)+0.14f);
}
PW_INLINE float AdaptExposure(float previous, float logLuminance, float dt)
{
    float target=0.18f/exp(logLuminance);
    return previous+(target-previous)*(1-exp(-dt));
}
#undef PW_INLINE
#endif
