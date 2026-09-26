#include "options.h"
#include "ngx_nr_contract.h"
#include "ngx_nr_params.h"
#include <algorithm>
#include <vector>

namespace {
constexpr auto Ok=NVSDK_NGX_Result_Success;
void Require(bool condition, const char *message) { if(!condition) throw std::runtime_error(message); }
Options Parse(std::vector<std::string> args)
{
    std::vector<char *> pointers; for(auto &arg:args) pointers.push_back(arg.data());
    return ParseOptions(int(pointers.size()),pointers.data());
}
bool Rejected(std::vector<std::string> args)
{
    try { (void)Parse(std::move(args)); } catch(const std::exception &) { return true; }
    return false;
}
void OptionChecks()
{
    const auto defaults=Parse({"bench"});
    Require(defaults.host=="ngx" && defaults.coreMode==2 && defaults.coreTemporal==0 && defaults.coreWarpPath==0,"Core defaults");
    Require(!defaults.coreStateRebind && Parse({"bench","--core-state-rebind"}).coreStateRebind,
            "explicit state rebind option");
    for (int mode=0; mode<=2; ++mode) for (int temporal=0; temporal<=1; ++temporal) {
        const auto core=Parse({"bench","--host","core","--nr","native","--upscaler","sr",
            "--core-mode",std::to_string(mode),"--core-temporal",std::to_string(temporal)});
        Require(core.host=="core" && core.coreMode==mode && core.coreTemporal==temporal,"Core options");
    }
    for (int path=0; path<=2; ++path) {
        const auto core=Parse({"bench","--core-warp-path",std::to_string(path)});
        Require(core.coreWarpPath==path,"Core warp path option");
    }
    Require(defaults.nr=="off" && defaults.mvFormat=="rg16f" && defaults.nrLog==1 && defaults.nrColour=="srgb" &&
        !defaults.nrPadX && !defaults.nrBaseY,"NR defaults");
    const auto o=Parse({"bench","--nr","native","--upscaler","sr","--mv-format","rgba16f","--nr-log","2","--nr-colour","linear",
        "--nr-output-pad","64,32,32,16"});
    Require(o.nr=="native" && o.mvFormat=="rgba16f" && o.nrLog==2 && o.nrColour=="linear","NR options");
    Require(o.nrPadX==64 && o.nrPadY==32 && o.nrBaseX==32 && o.nrBaseY==16,"NR output pad with base");
    const auto pad=Parse({"bench","--nr","native","--nr-output-pad","64,32"});
    Require(pad.nrPadX==64 && pad.nrPadY==32 && pad.nrBaseX==0 && pad.nrBaseY==0,"NR output pad defaults to base 0,0");
    Require(Parse({"bench","--nr","upscale"}).nr=="upscale","NR upscale without DLSS");
    Require(Parse({"bench"}).nrList=="direct","NR list defaults to direct");
    Require(Parse({"bench","--nr","native","--nr-list","compute"}).nrList=="compute","NR on a compute list");
    Require(Parse({"bench","--switch","60"}).switchEvery==60 && Parse({"bench"}).switchEvery==0,"layout switch interval");
    const std::vector<std::vector<std::string>> invalid{
        {"bench","--host","invalid"},{"bench","--host","core"},
        {"bench","--core-mode","-1"},{"bench","--core-mode","3"},
        {"bench","--core-temporal","-1"},{"bench","--core-temporal","2"},
        {"bench","--core-warp-path","-1"},{"bench","--core-warp-path","3"},
        {"bench","--host","core","--nr","upscale"},
        {"bench","--host","core","--nr","native","--upscaler","rr"},
        {"bench","--host","core","--nr","native","--upscaler","sr","--nr-colour","linear"},
        {"bench","--nr","on"},{"bench","--nr","upscale","--upscaler","sr"},{"bench","--nr","upscale","--upscaler","rr"},
        {"bench","--mv-format","rg32f"},{"bench","--nr-log","3"},{"bench","--nr-colour","pq"},{"bench","--nr-output-pad","64"},
        {"bench","--nr-output-pad","64,32,65,0"},{"bench","--nr-output-pad","-1,0"},{"bench","--nr-output-pad","1,2,3"},
        {"bench","--nr-list","copy"},{"bench","--nr-list","compute"},{"bench","--nr","upscale","--nr-list","compute"},{"bench","--switch","-1"},{"bench","--nr-proxy-format","rgb10"},{"bench","--nr-colour","linear","--nr-proxy-format","r10g10b10a2"}};
    Require(std::all_of(invalid.begin(),invalid.end(),[](const auto &args) { return Rejected(args); }),"Invalid NR option accepted");
}
void ParameterMapChecks()
{
    NgxParameterMap p; p.Set("real",2.5f); p.Set("count",7u); p.Set("signed",-3);
    int i=0; unsigned u=0; float f=0; double x=0; void *pointer=nullptr;
    Require(p.Get("real",&i)==Ok && i==2 && p.Get("count",&f)==Ok && f==7.0f && p.Get("signed",&x)==Ok && x==-3,"Numeric conversions");
    Require(NVSDK_NGX_FAILED(p.Get("missing",&u)) && NVSDK_NGX_FAILED(p.Get("real",&pointer)),"Missing keys and pointer mismatches fail");
    p.Set("resource",static_cast<void *>(&p));
    Require(p.Get("resource",&pointer)==Ok && pointer==&p && NVSDK_NGX_FAILED(p.Get("resource",&u)),"Pointer keys answer pointer getters only");
    p.Reset(); Require(NVSDK_NGX_FAILED(p.Get("count",&u)),"Reset clears the block");
}
void ContractChecks()
{
    NgxParameterMap p; NrCreateInfo create; create.width=1920; create.height=1080;
    NrWriteCreate(p,create);
    unsigned width=0, height=0; int preset=-1, quality=-1; float ratio=0;
    Require(p.Get("DLSSNR.Width",&width)==Ok && width==1920 && p.Get("DLSSNR.Height",&height)==Ok && height==1080,"NR create size");
    Require(p.Get("DLSSNR.Hint.Render.Preset",&preset)==Ok && preset==1,"NR shipping preset");
    Require(NVSDK_NGX_FAILED(p.Get("DLSSNR.ScalingRatio",&ratio)) && NVSDK_NGX_FAILED(p.Get("PerfQualityValue",&quality)),"Native create omits scaling keys");
    NrCreateInfo upscale; upscale.width=960; upscale.height=540; upscale.perfQuality=0; upscale.scalingRatio=0.5f;
    NrWriteCreate(p,upscale);
    Require(p.Get("PerfQualityValue",&quality)==Ok && quality==0 && p.Get("DLSSNR.ScalingRatio",&ratio)==Ok && ratio==0.5f,"Upscale create keys");
    NrFrame frame; frame.colourRect={0,0,1920,1080}; frame.guideRect={0,0,960,540}; frame.outputRect={32,16,1920,1080};
    frame.mvScaleX=-480; frame.depthInverted=true; frame.reset=true;
    NrWriteEvaluate(p,frame);
    int baseX=0, baseY=0, mvWidth=0, reset=0, inverted=0, enabled=0; float scaleX=0, intensity=0;
    Require(p.Get("DLSSNR.OutputSubrectBaseX",&baseX)==Ok && baseX==32 && p.Get("DLSSNR.OutputSubrectBaseY",&baseY)==Ok && baseY==16,"Output sub-rect base");
    Require(p.Get("DLSSNR.MVecSubrectWidth",&mvWidth)==Ok && mvWidth==960,"Guide sub-rect at render size");
    Require(p.Get("DLSSNR.Reset",&reset)==Ok && reset==1 && p.Get("DLSSNR.DepthInverted",&inverted)==Ok && inverted==1,"Reset and depth flags");
    Require(p.Get("DLSSNR.Enabled",&enabled)==Ok && enabled==1 && p.Get("DLSSNR.MVecScaleX",&scaleX)==Ok && scaleX==-480,"Enable and motion scale");
    Require(p.Get("DLSSNR.Intensity",&intensity)==Ok && intensity==NrStrength,"NR strength");
}
}
void NrChecks()
{
    OptionChecks(); ParameterMapChecks(); ContractChecks();
}
