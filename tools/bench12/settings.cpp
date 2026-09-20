#include "settings.h"
#include "json.h"
#include <windows.h>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <limits>

namespace {
double Number(const Json &j, double low, double high)
{
    if(j.kind!=Json::Kind::Number || j.number<low || j.number>high) throw std::runtime_error("Settings number out of range");
    return j.number;
}
bool Boolean(const Json &j)
{
    if(j.kind!=Json::Kind::Boolean) throw std::runtime_error("Expected settings boolean");
    return j.boolean;
}
Vec3 Vector(const Json &j)
{
    if(j.kind!=Json::Kind::Array || j.array.size()!=3) throw std::runtime_error("Expected settings vector");
    return {float(Number(j.array[0],-1e8,1e8)),float(Number(j.array[1],-1e8,1e8)),float(Number(j.array[2],-1e8,1e8))};
}
CameraState Pose(const Json &j)
{
    CameraState p{Vector(j.At("eye")),Vector(j.At("target"))};
    auto right=Cross({0,1,0},p.target-p.eye);
    if(Dot(right,right)<1e-10f) throw std::runtime_error("Invalid settings camera");
    return p;
}
std::string VectorText(Vec3 v)
{
    std::ostringstream s; s<<std::setprecision(9)<<v.x<<','<<v.y<<','<<v.z; return s.str();
}
std::string PoseText(CameraState p)
{
    return "{\"eye\":["+VectorText(p.eye)+"],\"target\":["+VectorText(p.target)+"]}";
}
std::map<std::string,std::string> OptionValues(const Options &o)
{
    std::map<std::string,std::string> values;
    auto add=[&](const char *key, const auto &v) { std::ostringstream s; s<<std::setprecision(9)<<v; values[key]=s.str(); };
    add("render-scale",o.renderScale);
    add("anim-speed",o.animSpeed); add("anim-start",o.animStart); add("anim-playing",o.animPlaying);
    add("blas-rebuild",o.blasRebuild); add("follow-node",o.followNode); add("camera",o.camera);
    add("fov",o.fov);
    add("sun-strength",o.sunStrength);
    add("sun-angle",o.sunAngle);
    add("exposure",o.exposure);
    add("albedo",o.albedo);
    add("firefly",o.firefly);
    add("haze",o.haze);
    add("haze-g",o.hazeG);
    add("bloom",o.bloom);
    add("auto-exposure",o.autoExposure);
    add("spp",o.spp);
    add("bounces",o.bounces);
    add("light-candidates",o.lightCandidates);
    add("vsync",o.vsync);
    add("upscaler",o.upscaler);
    add("tonemap",o.tonemap);
    add("view",o.view);
    add("jitter",o.jitter);
    add("sun-dir",VectorText(o.sunDir));
    return values;
}
}
Options CommandLineOptions(int argc, char **argv, const Options &defaults)
{
    return ParseOptions(argc,argv,defaults);
}
std::string EncodeSettings(const Settings &s)
{
    std::ostringstream out; out<<std::setprecision(9);
    out<<"{\n  \"version\":1,\n  \"options\":{";
    bool first=true;
    for(const auto &[key,value]:OptionValues(s.options)) {
        out<<(first ? "\n" : ",\n")<<"    "<<JsonString(key)<<':'<<JsonString(value); first=false;
    }
    out<<"\n  },\n  \"accumulation\":{\"enabled\":"<<(s.options.accumulationEnabled ? "true" : "false")
       <<",\"infinite\":"<<(s.options.accumulationInfinite ? "true" : "false")
       <<",\"feed\":"<<(s.options.feedAccumulation ? "true" : "false")<<",\"frames\":"<<s.options.accumulationFrames<<"},\n";
    out<<"  \"menu\":"<<(s.menu ? "true" : "false")<<",\"overlay\":"<<(s.overlay ? "true" : "false")<<",\"speed\":"<<s.speed<<",\n";
    out<<"  \"camera\":"<<(s.hasPose ? PoseText(s.pose) : "null")<<",\n  \"bookmarks\":[";
    for(size_t i=0;i<4;++i) out<<(i ? "," : "")<<(s.bookmarks[i].valid ? PoseText(s.bookmarks[i].pose) : "null");
    out<<"],\n  \"lamps\":[";
    for(size_t i=0;i<s.lamps.size();++i) {
        const auto &g=s.lamps[i];
        out<<(i ? "," : "")<<"\n    {\"name\":"<<JsonString(g.name)<<",\"intensity\":"<<g.intensity<<",\"tint\":["<<VectorText(g.tint)<<"]}";
    }
    out<<"\n  ]\n}\n"; return out.str();
}
Settings DecodeSettings(const std::string &text, const Settings &defaults)
{
    Settings s=defaults; auto root=ReadJson(text);
    if(Number(root.At("version"),1,1)!=1) throw std::runtime_error("Unsupported settings version");
    const auto &options=root.At("options");
    if(options.kind!=Json::Kind::Object) throw std::runtime_error("Expected settings options object");
    auto allowed=OptionValues(s.options); std::vector<std::string> args{"settings"};
    for(const auto &[key,value]:options.object) {
        if(!allowed.contains(key) || value.kind!=Json::Kind::String) throw std::runtime_error("Invalid settings option: "+key);
        args.push_back("--"+key); args.push_back(value.string);
    }
    std::vector<char *> pointers; for(auto &arg:args) pointers.push_back(arg.data());
    s.options=ParseOptions(int(pointers.size()),pointers.data(),s.options);
    const auto &a=root.At("accumulation");
    s.options.accumulationEnabled=Boolean(a.At("enabled")); s.options.accumulationInfinite=Boolean(a.At("infinite"));
    s.options.feedAccumulation=Boolean(a.At("feed"));
    double frames=Number(a.At("frames"),2,4096);
    if(std::floor(frames)!=frames) throw std::runtime_error("Accumulation frames must be an integer");
    s.options.accumulationFrames=int(frames);
    s.menu=Boolean(root.At("menu")); s.overlay=Boolean(root.At("overlay")); s.speed=float(Number(root.At("speed"),0.01,1000));
    s.hasPose=root.At("camera").kind!=Json::Kind::Null;
    if(s.hasPose) s.pose=Pose(root.At("camera"));
    const auto &bookmarks=root.At("bookmarks");
    if(bookmarks.kind!=Json::Kind::Array || bookmarks.array.size()!=4) throw std::runtime_error("Expected four bookmarks");
    for(size_t i=0;i<4;++i) { s.bookmarks[i].valid=bookmarks.array[i].kind!=Json::Kind::Null; if(s.bookmarks[i].valid) s.bookmarks[i].pose=Pose(bookmarks.array[i]); }
    const auto &lamps=root.At("lamps");
    if(lamps.kind!=Json::Kind::Array) throw std::runtime_error("Expected lamp groups array");
    s.lamps.clear();
    for(const auto &item:lamps.array) {
        const auto &name=item.At("name");
        if(name.kind!=Json::Kind::String) throw std::runtime_error("Invalid lamp name");
        LampGroup g; g.name=LampName(name.string); g.intensity=float(Number(item.At("intensity"),0,20)); g.tint=Vector(item.At("tint"));
        if(g.tint.x<0 || g.tint.x>1 || g.tint.y<0 || g.tint.y>1 || g.tint.z<0 || g.tint.z>1) throw std::runtime_error("Lamp tint requires 0..1");
        for(const auto &other:s.lamps) if(other.name==g.name) throw std::runtime_error("Duplicate lamp group");
        s.lamps.push_back(g);
    }
    return s;
}
Settings LoadSettings(const std::filesystem::path &path, const Settings &defaults)
{
    std::ifstream file(path,std::ios::binary);
    if(!file) throw std::runtime_error("Cannot read settings: "+path.string());
    std::ostringstream text; text<<file.rdbuf();
    if(file.bad()) throw std::runtime_error("Settings read failed: "+path.string());
    return DecodeSettings(text.str(),defaults);
}
void SaveSettings(const std::filesystem::path &path, const Settings &settings)
{
    auto text=EncodeSettings(settings); (void)DecodeSettings(text);
    auto temporary=path; temporary+=".tmp";
    { std::ofstream file(temporary,std::ios::binary|std::ios::trunc); file<<text; file.close();
      if(!file) throw std::runtime_error("Cannot write settings: "+temporary.string()); }
    // Replace atomically so a failed write never truncates the previous settings.
    if(!MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("Cannot replace settings: "+path.string());
}
