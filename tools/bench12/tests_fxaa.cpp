// --fxaa: the option's rules (the filter itself is a shader, checked by the dumps, RASTER.md "FXAA").
#include "tests_raster_support.h"
using namespace raster_tests;

void FxaaChecks()
{
    Require(!Parse({"bench"}).fxaa && !Parse({"bench","--render","raster"}).fxaa,"--fxaa is off by default");
    Require(Parse({"bench","--render","raster","--fxaa"}).fxaa,"--fxaa parses with --render raster");
    Require(RejectedWith({"bench","--fxaa"},"--fxaa requires --render raster"),"--fxaa rejected without raster");
    Require(RejectedWith({"bench","--render","pathtrace","--fxaa"},"--fxaa requires --render raster"),"--fxaa rejected with pathtrace");
    // It only touches the raster colour, so it composes with every other raster option and the upscalers raster supports.
    const auto all=Parse({"bench","--render","raster","--fxaa","--csm","--ao","game","--probes","--lamp-tubes","--lamp-shadows",
                          "--wind","--hud","--upscaler","sr"});
    Require(all.fxaa && all.csm && all.ao=="game" && all.probes && all.lampTubes && all.lampShadows && all.wind && all.hud,"--fxaa composes");
    // The menu's checkbox persists like hud: a saved 1 or 0 round trips, over a different default, and never needs --render raster.
    Settings saved; saved.options.render="raster"; saved.options.fxaa=true;
    Require(DecodeSettings(EncodeSettings(saved)).options.fxaa,"settings keep fxaa on");
    Settings fxaaOn; fxaaOn.options.fxaa=true; saved.options.fxaa=false;
    Require(!DecodeSettings(EncodeSettings(saved),fxaaOn).options.fxaa,"a saved fxaa off clears fxaa");
    saved.options.fxaa=true; saved.options.render="pathtrace";
    Require(DecodeSettings(EncodeSettings(saved)).options.fxaa,"fxaa saved from a path-traced session decodes");
    Require(EncodeSettings(DecodeSettings(EncodeSettings(saved)))==EncodeSettings(saved),"fxaa settings round trip");
    Require(!DecodeSettings(EncodeSettings(Settings{})).options.fxaa,"fxaa defaults off in settings");
    {   // a saved fxaa with an explicit pathtrace start is ignored there, not rejected; the flag itself still needs raster
        char exe[]="bench", render[]="--render", mode[]="pathtrace";
        char *args[]={exe,render,mode};
        Require(CommandLineOptions(3,args,fxaaOn.options).render=="pathtrace","saved fxaa does not break a pathtrace start");
    }
    {   // an old settings file has no fxaa key: the session's default stands
        auto text=EncodeSettings(Settings{}); const auto at=text.find("    "+std::string(1,'"')+"fxaa"+std::string(1,'"'));
        Require(at!=std::string::npos,"fxaa key written");
        text.erase(at,text.find(char(10),at)-at+1);
        Require(DecodeSettings(text,fxaaOn).options.fxaa,"settings without fxaa keep the default");
    }
    std::printf("[test] fxaa checks passed\n");
}
