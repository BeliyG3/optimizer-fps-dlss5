#include "settings.h"
#include "json.h"
#include "camera_angles.h"
#include <stdexcept>
#include <fstream>

namespace {
void Require(bool value,const char *message) { if(!value) throw std::runtime_error(message); }
bool Near(float a,float b) { return std::abs(a-b)<0.0001f; }
template<class Action> void Reject(Action action)
{
    bool rejected=false; try { action(); } catch(const std::exception &) { rejected=true; }
    Require(rejected,"Invalid interactive settings accepted");
}
}
void InteractiveChecks(const std::filesystem::path &directory)
{
    Settings s; s.options.sunStrength=1500; s.options.sunDir={0.45f,-0.77f,0.45f};
    s.options.upscaler="rr"; s.options.view="roughness"; s.options.feedAccumulation=true;
    s.options.animSpeed=-0.5f; s.options.animStart=1.25f; s.options.animPlaying=false;
    s.options.camera="follow"; s.options.followNode="hero"; s.options.blasRebuild=30;
    s.options.accumulationEnabled=true; s.options.accumulationFrames=4096;
    s.options.accumulationInfinite=true; s.options.vsync=true; s.speed=9.25f;
    s.hasPose=true; s.pose={{1,2,3},{4,2,6}}; s.bookmarks[2]={true,{{7,8,9},{10,11,12}}};
    s.menu=false; s.overlay=false;
    s.lamps.push_back({"lab_\"screen\\name\n",2.25f,{0.25f,0.5f,0.75f}});
    auto text=EncodeSettings(s); auto round=DecodeSettings(text);
    Require(EncodeSettings(round)==text,"Complete settings round trip");
    auto path=directory/"test_settings.json"; SaveSettings(path,s);
    Require(EncodeSettings(LoadSettings(path))==text,"Settings disk round trip");
    s.options.exposure=0.22f; SaveSettings(path,s);
    Require(Near(LoadSettings(path).options.exposure,0.22f),"Atomic settings replacement");
    std::filesystem::remove(path);
    char exe[]="bench", strength[]="--sun-strength", value[]="42", interactive[]="--interactive", vsync[]="--vsync", off[]="0";
    char *args[]={exe,interactive,strength,value,vsync,off};
    auto options=CommandLineOptions(6,args,round.options);
    Require(options.sunStrength==42 && options.upscaler=="rr" && !options.vsync && options.interactive,"Explicit CLI wins, other settings preserved");
    auto invalid=text; auto at=invalid.find("\"frames\":4096"); invalid.replace(at,13,"\"frames\":1");
    Reject([&] { (void)DecodeSettings(invalid); });
    for(const std::string bad:{"{\"x\":1,\"x\":2}","{\"x\":01}","[1,]","true false","\"\\uD800\"","1e9999","{\"x\":NaN}"})
        Reject([&] { (void)ReadJson(bad); });
    Require(ReadJson("\"\\u0061\\uD83D\\uDE00\"").string=="a\xf0\x9f\x98\x80","JSON surrogate pairs");
    Require(LampName("lab_tube.001")=="lab_tube" && LampName("lab_ring.999")=="lab_ring","Numeric lamp suffix");
    Require(LampName("lab.01")=="lab.01" && LampName("lab.foo")=="lab.foo" && LampName("lab.001.extra")=="lab.001.extra","Strip only trailing three digits");
    std::vector<LampGroup> groups;
    unsigned a=FindLampGroup(groups,"lab_tube"), b=FindLampGroup(groups,"lab_tube.001"), c=FindLampGroup(groups,"lab_ring.002");
    Require(a==b && c!=a && groups.size()==2,"Material name lamp grouping");
    std::vector<LampSource> sources{{4,a,2,{1,1,1}},{8,b,1,{2,2,2}},{9,c,4,{1,1,1}}};
    std::vector<EmissiveTriangle> lights;
    Require(Near(RebuildLampCdf(sources,groups,lights),8) && Near(lights[1].cdf,0.5f),"Initial power CDF");
    Require(groups[a].triangles==2 && Near(groups[a].power,4),"Group triangle counts and power");
    groups[a].intensity=2; groups[a].tint={1,0,0}; groups[c].intensity=0;
    Require(Near(RebuildLampCdf(sources,groups,lights),8*0.2126f),"Tint and intensity affect CDF");
    Require(lights.size()==2 && Near(lights[0].cdf,0.5f) && lights.back().cdf==1,"Disabled lamps excluded from CDF");
    groups[a].intensity=0;
    Require(RebuildLampCdf(sources,groups,lights)==0 && lights.empty(),"All-off lamp sampling");
    groups[c].intensity=1;
    Require(RebuildLampCdf(sources,groups,lights)>0 && lights.size()==1 && lights[0].triangle==9,"Re-enable after all-off");
    for(float azimuth:{-179.0f,-90.0f,0.0f,45.0f,179.0f}) for(float elevation:{-89.0f,-45.0f,0.0f,60.0f,89.0f}) {
        auto direction=DirectionFromAngles(azimuth,elevation); float a2=0,e2=0; DirectionAngles(direction,a2,e2);
        Require(std::abs(a2-azimuth)<0.001f && std::abs(e2-elevation)<0.001f,"Sun angle round trip");
        Require(Near(Dot(direction,direction),1),"Sun direction unit length");
    }
    float azimuth=0,elevation=0; DirectionAngles({0,-1,0},azimuth,elevation);
    Require(Near(elevation,-90),"Vertical sun direction");
}
