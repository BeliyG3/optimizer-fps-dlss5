#ifndef PW_SPECULAR_GUIDE_HLSLI
#define PW_SPECULAR_GUIDE_HLSLI
#ifdef __cplusplus
#define PW_INLINE inline
#else
#define PW_INLINE
#endif
PW_INLINE float SpecularGuideF0(float roughness, float nv, float f0)
{
    // Scalar expansion of NVIDIA's EnvBRDFApprox2 (Ray Tracing Gems, chapter 32),
    // evaluated per colour channel of F0. The fit takes alpha=linearRoughness^2.
    // https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideDLSS_RR.md
    float n=nv<0 ? -nv : nv, n2=n*n, n3=n2*n;
    float a=roughness*roughness, a3=a*a*a;
    float bias=(0.99044f-1.28514f*n+a*(1.29678f-0.755907f*n))/
        (1+2.92338f*n+59.4188f*n3+a*(20.3225f-27.0302f*n+222.592f*n3)+a3*(121.563f+626.13f*n+316.627f*n3));
    float scale=(0.0365463f+3.32707f*n+a*(9.0632f-9.04756f*n))/
        (1+3.59685f*n2-1.36772f*n3+a*(9.04401f-16.3174f*n2+9.22949f*n3)+a3*(5.56589f+19.7886f*n2-20.2123f*n3));
    return f0*(scale>0 ? scale : 0)+(bias>0 ? bias : 0);
}
PW_INLINE float SpecularGuide(float roughness, float nv) { return SpecularGuideF0(roughness,nv,0.04f); }
#undef PW_INLINE
#endif
