#ifndef PW_DISPLAY_MATH_HLSLI
#define PW_DISPLAY_MATH_HLSLI
#ifdef __cplusplus
#define PW_INLINE inline
#define PW_BINS const unsigned
#define PW_UINT unsigned
#else
#define PW_INLINE
#define PW_BINS uint
#define PW_UINT uint
#endif
PW_INLINE float Filmic(float x)
{
    return (x*(2.51f*x+0.03f))/(x*(2.43f*x+0.59f)+0.14f);
}
// The auto exposure's metering histogram: METER_BINS bins of log2 luminance (of the colour as rendered, --exposure
// already applied) from METER_LOW, METER_STEP stops each.
#define METER_BINS 64
#define METER_LOW (-20.0f)
#define METER_STEP 0.5f
// Highlight protection: the luminance HIGHLIGHT_PERCENTILE of the (centre-weighted) pixels lie below maps at most to
// HIGHLIGHT_KEY before the tone curve, which Filmic takes to 0.30 (sRGB 0.58). In the sunny garden that percentile is
// the sunlit sand itself (a third of the frame and more), so it reads as a mid-light beige with its texture visible; the
// average alone (0.18) left it 0.6-0.9 stops brighter, and the earlier key 0.32 (sRGB 0.71) still read pale. Indoors it is the lamps and their reflections: the hall is metered about 1 stop below its
// average, still 6-7 stops above the garden.
#define HIGHLIGHT_PERCENTILE 0.9f
#define HIGHLIGHT_KEY 0.20f
PW_INLINE PW_UINT MeterBin(float luminance)
{
    float bin=floor((log2(luminance>1e-30f ? luminance : 1e-30f)-METER_LOW)/METER_STEP);
    return PW_UINT(bin<0 ? 0 : bin>METER_BINS-1 ? METER_BINS-1 : bin);
}
// log2 of the luminance below which `fraction` of the histogram's weight lies (linear within the bin).
PW_INLINE float MeterPercentile(PW_BINS bins[METER_BINS], float fraction)
{
    float total=0;
    for(int i=0;i<METER_BINS;++i) total+=float(bins[i]);
    float goal=fraction*total, below=0;
    for(int k=0;k<METER_BINS;++k) {
        float here=float(bins[k]);
        if(here>0 && below+here>=goal) return METER_LOW+METER_STEP*(float(k)+(goal-below)/here);
        below+=here;
    }
    return METER_LOW+METER_STEP*METER_BINS;
}
// The scale auto exposure aims at: the log-average luminance to 0.18 (a mid grey), but never so high that the highlight
// percentile (log2) goes past HIGHLIGHT_KEY; times evScale (2^--exposure-ev).
PW_INLINE float MeterTarget(float logAverage, float highlightLog2, float evScale)
{
    float average=0.18f/exp(logAverage), highlight=HIGHLIGHT_KEY/exp2(highlightLog2);
    return (average<highlight ? average : highlight)*evScale;
}
// --exposure-metering ue: Unreal Engine's histogram auto exposure. The mean log2 luminance of the pixels between the
// lowFraction and highFraction of the histogram's weight (UE's "Low/High Percent" 10 and 90: the darkest and brightest
// tenth are ignored), bins at their centres.
PW_INLINE float MeterBandAverage(PW_BINS bins[METER_BINS], float lowFraction, float highFraction)
{
    float total=0;
    for(int i=0;i<METER_BINS;++i) total+=float(bins[i]);
    float low=lowFraction*total, high=highFraction*total, below=0, sum=0, weight=0;
    for(int k=0;k<METER_BINS;++k) {
        float here=float(bins[k]), from=below>low ? below : low, to=below+here<high ? below+here : high;
        if(to>from) { sum+=(to-from)*(METER_LOW+METER_STEP*(float(k)+0.5f)); weight+=to-from; }
        below+=here;
    }
    return weight>0 ? sum/weight : METER_LOW;
}
// The UE target: that mean to 0.18, kept within [minScale, maxScale] (UE's Min/Max EV100 as scales, MeterScaleBounds in
// display.h), then the compensation evScale on top as UE applies "Exposure Compensation" after the clamp.
PW_INLINE float UeMeterTarget(float bandLog2, float evScale, float minScale, float maxScale)
{
    float target=0.18f/exp2(bandLog2);
    target=target<minScale ? minScale : target>maxScale ? maxScale : target;
    return target*evScale;
}
// The scene's EV100 from the metered mean log2 luminance of the colour (radiance times --exposure): EV100 = log2(L S / K),
// S = 100, K = 12.5, of the radiance. The garden in the sun meters about 7, the hall about -0.5.
PW_INLINE float SceneEv100(float bandLog2, float exposure) { return bandLog2-log2(exposure>1e-30f ? exposure : 1e-30f)+3; }
// UE's Exposure Compensation Curve as a ramp: no extra compensation up to the scene EV100 `low`, `bias` stops from `high`,
// linear between. It is applied with --exposure-ev after the EV100 bounds, as UE does.
PW_INLINE float ExposureCurveBias(float sceneEv100, float low, float high, float bias)
{
    float t=(sceneEv100-low)/(high-low);
    return bias*(t<0 ? 0 : t>1 ? 1 : t);
}
// Auto exposure moves the scale towards the target in log2 space, as a game's eye adaptation: towards a brighter scene
// (the scale falls, e.g. stepping outdoors) at speedUp per second, towards a darker one at speedDown. Each frame closes
// 1 - exp(-speed * dt) of the remaining difference in stops, so it is frame-rate independent.
PW_INLINE float AdaptExposure(float previous, float targetScale, float dt, float speedUp, float speedDown)
{
    float target=log2(targetScale>1e-30f ? targetScale : 1e-30f);
    float current=log2(previous>1e-30f ? previous : 1e-30f);
    float speed=target<current ? speedUp : speedDown;
    return exp2(current+(target-current)*(1-exp(-speed*dt)));
}
#undef PW_BINS
#undef PW_UINT
#undef PW_INLINE
#endif
