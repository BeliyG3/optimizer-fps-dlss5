#ifndef PW_RESERVOIR_HLSLI
#define PW_RESERVOIR_HLSLI
#ifdef __cplusplus
#define PW_INLINE inline
#define PW_INOUT(T) T &
#else
#define PW_INLINE
#define PW_INOUT(T) inout T
#endif

// Shared with CPU estimator checks so selection and normalization are tested as used by HLSL.
struct LampReservoir { float weightSum, selectedTarget; };
PW_INLINE bool UpdateLampReservoir(PW_INOUT(LampReservoir) reservoir, float target, float sourcePdf, float randomValue)
{
    if(target<=0 || sourcePdf<=0) return false;
    float weight=target/sourcePdf;
    reservoir.weightSum+=weight;
    if(randomValue*reservoir.weightSum>=weight) return false;
    reservoir.selectedTarget=target;
    return true;
}
PW_INLINE float LampNormalization(LampReservoir reservoir, unsigned int candidates)
{
    // M includes rejected/zero-contribution candidates; visibility is applied only afterwards.
    return reservoir.selectedTarget>0 && candidates>0 ? (reservoir.weightSum/float(candidates))/reservoir.selectedTarget : 0;
}
#undef PW_INLINE
#undef PW_INOUT
#endif
