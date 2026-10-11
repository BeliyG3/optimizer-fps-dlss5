#ifndef PW_TONEMAP_UE_HLSLI
#define PW_TONEMAP_UE_HLSLI
// --tonemap ue-filmic: Unreal Engine's filmic tone curve (TonemapCommon.ush FilmToneMap, the default since UE 4.15),
// shared with the CPU checks (hlsl_compat.h). The colour goes from linear sRGB to ACES AP0 for the RRT's glow and red
// modifier, then to ACEScg (AP1), where it is desaturated 4 %, put through the curve per channel (a log10 straight
// segment of slope `slope` through 0.18 -> 0.18 with a logistic toe and shoulder) and desaturated 7 %, and back to linear
// sRGB. Working in AP1 keeps saturated colours from skewing towards the sRGB primaries, and the desaturation lets bright
// saturated colours roll off towards white instead of clipping one channel. Not included: UE's colour grading, its blue
// correction and gamut expansion (Post Process "Blue Correction" / "Expand Gamut").
#ifdef __cplusplus
#define PW_INLINE inline
#else
#define PW_INLINE
#endif
// UE's Post Process "Film" settings; UeFilmDefault() is UE's default.
struct UeFilm { float slope, toe, shoulder, blackClip, whiteClip; };
PW_INLINE UeFilm UeFilmDefault() { UeFilm f; f.slope=0.88f; f.toe=0.55f; f.shoulder=0.26f; f.blackClip=0; f.whiteClip=0.04f; return f; }
PW_INLINE float3 UeRows(float3 r0, float3 r1, float3 r2, float3 c) { return float3(dot(r0,c),dot(r1,c),dot(r2,c)); }
// Linear sRGB (D65) <-> ACEScg (AP1, D60) with UE's Bradford adaptation; AP0 <-> AP1 (ACES 1.0).
PW_INLINE float3 SrgbToAp1(float3 c)
{
    return UeRows(float3(0.613191478f,0.339512089f,0.047366331f),float3(0.070206904f,0.916335817f,0.013450011f),
                  float3(0.020618871f,0.109567294f,0.869606747f),c);
}
PW_INLINE float3 Ap1ToSrgb(float3 c)
{
    return UeRows(float3(1.705051545f,-0.62179068f,-0.083258397f),float3(-0.130257144f,1.14080289f,-0.010548528f),
                  float3(-0.024003275f,-0.128968769f,1.152971708f),c);
}
PW_INLINE float3 Ap1ToAp0(float3 c)
{
    return UeRows(float3(0.695452241f,0.140678697f,0.163869062f),float3(0.044794563f,0.859671118f,0.095534318f),
                  float3(-0.005525883f,0.00402521f,1.001500672f),c);
}
PW_INLINE float3 Ap0ToAp1(float3 c)
{
    return UeRows(float3(1.451439316f,-0.236510747f,-0.214928569f),float3(-0.076553773f,1.1762297f,-0.099675926f),
                  float3(0.008316148f,-0.00603245f,0.997716301f),c);
}
PW_INLINE float Ap1Luminance(float3 c) { return dot(c,float3(0.2722287168f,0.6740817658f,0.0536895174f)); }
// The curve's joins in log10, solved so that 0.18 maps to 0.18 (FilmToneMap's ToeMatch, StraightMatch, ShoulderMatch).
struct UeCurveFit { float toeScale, shoulderScale, toeMatch, straightMatch, shoulderMatch; };
PW_INLINE UeCurveFit UeFit(UeFilm f)
{
    UeCurveFit m;
    m.toeScale=1+f.blackClip-f.toe; m.shoulderScale=1+f.whiteClip-f.shoulder;
    if(f.toe>0.8f) m.toeMatch=(1-f.toe-0.18f)/f.slope+log10(0.18f); // 0.18 on the straight segment
    else {
        float bt=(0.18f+f.blackClip)/m.toeScale-1;
        m.toeMatch=log10(0.18f)-0.5f*log((1+bt)/(1-bt))*(m.toeScale/f.slope);
    }
    m.straightMatch=(1-f.toe)/f.slope-m.toeMatch;
    m.shoulderMatch=f.shoulder/f.slope-m.straightMatch;
    return m;
}
PW_INLINE float UeCurve(float value, UeFilm f, UeCurveFit m)
{
    float lc=log10(max(value,1e-10f));
    float straight=f.slope*(lc+m.straightMatch);
    float toe=-f.blackClip+(2*m.toeScale)/(1+exp((-2*f.slope/m.toeScale)*(lc-m.toeMatch)));
    float shoulder=(1+f.whiteClip)-(2*m.shoulderScale)/(1+exp((2*f.slope/m.shoulderScale)*(lc-m.shoulderMatch)));
    toe=lc<m.toeMatch ? toe : straight;
    shoulder=lc>m.shoulderMatch ? shoulder : straight;
    float t=saturate((lc-m.toeMatch)/(m.shoulderMatch-m.toeMatch));
    t=m.shoulderMatch<m.toeMatch ? 1-t : t;
    t=(3-2*t)*t*t;
    return lerp(toe,shoulder,t);
}
// Linear sRGB in (exposure applied), linear sRGB out (0..1 after saturate, before the sRGB encode).
PW_INLINE float3 UeFilmToneMap(float3 linearSrgb, UeFilm f)
{
    float3 c=Ap1ToAp0(SrgbToAp1(linearSrgb));
    // RRT glow: lifts dark saturated colours a little (ACES rgb_2_saturation, rgb_2_yc, sigmoid_shaper, glow_fwd).
    float lowest=min(c.x,min(c.y,c.z)), highest=max(c.x,max(c.y,c.z));
    float saturation=(max(highest,1e-10f)-max(lowest,1e-10f))/max(highest,1e-2f);
    float chroma=sqrt(max(c.z*(c.z-c.y)+c.y*(c.y-c.x)+c.x*(c.x-c.z),0.0f));
    float yc=(c.z+c.y+c.x+1.75f*chroma)/3;
    float x=(saturation-0.4f)/0.2f, t=max(1-abs(0.5f*x),0.0f);
    float gain=0.05f*0.5f*(1+sign(x)*(1-t*t));
    float glow=yc<=2.0f/3*0.08f ? gain : yc>=2*0.08f ? 0.0f : gain*(0.08f/yc-0.5f);
    c=c*(1+glow);
    // RRT red modifier: pulls saturated reds' red channel towards 0.03 by 18 %, weighted around hue 0 (width 135 degrees).
    float hue=(c.x==c.y && c.y==c.z) ? 0.0f : 57.2957795f*atan2(sqrt(3.0f)*(c.y-c.z),2*c.x-c.y-c.z);
    float weight=smoothstep(0.0f,1.0f,1-abs(2*hue/135)); // atan2's -180..180 is already centred on red
    c.x=c.x+weight*weight*saturation*(0.03f-c.x)*(1-0.82f);
    float3 w=max(Ap0ToAp1(c),float3(0,0,0));
    w=lerp(float3(Ap1Luminance(w),Ap1Luminance(w),Ap1Luminance(w)),w,0.96f);
    UeCurveFit m=UeFit(f);
    float3 tone=float3(UeCurve(w.x,f,m),UeCurve(w.y,f,m),UeCurve(w.z,f,m));
    tone=lerp(float3(Ap1Luminance(tone),Ap1Luminance(tone),Ap1Luminance(tone)),tone,0.93f);
    return Ap1ToSrgb(max(tone,float3(0,0,0)));
}
#undef PW_INLINE
#endif
