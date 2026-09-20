#ifndef PW_SAMPLING_MATH_HLSLI
#define PW_SAMPLING_MATH_HLSLI
#ifdef __cplusplus
#define PW_INLINE inline
#else
#define PW_INLINE
#endif
// Deterministic complementary weights based on the source lamp proposal, NOT a
// guessed RIS PDF. RIS integrates f*wL with its original target and normalization;
// the BSDF estimator integrates f*(1-wL), so their expectations sum to f for any M.
PW_INLINE float LampMis(float lampPdf, float bsdfPdf)
{
    return lampPdf>0 ? lampPdf/(lampPdf+bsdfPdf) : 0;
}
PW_INLINE float MediumTransmittance(float density, float distance) { return exp(-density*distance); }
PW_INLINE float MediumDistance(float density, float distance, float u)
{
    return density>0 ? -log(1-u*(1-MediumTransmittance(density,distance)))/density : u*distance;
}
PW_INLINE float PhaseHG(float cosine, float g)
{
    float d=1+g*g-2*g*cosine;
    return (1-g*g)/(12.56637061436f*d*sqrt(d));
}
#undef PW_INLINE
#endif
