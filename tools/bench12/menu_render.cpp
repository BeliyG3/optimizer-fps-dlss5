#include "menu.h"
#include "camera_angles.h"
#include "external/imgui/imgui.h"

namespace {
bool Choice(const char *label,std::string &value,const char *const *names,const char *const *values,int count)
{
    int current=0; for(int i=0;i<count;++i) if(value==values[i]) current=i;
    if(!ImGui::Combo(label,&current,names,count)) return false;
    value=values[current]; return true;
}
}
bool Menu::RenderControls(Options &o)
{
    bool changed=false;
    const char *upNames[]={"None","DLSS SR","DLSS RR"}, *upValues[]={"none","sr","rr"};
    changed|=CoerceRasterUpscaler(o); // keeps the combo truthful: RR is not offered for raster
    changed|=Choice("Upscaler",o.upscaler,upNames,upValues,o.render=="raster" ? 2 : 3);
    const float scales[]={0.5f,0.58f,2.0f/3,1};
    int quality=0; for(int i=1;i<4;++i) if(std::abs(o.renderScale-scales[i])<std::abs(o.renderScale-scales[quality])) quality=i;
    if(ImGui::Combo("DLSS mode",&quality,"Performance\0Balanced\0Quality\0DLAA\0")) { o.renderScale=scales[quality]; changed=true; }
    changed|=ImGui::SliderInt("Paths / pixel",&o.spp,1,16);
    changed|=ImGui::SliderInt("Bounces",&o.bounces,0,8);
    changed|=ImGui::SliderInt("Light candidates",&o.lightCandidates,1,32);
    changed|=ImGui::Checkbox("Accumulation",&o.accumulationEnabled);
    changed|=ImGui::SliderInt("Frames",&o.accumulationFrames,2,4096);
    changed|=ImGui::Checkbox("Infinite",&o.accumulationInfinite);
    changed|=ImGui::Checkbox("Feed the upscaler the accumulated image",&o.feedAccumulation);
    changed|=ImGui::SliderFloat("Sun strength",&o.sunStrength,0,10000,"%.3f",ImGuiSliderFlags_Logarithmic);
    float azimuth=0,elevation=0; DirectionAngles(o.sunDir,azimuth,elevation);
    bool direction=ImGui::SliderFloat("Sun azimuth",&azimuth,-180,180);
    direction|=ImGui::SliderFloat("Sun elevation",&elevation,-90,90);
    if(direction) { o.sunDir=DirectionFromAngles(azimuth,elevation); changed=true; }
    changed|=ImGui::SliderFloat("Sun angular size",&o.sunAngle,0,20);
    changed|=ImGui::SliderFloat("Haze density",&o.haze,0,0.2f,"%.4f");
    changed|=ImGui::SliderFloat("Haze anisotropy",&o.hazeG,-0.99f,0.99f);
    changed|=ImGui::SliderFloat("Exposure",&o.exposure,0.001f,20,"%.4f",ImGuiSliderFlags_Logarithmic);
    changed|=ImGui::Checkbox("Auto exposure",&o.autoExposure);
    if(o.autoExposure) { // stops' closing rate per second; brighter: the scale falls (walking outdoors), darker: it rises
        changed|=ImGui::SliderFloat("Adapt to brighter",&o.exposureSpeedUp,0.1f,20,"%.2f /s",ImGuiSliderFlags_Logarithmic);
        changed|=ImGui::SliderFloat("Adapt to darker",&o.exposureSpeedDown,0.1f,20,"%.2f /s",ImGuiSliderFlags_Logarithmic);
        changed|=ImGui::SliderFloat("Exposure compensation",&o.exposureEv,-4,4,"%+.2f EV");
        changed|=ImGui::SliderFloat("Centre weighting",&o.exposureCentre,0,1);
        const char *meterNames[]={"Bench (highlight cap)","UE histogram 10-90 %"}, *meterValues[]={"bench","ue"};
        changed|=Choice("Metering",o.exposureMetering,meterNames,meterValues,2);
        if(o.exposureMetering=="ue") {
            changed|=ImGui::SliderFloat("Min EV100",&o.exposureMinEv,-16,o.exposureMaxEv,"%.2f");
            changed|=ImGui::SliderFloat("Max EV100",&o.exposureMaxEv,o.exposureMinEv,32,"%.2f");
            // UE's Exposure Compensation Curve as a ramp: none up to the low scene EV100, the full compensation from the high one
            changed|=ImGui::SliderFloat("Daylight compensation",&o.exposureCurve.z,-2,2,"%+.2f EV");
            changed|=ImGui::SliderFloat("from scene EV100",&o.exposureCurve.x,-16,o.exposureCurve.y-0.1f,"%.1f");
            changed|=ImGui::SliderFloat("full at scene EV100",&o.exposureCurve.y,o.exposureCurve.x+0.1f,32,"%.1f");
        }
    }
    changed|=ImGui::Checkbox("Local exposure",&o.localExposure);
    if(o.localExposure) { // UE5's: the local base pulled towards mid grey, the detail kept
        changed|=ImGui::SliderFloat("Highlight contrast",&o.localHighlights,0,2);
        changed|=ImGui::SliderFloat("Shadow contrast",&o.localShadows,0,2);
        changed|=ImGui::SliderFloat("Detail strength",&o.localDetail,0,4);
        changed|=ImGui::SliderFloat("Blurred luminance blend",&o.localBlend,0,1);
    }
    changed|=ImGui::SliderFloat("Bloom",&o.bloom,0,1);
    const char *toneNames[]={"ACES (per channel)","Neutral","None","UE filmic"}, *toneValues[]={"aces","neutral","none","ue-filmic"};
    changed|=Choice("Tone map",o.tonemap,toneNames,toneValues,4);
    if(o.tonemap=="ue-filmic") { // UE's Post Process "Film" settings
        changed|=ImGui::SliderFloat("Film slope",&o.filmSlope,0.01f,2);
        changed|=ImGui::SliderFloat("Film toe",&o.filmToe,0,1);
        changed|=ImGui::SliderFloat("Film shoulder",&o.filmShoulder,0,1);
        changed|=ImGui::SliderFloat("Film black clip",&o.filmBlackClip,0,1);
        changed|=ImGui::SliderFloat("Film white clip",&o.filmWhiteClip,0,1);
    }
    changed|=ImGui::SliderFloat("Firefly clamp",&o.firefly,0.01f,10000,"%.2f",ImGuiSliderFlags_Logarithmic);
    changed|=ImGui::SliderFloat("FOV",&o.fov,10,150);
    if(o.render=="raster") changed|=ImGui::Checkbox("FXAA",&o.fxaa); // the raster resolve's filter; path tracing has no such pass
    if(o.render=="raster") { // the sky's image-based specular (RASTER_REFLECTIONS.md); path tracing reflects the sky by itself
        bool reflections=o.SkyReflectionsOn();
        if(ImGui::Checkbox("Sky reflections",&reflections)) { o.skyReflections=reflections ? 1 : 0; changed=true; }
    }
    changed|=ImGui::Checkbox("Vsync",&o.vsync);
    const char *viewNames[]={"Final","Noisy input","Accumulated","Depth","Motion vectors","Normals","Roughness","Diffuse albedo","Specular albedo","Foliage class (raster)"};
    const char *viewValues[]={"colour","noisy","accum","depth","motion","normal","roughness","albedo","specular","vegmask"};
    changed|=Choice("View",o.view,viewNames,viewValues,10);
    return changed;
}
