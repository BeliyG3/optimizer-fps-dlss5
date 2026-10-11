// --lightmap without the GPU: the bake's JSON, the warnings against it, the atlas texel encoding (the CPU checks link this).
#include "lightmap.h"
#include "json.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <sstream>

namespace {
[[noreturn]] void Fail(const std::string &what) { throw std::runtime_error("lightmap JSON: "+what); }
const Json &Field(const Json &object, const char *key)
{
    if(object.kind!=Json::Kind::Object || !object.object.contains(key)) Fail(std::string("missing \"")+key+"\"");
    return object.object.at(key);
}
float Number(const Json &value, const char *key)
{
    if(value.kind!=Json::Kind::Number || !std::isfinite(float(value.number))) Fail(std::string("\"")+key+"\" is not a number");
    return float(value.number);
}
float Number(const Json &object, const char *key, float low, float high)
{
    const float n=Number(Field(object,key),key);
    if(n<low || n>high) Fail(std::string("\"")+key+"\" is out of range");
    return n;
}
std::string Text(const Json &object, const char *key)
{
    const Json &value=Field(object,key);
    if(value.kind!=Json::Kind::String || value.string.empty()) Fail(std::string("\"")+key+"\" is not a file name");
    return value.string;
}
bool Same(float a, float b) { return std::abs(a-b)<=1e-3f*std::max(1.0f,std::max(std::abs(a),std::abs(b))); }
std::string SunText(Vec3 dir, float strength, float angle)
{
    std::ostringstream s; s<<std::setprecision(4)<<"dir "<<dir.x<<','<<dir.y<<','<<dir.z<<" strength "<<strength<<" angle "<<angle;
    return s.str();
}
}

LightmapBake ParseLightmapJson(const std::string &text)
{
    Json root;
    try { root=ReadJson(text); } catch(const std::exception &error) { Fail(std::string("malformed (")+error.what()+")"); }
    (void)Number(root,"version",1,1); // the only version so far
    LightmapBake bake;
    const Json &atlases=Field(root,"atlases");
    if(atlases.kind!=Json::Kind::Array || atlases.array.empty()) Fail("\"atlases\" is not a non-empty array");
    for(const auto &atlas:atlases.array) {
        const float size=Number(atlas,"size",1,16384);
        if(std::floor(size)!=size) Fail("\"size\" is not a whole number");
        const bool rest=atlas.object.contains("rest"), lamp=atlas.object.contains("lamp"); // an object: Number found "size" in it
        bake.atlases.push_back({Text(atlas,"full"),Text(atlas,"sun"),rest ? Text(atlas,"rest") : std::string(),
            lamp ? Text(atlas,"lamp") : std::string(),unsigned(size)});
        if(rest!=!bake.atlases.front().rest.empty()) Fail("\"rest\" on some atlases only");
        if(lamp!=!bake.atlases.front().lamp.empty()) Fail("\"lamp\" on some atlases only");
        if(lamp && !rest) Fail("\"lamp\" without \"rest\"");
    }
    const Json &sun=Field(root,"sun"), &dir=Field(sun,"dir");
    if(dir.kind!=Json::Kind::Array || dir.array.size()!=3) Fail("\"dir\" is not x,y,z");
    bake.sunDir={Number(dir.array[0],"dir"),Number(dir.array[1],"dir"),Number(dir.array[2],"dir")};
    if(Dot(bake.sunDir,bake.sunDir)<1e-12f) Fail("\"dir\" is zero");
    bake.sunStrength=Number(sun,"strength",0,1e9f); bake.sunAngle=Number(sun,"angle",0,180);
    bake.scaleFull=Number(root,"scaleFull",0,1e9f); bake.scaleSun=Number(root,"scaleSun",0,1e9f);
    return bake;
}
std::string LightmapSunWarning(const LightmapSet &lightmap, const Options &o)
{
    const LightmapBake &b=lightmap.bake;
    const Vec3 baked=Normalize(b.sunDir), run=Normalize(o.sunDir);
    if(Same(baked.x,run.x) && Same(baked.y,run.y) && Same(baked.z,run.z) && Same(b.sunStrength,o.sunStrength) && Same(b.sunAngle,o.sunAngle)) return {};
    return "lightmap baked for sun "+SunText(b.sunDir,b.sunStrength,b.sunAngle)+"; this run uses "+SunText(o.sunDir,o.sunStrength,o.sunAngle)
        +" (static surfaces keep the baked sun, characters get this run's)";
}
std::string LightmapLampWarning(const std::vector<LampGroup> &groups)
{
    std::string changed;
    for(const auto &g:groups)
        if(g.intensity!=1 || g.tint.x!=1 || g.tint.y!=1 || g.tint.z!=1) changed+=(changed.empty() ? "" : ", ")+g.name;
    if(changed.empty()) return {};
    return "lamp groups changed ("+changed+"): the lightmap keeps the baked lamp light; only emission and characters follow";
}
uint32_t PackR9G9B9E5(float r, float g, float b)
{
    constexpr float MaxValue=float(0x1ff<<7), MinValue=1.0f/float(1<<16);
    auto clamp=[](float v) { return v>=0 ? std::min(v,MaxValue) : 0.0f; }; // NaN fails v>=0
    const float x=clamp(r), y=clamp(g), z=clamp(b), largest=std::max(std::max(std::max(x,y),z),MinValue);
    uint32_t bits; std::memcpy(&bits,&largest,4);
    bits+=0x00004000; // round up, keeping 9 mantissa bits with the implicit 1
    const uint32_t exponent=bits>>23, scaleBits=0x83000000u-(exponent<<23);
    float scale; std::memcpy(&scale,&scaleBits,4);
    auto mantissa=[&](float v) { return uint32_t(std::nearbyint(v*scale)); };
    return mantissa(x)|mantissa(y)<<9|mantissa(z)<<18|(exponent-0x6f)<<27;
}
