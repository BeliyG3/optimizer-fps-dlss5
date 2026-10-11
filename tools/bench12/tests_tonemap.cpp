// --tonemap ue-filmic, --local-exposure and --exposure-metering ue: the shaders' own math (hlsl_compat.h) and the options.
#include "tests_raster_support.h"
#include "hlsl_compat.h"
#include "display.h"
#include "local_exposure.h"
#include "shaders/display_math.hlsli"
namespace hlsl {
#include "shaders/tonemap_ue.hlsli"
#include "shaders/local_exposure.hlsli"
}
#include <cstdio>
using namespace raster_tests;

namespace {
bool Close(float a, float b, float tolerance) { return std::abs(a-b)<=tolerance; }
// The hue (degrees) of a linear colour, as an HSV hue.
float Hue(hlsl::float3 c)
{
    const float high=std::max({c.x,c.y,c.z}), low=std::min({c.x,c.y,c.z}), d=high-low;
    if(d<=0) return 0;
    float h=high==c.x ? std::fmod((c.y-c.z)/d+6,6.0f) : high==c.y ? (c.z-c.x)/d+2 : (c.x-c.y)/d+4;
    return h*60;
}
float Saturation(hlsl::float3 c) { const float high=std::max({c.x,c.y,c.z}); return high>0 ? (high-std::min({c.x,c.y,c.z}))/high : 0; }
void FilmChecks()
{
    using namespace hlsl;
    const UeFilm film=UeFilmDefault(); const UeCurveFit fit=UeFit(film);
    Require(Close(UeCurve(0.18f,film,fit),0.18f,1e-4f),"UE curve: 0.18 maps to 0.18");
    Require(UeCurve(0,film,fit)<1e-4f && Close(UeCurve(1,film,fit),0.723f,0.005f) && UeCurve(100,film,fit)<=1.04f+1e-6f,
            "UE curve: black, 1.0 (UE: 0.72) and the white clip");
    float last=-1;
    for(float x=1e-4f;x<1e4f;x*=1.25f) { float y=UeCurve(x,film,fit); Require(y>=last,"UE curve is monotonic"); last=y; }
    UeFilm straight=film; straight.toe=0.9f;
    Require(Close(UeCurve(0.18f,straight,UeFit(straight)),0.18f,1e-3f),"UE curve: 0.18 on the straight segment (toe > 0.8)");
    // Grey stays grey and keeps the curve; the colour transforms round trip.
    const float3 grey=UeFilmToneMap(float3(0.18f,0.18f,0.18f),film);
    Require(Close(grey.x,grey.y,2e-3f) && Close(grey.y,grey.z,2e-3f) && Close(grey.y,0.18f,4e-3f),"UE filmic: 18 % grey");
    const float3 back=Ap1ToSrgb(SrgbToAp1(float3(0.3f,0.6f,0.1f)));
    Require(Close(back.x,0.3f,1e-3f) && Close(back.y,0.6f,1e-3f) && Close(back.z,0.1f,1e-3f),"sRGB <-> AP1 round trip");
    // The garden's colours as the owner saw them: a pink mannequin, a red barrel, a white suit, from shade to far past white.
    // UE's curve keeps the hue within a few degrees while the colour is in range and rolls it towards white as it brightens
    // (saturation falls, brightness rises). Far over white a saturated red drifts to orange (up to 25 degrees at 30x, ACES's
    // known red-to-orange shift; the per-channel fit drifts about half as far): not checked past 0.95.
    const float3 colours[]={float3(0.80f,0.36f,0.33f),float3(0.60f,0.05f,0.04f),float3(0.85f,0.85f,0.82f)};
    for(const float3 albedo:colours) {
        float lastHigh=0, lastSaturation=2;
        for(float k=0.05f;k<=40;k*=1.5f) {
            const float3 input=albedo*k, out=UeFilmToneMap(input,film);
            const float high=std::max({out.x,out.y,out.z});
            const float shown=0.2126f*saturate(out.x)+0.7152f*saturate(out.y)+0.0722f*saturate(out.z); // as displayed
            Require(shown>=lastHigh-1e-5f,"UE filmic: brighter in, brighter out");
            if(Saturation(albedo)>0.3f && k>=0.5f) {
                float dh=std::abs(Hue(out)-Hue(input)); dh=std::min(dh,360-dh);
                Require(high>0.95f || dh<8,"UE filmic keeps a saturated colour's hue in range");
                Require(Saturation(out)<=lastSaturation+1e-3f,"UE filmic rolls bright saturated colours towards white");
                lastSaturation=Saturation(out);
            }
            lastHigh=shown;
        }
        const float3 white=UeFilmToneMap(albedo*1000,film);
        Require(std::min({white.x,white.y,white.z})>0.97f,"UE filmic: far past white is white");
    }
}
void LocalChecks()
{
    using namespace hlsl;
    // The defaults of UE (contrast 1, detail 1) change nothing; the bench's 0.8 / 0.8 pull a flat region 20 % of its
    // distance from mid grey (in stops) towards it, both ways, and keep the detail.
    for(float pixel=-9;pixel<=5;pixel+=0.7f)
        Require(Close(LocalExposureFactor(pixel,pixel-0.4f,pixel+0.3f,1,1,1,0.6f),1,1e-5f),"local exposure off at contrast 1");
    const float sun=1.0f, shade=-5.0f;
    Require(Close(std::log2(LocalExposureFactor(sun,sun,sun,0.8f,0.8f,1,0.6f)),-0.2f*(sun-LE_MIDDLE_GREY),1e-4f),"highlights pulled down");
    Require(Close(std::log2(LocalExposureFactor(shade,shade,shade,0.8f,0.8f,1,0.6f)),-0.2f*(shade-LE_MIDDLE_GREY),1e-4f),"shadows lifted");
    const float a=0.5f+std::log2(LocalExposureFactor(0.5f,0,0,0.8f,0.8f,1,0.6f)), b=1.5f+std::log2(LocalExposureFactor(1.5f,0,0,0.8f,0.8f,1,0.6f));
    Require(Close(b-a,1,1e-4f),"detail (a pixel against its base) is kept at detail strength 1");
    Require(Close(std::log2(LocalExposureFactor(0,2,-2,0.8f,0.8f,1,0.5f))+0,0.2f*LE_MIDDLE_GREY,1e-4f),"base mixes the bilateral and the blur");
    Require(LocalExposureSlice(-30)==0 && LocalExposureSlice(LE_LOG_LOW+0.49f)==0 && LocalExposureSlice(LE_LOG_LOW+0.51f)==1 &&
            LocalExposureSlice(LE_LOG_HIGH)==LE_SLICES-1 && LocalExposureSlice(30)==LE_SLICES-1,"grid slices: half a stop each, clamped");
    Require(LocalExposureBlurLevel({960,480,240,120,60,30,15,8,4,2,1})==7 && LocalExposureBlurLevel({4,2,1})==0,"blur level: first at most 8 wide");
}
void MeterChecks()
{
    unsigned bins[METER_BINS]={};
    bins[MeterBin(0.5f)]=100;
    Require(Close(MeterBandAverage(bins,0.1f,0.9f),METER_LOW+METER_STEP*(float(MeterBin(0.5f))+0.5f),1e-4f),"band average of one bin");
    unsigned split[METER_BINS]={};
    split[10]=10; split[30]=80; split[50]=10; // the darkest and brightest tenth are ignored
    Require(Close(MeterBandAverage(split,0.1f,0.9f),METER_LOW+METER_STEP*30.5f,1e-4f),"band average ignores the outer tenths");
    split[10]=20;  // total 110: the band is weight 11..99, 9 of bin 10 and 79 of bin 30
    Require(Close(MeterBandAverage(split,0.1f,0.9f),METER_LOW+METER_STEP*(9*10.5f+79*30.5f)/88,1e-3f),"band average weighs partial bins");
    Require(Close(UeMeterTarget(std::log2(0.18f),1,0,1e30f),1,1e-5f) && Close(UeMeterTarget(std::log2(0.18f),2,0,0.5f),1,1e-5f) &&
            Close(UeMeterTarget(std::log2(0.18f)+3,1,0.5f,4),0.5f,1e-5f),"UE target: 18 %, the EV100 bounds, compensation after them");
    float low=0, high=0; MeterScaleBounds(0,20,1,low,high);
    Require(Close(high,1/1.2f,1e-5f) && Close(low,1/(1.2f*1048576.0f),1e-9f),"EV100 0 is UE's exposure 1/1.2");
    MeterScaleBounds(0.5f,20,1.45f,low,high);
    Require(Close(high*1.45f,1/(1.2f*std::sqrt(2.0f)),1e-5f),"the bounds are of the total exposure (--exposure times the scale)");
    // The daylight compensation (UE's Exposure Compensation Curve as a ramp, 3..6 -> +0.33): the garden's views meter EV100
    // 6.3-7.6 and get all of it, the hall (-0.5) none, so a hall on the EV100 bound keeps its exposure.
    const float picnic=std::log2(0.18f/0.006641f), hall=std::log2(0.18f/1.3537f);
    Require(Close(SceneEv100(picnic,1.45f),7.22f,0.02f) && Close(SceneEv100(hall,1.45f),-0.45f,0.02f),"scene EV100 of the garden and the hall");
    Require(Close(ExposureCurveBias(SceneEv100(picnic,1.45f),3,6,0.33f),0.33f,1e-6f) && ExposureCurveBias(SceneEv100(hall,1.45f),3,6,0.33f)==0 &&
            Close(ExposureCurveBias(4.5f,3,6,0.33f),0.165f,1e-6f),"daylight compensation ramp");
    MeterScaleBounds(0.5f,20,1.45f,low,high);
    Require(UeMeterTarget(hall,1*std::exp2(ExposureCurveBias(SceneEv100(hall,1.45f),3,6,0.33f)),low,high)==high,"the hall stays on its EV100 bound");
}
void OptionChecks()
{
    const auto d=Parse({"bench"});
    Require(d.tonemap=="aces" && !d.localExposure && d.exposureMetering=="bench" && Near(d.filmSlope,0.88f) && Near(d.filmToe,0.55f) &&
            Near(d.filmShoulder,0.26f) && d.filmBlackClip==0 && Near(d.filmWhiteClip,0.04f) && Near(d.localHighlights,0.8f) &&
            Near(d.localShadows,0.8f) && d.localDetail==1 && Near(d.localBlend,0.6f) && Near(d.exposureMinEv,0.5f) && d.exposureMaxEv==20,
            "display defaults: the old ACES look, UE's film settings");
    const auto ue=Parse({"bench","--tonemap","ue-filmic","--local-exposure","1","--local-exposure-highlights","0.7","--local-exposure-shadows","1",
                         "--local-exposure-detail","1.2","--local-exposure-blend","0.5","--exposure-metering","ue","--exposure-min-ev","-2",
                         "--exposure-max-ev","12","--film-slope","0.9","--film-toe","0.5","--film-shoulder","0.3","--film-black-clip","0.01",
                         "--film-white-clip","0.05","--exposure-curve","2,8,0.5"});
    Require(ue.tonemap=="ue-filmic" && ue.localExposure && Near(ue.localHighlights,0.7f) && ue.localShadows==1 && Near(ue.localDetail,1.2f) &&
            Near(ue.localBlend,0.5f) && ue.exposureMetering=="ue" && ue.exposureMinEv==-2 && ue.exposureMaxEv==12 && Near(ue.filmSlope,0.9f) &&
            Near(ue.filmToe,0.5f) && Near(ue.filmShoulder,0.3f) && Near(ue.filmBlackClip,0.01f) && Near(ue.filmWhiteClip,0.05f) &&
            ue.exposureCurve.x==2 && ue.exposureCurve.y==8 && ue.exposureCurve.z==0.5f && d.exposureCurve.x==3 && d.exposureCurve.y==6 &&
            Near(d.exposureCurve.z,0.33f),"UE display options parse");
    for(auto bad:std::vector<std::vector<std::string>>{{"bench","--tonemap","agx"},{"bench","--local-exposure","2"},{"bench","--film-slope","0"},
            {"bench","--film-toe","1.5"},{"bench","--local-exposure-highlights","-1"},{"bench","--local-exposure-blend","2"},
            {"bench","--exposure-metering","mean"},{"bench","--exposure-min-ev","5","--exposure-max-ev","4"},
            {"bench","--exposure-curve","6,3,0.5"},{"bench","--exposure-curve","3,6,5"}})
        Require(Rejected(bad),"bad UE display option rejected");
    Settings saved; saved.options=ue;
    const auto back=DecodeSettings(EncodeSettings(saved)).options;
    Require(back.tonemap=="ue-filmic" && back.localExposure && Near(back.localHighlights,0.7f) && back.exposureMetering=="ue" &&
            back.exposureMinEv==-2 && Near(back.filmWhiteClip,0.05f) && back.exposureCurve.y==8,"UE display settings round trip");
    auto text=EncodeSettings(Settings{}); const auto at=text.find("local-exposure\"");
    Require(at!=std::string::npos,"local-exposure written to settings");
}
}
void TonemapChecks()
{
    FilmChecks(); LocalChecks(); MeterChecks(); OptionChecks();
    std::printf("[test] tonemap / local exposure / UE metering checks passed\n");
}
