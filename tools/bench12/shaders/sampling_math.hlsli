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
// The sun's transmittance from a scattering point of a camera ray to the medium's boundary (a sphere of `radius` around
// the camera), averaged over the ray's single-scattering distances: Haze() (haze.hlsli) samples a distance t up to
// `distance` with MediumDistance and attenuates the sun over the rest of the way to the boundary along the direction
// whose cosine with the ray is cosToSun. The raster fog takes that mean with `steps` midpoints of the same inverse CDF.
PW_INLINE float HazeSunTransmittance(float density, float distance, float cosToSun, float radius, int steps)
{
    float sum=0;
    for(int i=0;i<steps;++i) {
        float t=MediumDistance(density,distance,(float(i)+0.5f)/float(steps)), projected=t*cosToSun;
        float inside=radius*radius-t*t+projected*projected;
        float sunDistance=-projected+sqrt(inside>0 ? inside : 0.0f);
        sum+=MediumTransmittance(density,sunDistance>0 ? sunDistance : 0.0f);
    }
    return sum/float(steps);
}
PW_INLINE float PhaseHG(float cosine, float g)
{
    float d=1+g*g-2*g*cosine;
    return (1-g*g)/(12.56637061436f*d*sqrt(d));
}
#undef PW_INLINE
#endif
