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
    changed|=Choice("Upscaler",o.upscaler,upNames,upValues,3);
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
    changed|=ImGui::SliderFloat("Bloom",&o.bloom,0,1);
    const char *toneNames[]={"ACES","Neutral"}, *toneValues[]={"aces","neutral"};
    changed|=Choice("Tone map",o.tonemap,toneNames,toneValues,2);
    changed|=ImGui::SliderFloat("Firefly clamp",&o.firefly,0.01f,10000,"%.2f",ImGuiSliderFlags_Logarithmic);
    changed|=ImGui::SliderFloat("FOV",&o.fov,10,150);
    changed|=ImGui::Checkbox("Vsync",&o.vsync);
    const char *viewNames[]={"Final","Noisy input","Accumulated","Depth","Motion vectors","Normals","Roughness","Diffuse albedo","Specular albedo"};
    const char *viewValues[]={"colour","noisy","accum","depth","motion","normal","roughness","albedo","specular"};
    changed|=Choice("View",o.view,viewNames,viewValues,9);
    return changed;
}
