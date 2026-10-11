#include "ao_math.h"
#include "camera.h"
#include "camera_snap.h"
#include "hud_layout.h"
#include "lamps.h"
#include "lightmap.h"
#include "lightmap_uv.h"
#include "raster_csm.h"
#include "raster_lights.h"
#include "raster_shadow.h"
#include "settings.h"
#include "tests_raster_support.h"
#include "shaders/raster_depth.hlsli"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>
void CsmAoChecks();
namespace {
using namespace raster_tests;
// Six one-triangle nodes (all share one triangle and one TEXCOORD_1 accessor, the extras tell them apart):
//   0 static, atlas 0   1 animated, TEXCOORD_1, atlas 1   2 static, atlas 2   3 static, TEXCOORD_1 but no extras
//   4 animated, no TEXCOORD_1   5 static, no TEXCOORD_1
void WriteLightmapGlb(const std::filesystem::path &path)
{
    auto mesh=[](bool uv2, const char *extras) { return std::string(R"({"primitives":[{"attributes":{"POSITION":0)")+(uv2 ? R"(,"TEXCOORD_1":1)" : "")+"}}]"+extras+"}"; };
    std::string json=R"({"asset":{"version":"2.0"},"bufferViews":[{"byteOffset":0,"byteLength":36},{"byteOffset":36,"byteLength":24},{"byteOffset":60,"byteLength":8},{"byteOffset":68,"byteLength":24}],
        "accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"},{"bufferView":1,"componentType":5126,"count":3,"type":"VEC2"},
        {"bufferView":2,"componentType":5126,"count":2,"type":"SCALAR"},{"bufferView":3,"componentType":5126,"count":2,"type":"VEC3"}],"meshes":[)"
        +mesh(true,R"(,"extras":{"lightmapAtlas":0})")+","+mesh(true,R"(,"extras":{"lightmapAtlas":1})")+","+mesh(true,R"(,"extras":{"lightmapAtlas":2})")+","
        +mesh(true,"")+","+mesh(false,"")+","+mesh(false,"")
        +R"(],"nodes":[{"mesh":0},{"mesh":1},{"mesh":2},{"mesh":3},{"mesh":4},{"mesh":5}],
        "animations":[{"samplers":[{"input":2,"output":3}],"channels":[{"sampler":0,"target":{"node":1,"path":"translation"}},{"sampler":0,"target":{"node":4,"path":"translation"}}]}]})";
    while(json.size()%4) json+=' ';
    const float data[]={0,0,0,1,0,0,0,1,0, 0.1f,0.2f,0.3f,0.4f,0.5f,0.6f, 0,1, 0,0,0,0,1,0};
    const uint32_t header[]={0x46546c67,2,uint32_t(28+json.size()+sizeof(data)),uint32_t(json.size()),0x4e4f534a}, bin[]={sizeof(data),0x004e4942};
    std::ofstream file(path,std::ios::binary); file.write(reinterpret_cast<const char *>(header),sizeof(header)); file.write(json.data(),std::streamsize(json.size()));
    file.write(reinterpret_cast<const char *>(bin),sizeof(bin)); file.write(reinterpret_cast<const char *>(data),sizeof(data));
}
void LightmapChecks(const std::filesystem::path &directory)
{
    const auto path=directory/"test_lightmap.glb"; WriteLightmapGlb(path);
    pwgltf::Scene scene; const bool loaded=pwgltf::Load(path.string().c_str(),scene);
    std::filesystem::remove(path); Require(loaded,"Synthetic lightmap GLB load");
    std::vector<pwgltf::Vertex> raw; std::vector<Vec3> previous; pwgltf::BuildVertices(scene,0,0,raw,previous);
    Require(raw.size()==18 && raw[3].lightmapAtlas==1 && raw[6].lightmapAtlas==2 && raw[9].lightmapAtlas==0 && raw[12].lightmapAtlas==-1,"loader reads TEXCOORD_1 and extras.lightmapAtlas");
    Require(Near(raw[0].uv2[0],0.1f) && Near(raw[1].uv2[1],0.4f) && Near(raw[2].uv2[0],0.5f) && raw[12].uv2[0]==0,"loader keeps the TEXCOORD_1 values");
    // The triangle order Scene uses: dynamic nodes last, the rest in node order (one triangle per node here).
    std::vector<bool> dynamic; for(size_t node=0;node<scene.nodes.size();++node) dynamic.push_back(pwgltf::DynamicNode(scene,node));
    Require(dynamic==std::vector<bool>({false,true,false,false,true,false}),"dynamic nodes in the lightmap scene");
    std::vector<unsigned> order(6); std::iota(order.begin(),order.end(),0u);
    std::stable_partition(order.begin(),order.end(),[&](unsigned t) { return !dynamic[t]; });
    const auto lightmap=BuildLightmapUvs(raw,order,dynamic);
    // Static nodes 0, 2, 3, 5 then dynamic 1, 4. Node 5 has no TEXCOORD_1 and the dynamic node 1 has it: both are -1.
    const float atlas[6]={0,2,0,-1,-1,-1}, u[3]={0.1f,0.3f,0.5f}, v[3]={0.2f,0.4f,0.6f};
    Require(lightmap.size()==18,"one lightmap entry per vertex");
    for(size_t t=0;t<6;++t) for(size_t k=0;k<3;++k) {
        const LightmapUv &e=lightmap[t*3+k]; const bool has=t<3;
        Require(e.atlas==atlas[t] && e.padding==0 && Near(e.u,has ? u[k] : 0) && Near(e.v,has ? v[k] : 0),"lightmap entry follows the static-first vertex order");
    }
    Require(BuildLightmapUvs(std::vector<pwgltf::Vertex>(6),{0,1},{false,false}).empty(),"no TEXCOORD_1 anywhere: no lightmap buffer");
}

// --lightmap: the option, the JSON the bake writes, the sun and lamp warnings, the atlas texel format.
void LightmapFileChecks()
{
    Require(Rejected({"bench","--lightmap","x.json"}) && Rejected({"bench","--render","pathtrace","--lightmap","x.json"}),"--lightmap without --render raster accepted");
    try { (void)Parse({"bench","--lightmap","x.json"}); } catch(const std::exception &e) { Require(std::string(e.what())=="--lightmap requires --render raster","--lightmap rejection message"); }
    Require(Parse({"bench","--render","raster","--lightmap","x.json"}).lightmap=="x.json" && Parse({"bench"}).lightmap.empty(),"--lightmap with --render raster");
    const std::string valid=R"({"version":1,"atlases":[{"full":"lab_lightmap_full_0.hdr","sun":"lab_lightmap_sun_0.hdr","size":8192}],"sun":{"dir":[0.45,-0.77,0.45],"strength":1500.0,"angle":0.5},"sky":[0.12,0.16,0.23],"scaleFull":1.0,"scaleSun":1.0,"samples":512,"texelsPerMeter":95.31,"bakeSeconds":0.0})";
    const LightmapBake bake=ParseLightmapJson(valid);
    Require(bake.atlases.size()==1 && bake.atlases[0].full=="lab_lightmap_full_0.hdr" && bake.atlases[0].sun=="lab_lightmap_sun_0.hdr" && bake.atlases[0].size==8192,"lightmap JSON atlases");
    Require(Near(bake.scaleFull,1) && Near(bake.scaleSun,1) && Near(bake.sunDir.x,0.45f) && Near(bake.sunDir.y,-0.77f) && Near(bake.sunStrength,1500) && Near(bake.sunAngle,0.5f),"lightmap JSON scales and sun");
    auto edited=[&](const std::string &from, const std::string &to) { std::string s=valid; s.replace(s.find(from),from.size(),to); return s; };
    auto fails=[](const std::string &text) {
        try { (void)ParseLightmapJson(text); } catch(const std::runtime_error &e) { return std::string(e.what()).find("lightmap")!=std::string::npos; }
        return false;
    };
    Require(fails(edited("\"atlases\"","\"maps\"")),"lightmap JSON without atlases");
    Require(fails(edited("\"scaleSun\":1.0","\"scaleSun\":\"1\"")) && fails(edited("\"scaleFull\":1.0","\"scaleFull\":null")),"lightmap JSON with a non-number scale");
    Require(fails(edited("\"atlases\":[{","\"atlases\":[],\"x\":[{")) && fails(edited("\"full\":","\"fill\":")) && fails(edited("[0.45,-0.77,0.45]","[0.45,-0.77]")),"lightmap JSON atlas and sun fields");
    Require(fails(valid.substr(0,40)) && fails("") && fails(edited("\"version\":1","\"version\":2")),"malformed lightmap JSON");
    // The optional rest map (full - sun): absent above, present here, on every atlas or none.
    Require(bake.atlases[0].rest.empty(),"no rest map without \"rest\"");
    const std::string withRest=edited("\"sun\":\"lab_lightmap_sun_0.hdr\"","\"sun\":\"lab_lightmap_sun_0.hdr\",\"rest\":\"lab_lightmap_rest_0.hdr\"");
    const LightmapBake rested=ParseLightmapJson(withRest);
    Require(rested.atlases.size()==1 && rested.atlases[0].rest=="lab_lightmap_rest_0.hdr" && rested.atlases[0].full=="lab_lightmap_full_0.hdr","lightmap JSON with a rest map");
    const std::string second=R"({"full":"f1.hdr","sun":"s1.hdr","size":8192})";
    Require(ParseLightmapJson(edited("\"size\":8192}]","\"size\":8192},"+second+"]")).atlases.size()==2,"two atlases without rest maps");
    {
        std::string s=withRest; s.replace(s.find("\"size\":8192}]"),13,"\"size\":8192},"+second+"]");
        Require(fails(s),"a rest map on some atlases only");
    }
    Require(fails(edited("\"sun\":\"lab_lightmap_sun_0.hdr\"","\"sun\":\"lab_lightmap_sun_0.hdr\",\"rest\":1")),"a rest map that is not a file name");
    // The run's sun against the baked one: directions are compared normalised, within 1e-3.
    LightmapSet set; set.bake=bake;
    const Options run=Parse({"bench","--render","raster","--sun-dir","0.45,-0.77,0.45","--sun-strength","1500","--sun-angle","0.5"});
    Require(LightmapSunWarning(set,run).empty(),"the baked sun gives no warning");
    Options other=run; other.sunDir={0.9f,-1.54f,0.9f};
    Require(LightmapSunWarning(set,other).empty(),"the same sun direction at another length gives no warning");
    other.sunStrength=1000; const std::string warning=LightmapSunWarning(set,other);
    Require(warning.find("lightmap baked for sun")!=std::string::npos && warning.find("; this run uses")!=std::string::npos,"another sun strength warns");
    other=run; other.sunDir={0,-1,0}; Require(!LightmapSunWarning(set,other).empty(),"another sun direction warns");
    other=run; other.sunAngle=2; Require(!LightmapSunWarning(set,other).empty(),"another sun angle warns");
    // Lamp groups the menu changed cannot change the baked light.
    std::vector<LampGroup> lamps(2); lamps[0].name="tubes"; lamps[1].name="screens";
    Require(LightmapLampWarning(lamps).empty(),"baked lamp groups give no warning");
    lamps[1].tint={1,0.5f,1}; Require(LightmapLampWarning(lamps).find("screens")!=std::string::npos,"a tinted lamp group warns");
    lamps[1].tint={1,1,1}; lamps[0].intensity=2; Require(LightmapLampWarning(lamps).find("tubes")!=std::string::npos,"a brighter lamp group warns");
    // R9G9B9E5_SHAREDEXP: 9-bit mantissas at bits 0, 9, 18 and a shared exponent (bias 15) at bit 27.
    auto unpack=[](uint32_t texel, int channel) { return float((texel>>(9*channel))&511u)*std::ldexp(1.0f,int(texel>>27)-24); };
    const float nan=std::numeric_limits<float>::quiet_NaN(), infinity=std::numeric_limits<float>::infinity();
    Require(PackR9G9B9E5(1,1,1)==0x84020100u && PackR9G9B9E5(0,0,0)==0 && PackR9G9B9E5(-1,nan,-infinity)==0,"R9G9B9E5 of 1, 0 and negatives");
    Require(PackR9G9B9E5(infinity,0,0)==0xf80001ffu,"R9G9B9E5 clamps to its largest value");
    const uint32_t mixed=PackR9G9B9E5(0.3f,0.6f,0.9f), wide=PackR9G9B9E5(3.7f,0.02f,1200);
    for(int c=0;c<3;++c) Require(std::abs(unpack(mixed,c)-0.3f*float(c+1))<=0.9f/512,"R9G9B9E5 round trip");
    Require(unpack(wide,2)==1200 && unpack(wide,0)==4 && unpack(wide,1)==0,"R9G9B9E5 shares the largest channel's exponent");
}
}
void RasterChecks(const std::filesystem::path &directory)
{
    LightmapChecks(directory);
    LightmapFileChecks();
    CsmAoChecks();
    Options d=Parse({"bench"});
    Require(d.render=="pathtrace" && !d.hud && Near(d.snapDeg,90) && d.snapFrames==6 && d.snapHold==54,"raster option defaults");
    Require(Parse({"bench","--render","raster"}).render=="raster","--render raster");
    Require(Rejected({"bench","--render","bogus"}),"invalid --render accepted");
    Require(Rejected({"bench","--render","raster","--upscaler","rr"}),"raster + rr accepted");
    // --render-switch-at F: a frame below the frame count, scripted runs only, never with RR (one side is raster).
    Require(Parse({"bench"}).renderSwitchAt==-1,"--render-switch-at default");
    Require(Parse({"bench","120","--render","raster","--render-switch-at","60","--upscaler","sr"}).renderSwitchAt==60,"--render-switch-at");
    Require(Parse({"bench","120","--render-switch-at","0"}).renderSwitchAt==0,"--render-switch-at 0");
    Require(Rejected({"bench","120","--render-switch-at","120"}) && Rejected({"bench","120","--render-switch-at","-2"}),"--render-switch-at range");
    Require(Rejected({"bench","--render-switch-at","10","--interactive"}),"--render-switch-at with --interactive");
    Require(Rejected({"bench","120","--render-switch-at","60","--upscaler","rr"}),"--render-switch-at with RR");
    Require(!Rejected({"bench","--render","raster","--upscaler","sr"}),"raster + sr rejected");
    Require(Parse({"bench","--motion","none"}).motion=="none","--motion none");
    Require(Parse({"bench","--hud"}).hud,"--hud");
    Options s=Parse({"bench","--camera","snap","--snap-deg","60","--snap-frames","4","--snap-hold","10"});
    Require(s.camera=="snap" && Near(s.snapDeg,60) && s.snapFrames==4 && s.snapHold==10,"snap options");
    Require(Rejected({"bench","--snap-frames","0"}) && Rejected({"bench","--snap-hold","-1"}),"snap ranges");
    Require(Near(Parse({"bench","--raster-ambient","0.4"}).rasterAmbient,0.4f),"--raster-ambient");
    // Snap: turn 0 -> deg over turnFrames, hold, turn back, hold; period 2*(turn+hold).
    Require(Near(SnapYawDegrees(0,90,6,54),0),"snap frame 0");
    Require(Near(SnapYawDegrees(3,90,6,54),45),"snap halfway");
    Require(Near(SnapYawDegrees(6,90,6,54),90) && Near(SnapYawDegrees(59,90,6,54),90),"snap hold");
    Require(Near(SnapYawDegrees(63,90,6,54),45),"snap return halfway");
    Require(Near(SnapYawDegrees(66,90,6,54),0) && Near(SnapYawDegrees(120,90,6,54),0),"snap back and period");
    Options c=Parse({"bench","--camera","snap"});
    CameraState a=CameraAt(0,c,{}), b=CameraAt(6,c,{});
    Require(Near(a.eye.x,b.eye.x) && Near(a.eye.y,b.eye.y) && Near(a.eye.z,b.eye.z),"snap keeps the eye in place");
    // The default view looks slightly down, so the yaw is measured on the horizontal projection and the pitch must not change.
    Vec3 fa=Normalize({a.target.x-a.eye.x,0,a.target.z-a.eye.z}), fb=Normalize({b.target.x-b.eye.x,0,b.target.z-b.eye.z});
    Require(std::abs(Dot(fa,fb))<1e-3f,"snap turns 90 degrees in 6 frames");
    Require(Near(a.target.y-a.eye.y,b.target.y-b.eye.y),"snap keeps the pitch");
    Require(!CameraCut(CameraAt(2,c,{}),CameraAt(3,c,{})),"a snap step is not a cut");
    // The snap frame counts are bounded, and the curve is computed in 64 bits so a huge period cannot overflow int.
    Require(Rejected({"bench","--snap-frames","100001"}) && Rejected({"bench","--snap-hold","100001"}),"snap frame bounds");
    Require(!Rejected({"bench","--snap-frames","100000","--snap-hold","100000"}),"snap frame bounds are inclusive");
    Require(Near(SnapYawDegrees(5,90,1500000000,1500000000),0) && Near(SnapYawDegrees(1500000005,90,1500000000,1500000000),90),"snap 64-bit period");
    {
        const float yaw=SnapYawDegrees(2147483647,90,100000,100000);
        Require(yaw>=0 && yaw<=90,"snap at the largest frame and bounds");
    }
    // Settings carry the new options. --hud is a bare flag, so a saved 0 must clear it and must not leak into the frame count.
    Settings saved; Options &so=saved.options;
    so.render="raster"; so.rasterAmbient=0.4f; so.camera="snap"; so.snapDeg=-60; so.snapFrames=4; so.snapHold=10; so.hud=true;
    const Options loaded=DecodeSettings(EncodeSettings(saved)).options;
    Require(loaded.render=="raster" && Near(loaded.rasterAmbient,0.4f) && loaded.camera=="snap" && Near(loaded.snapDeg,-60)
        && loaded.snapFrames==4 && loaded.snapHold==10 && loaded.hud && loaded.frames==Options().frames,"settings keep the raster options");
    Require(EncodeSettings(DecodeSettings(EncodeSettings(saved)))==EncodeSettings(saved),"raster settings round trip");
    so.hud=false; Settings hudOn; hudOn.options.hud=true;
    Require(!DecodeSettings(EncodeSettings(saved),hudOn).options.hud,"a saved hud off clears hud");
    so.upscaler="rr"; bool rejected=false;
    try { (void)DecodeSettings(EncodeSettings(saved)); } catch(const std::exception &) { rejected=true; }
    Require(rejected,"settings with raster + rr accepted");
    // The interactive save never throws: RR in a raster session is coerced to SR first, other combinations are left alone.
    Require(CoerceRasterUpscaler(so) && so.upscaler=="sr" && !CoerceRasterUpscaler(so),"raster + rr coerced to sr once");
    (void)DecodeSettings(EncodeSettings(saved));
    Options traced; traced.upscaler="rr";
    Require(!CoerceRasterUpscaler(traced) && traced.upscaler=="rr","path tracing keeps RR");
    {
        std::vector<LampGroup> groups(2); groups[0].name="a"; groups[1].name="b";
        std::vector<LampSource> sources={{0,0,1.0f,{2,2,2}},{1,0,3.0f,{2,2,2}},{2,1,1.0f,{0,4,0}}};
        std::vector<Vec3> centroids={{0,0,0},{4,0,0},{0,0,9}};
        auto lights=BuildRasterLights(sources,centroids,groups);
        Require(lights.size()==2,"one light per group with emitters");
        Require(Near(lights[0].position.x,3) && Near(lights[0].power.x,8),"power-weighted centroid and summed power");
        Require(Near(lights[1].position.z,9) && Near(lights[1].power.y,4),"second group");
        groups[1].intensity=0.5f;
        Require(Near(BuildRasterLights(sources,centroids,groups)[1].power.y,2),"group intensity scales power");
        groups.insert(groups.begin(),LampGroup{});
        for(auto &source:sources) ++source.group;
        Require(BuildRasterLights(sources,centroids,groups).size()==2,"a group without emitters gives no light");
        centroids.pop_back(); bool mismatch=false;
        try { (void)BuildRasterLights(sources,centroids,groups); } catch(const std::exception &) { mismatch=true; }
        Require(mismatch,"centroids parallel to sources");
    }
    {
        // Unequal emission and a nonwhite tint, so area-only weighting, a plain mean and a missing tint all fail.
        // One group, tint (1,0.5,0.25), intensity 2, so LampFactor = (2,1,0.5).
        //   emitter 1: area 2, emission (1,0,1), centroid (0,2,-4):  luminance 0.2126+0.0722 = 0.2848, weight 2*0.2848 = 0.5696
        //   emitter 2: area 1, emission (0,2,1), centroid (10,6,8):  luminance 2*0.7152+0.0722 = 1.5026, weight 1.5026
        //   weight sum 2.0722
        //   position = (0.5696*(0,2,-4) + 1.5026*(10,6,8)) / 2.0722
        //     x = 15.026/2.0722 = 7.25123, y = (1.1392+9.0156)/2.0722 = 4.90049, z = (-2.2784+12.0208)/2.0722 = 4.70148
        //     (area-only weights would give x = 10/3 = 3.333, a plain mean x = 5)
        //   power = (2*(1,0,1) + 1*(0,2,1)) * (2,1,0.5) = (2,2,3) * (2,1,0.5) = (4,2,1.5)
        std::vector<LampGroup> groups(1); groups[0].name="warm"; groups[0].intensity=2; groups[0].tint={1,0.5f,0.25f};
        std::vector<LampSource> sources={{0,0,2.0f,{1,0,1}},{1,0,1.0f,{0,2,1}}};
        std::vector<Vec3> centroids={{0,2,-4},{10,6,8}};
        auto lights=BuildRasterLights(sources,centroids,groups);
        Require(lights.size()==1,"weighted group gives one light");
        Require(Near(lights[0].position.x,7.25123f) && Near(lights[0].position.y,4.90049f) && Near(lights[0].position.z,4.70148f),
            "luminance-weighted position");
        Require(Near(lights[0].power.x,4) && Near(lights[0].power.y,2) && Near(lights[0].power.z,1.5f),"tinted RGB power");
    }
    {
        // Groups with zero weight give no light: 0 = zero area, 1 = zero emission, 2 = a real emitter plus a zero-area
        // emitter far away. Only group 2 yields a light, at the real emitter's centroid, with the real emitter's power.
        std::vector<LampGroup> groups(3); groups[0].name="flat"; groups[1].name="dark"; groups[2].name="mixed";
        std::vector<LampSource> sources={{0,0,0.0f,{3,3,3}},{1,1,2.0f,{0,0,0}},{2,2,1.0f,{1,2,3}},{3,2,0.0f,{5,5,5}}};
        std::vector<Vec3> centroids={{9,9,9},{8,8,8},{1,2,3},{100,100,100}};
        auto lights=BuildRasterLights(sources,centroids,groups);
        Require(lights.size()==1,"zero-weight groups give no light");
        Require(Near(lights[0].position.x,1) && Near(lights[0].position.y,2) && Near(lights[0].position.z,3),
            "a zero-area emitter does not move the position");
        Require(Near(lights[0].power.x,1) && Near(lights[0].power.y,2) && Near(lights[0].power.z,3),"zero-area emitter adds no power");
    }
    // The resolve's hardware-depth inversion matches math.h's projection in both directions.
    for(bool reverse:{false,true}) {
        const Mat4 p=Perspective(60,16.0f/9,reverse);
        for(float w:{0.1f,0.5f,3.0f,40.0f,299.0f}) {
            const float z=(p.m[2][2]*w+p.m[2][3])/w, back=ViewDepthFromHardware(z,reverse);
            Require(std::abs(back-w)<=1e-3f*w,"hardware depth inverts to view depth");
        }
        Require(Near(ViewDepthFromHardware(reverse ? 1.0f : 0.0f,reverse),0.1f),"near plane");
    }
    // The sun shadow fit covers the scene bounds; depth grows along the light's travel.
    for(Vec3 sun:{Vec3{0.45f,-0.77f,0.45f},Vec3{0,-1,0},Vec3{1,-0.2f,0}}) {
        const Vec3 lo{-20,-1,-30}, hi{25,12,18};
        const SunShadow fit=FitSunShadow(sun,lo,hi,2048);
        const Mat4 &m=fit.viewProjection;
        Require(Near(m.m[3][3],1) && Near(m.m[3][0],0) && Near(m.m[3][1],0) && Near(m.m[3][2],0),"orthographic w");
        auto project=[&](Vec3 q) {
            float out[3]; for(int row=0;row<3;++row) out[row]=m.m[row][0]*q.x+m.m[row][1]*q.y+m.m[row][2]*q.z+m.m[row][3];
            return Vec3{out[0],out[1],out[2]};
        };
        for(int corner=0;corner<8;++corner) {
            const Vec3 q=project({corner&1 ? hi.x : lo.x,corner&2 ? hi.y : lo.y,corner&4 ? hi.z : lo.z});
            Require(std::abs(q.x)<1 && std::abs(q.y)<1 && q.z>0 && q.z<1,"shadow fit covers the bounds");
        }
        const Vec3 centre{2,5,-4}, nearer=project(centre), further=project(centre+Normalize(sun));
        Require(further.z>nearer.z && Near(nearer.x,further.x) && Near(nearer.y,further.y),"shadow depth grows along the sun's travel");
        Require(fit.texelWorld>0 && fit.texelWorld<0.1f && fit.depthRange>0,"shadow texel and depth range");
    }
    {
        // The HUD rects exactly: the brief's 1080p layout, x2 at 2160p, and an ultrawide frame. HudLayoutFor scales
        // with height/1080 and anchors the right-hand elements to the right edge, so at 2560x1080 the bar stays put,
        // the labels, icons and minimap move right by 640 and keep the 40 px margin of 1080p.
        auto same=[](const HudRect &r, HudRect e) { return Near(r.x,e.x) && Near(r.y,e.y) && Near(r.w,e.w) && Near(r.h,e.h); };
        auto exact=[&](const HudLayout &l, const HudRect (&e)[6]) {
            return same(l.bar,e[0]) && same(l.labels[0],e[1]) && same(l.labels[1],e[2]) && same(l.labels[2],e[3])
                && same(l.icons,e[4]) && same(l.minimap,e[5]);
        };
        const HudRect fhd[6]={{40,40,360,40},{1300,40,240,28},{1300,76,240,28},{1300,112,240,28},{1640,820,200,200},{1560,740,320,320}};
        const HudRect uhd[6]={{80,80,720,80},{2600,80,480,56},{2600,152,480,56},{2600,224,480,56},{3280,1640,400,400},{3120,1480,640,640}};
        const HudRect wide[6]={{40,40,360,40},{1940,40,240,28},{1940,76,240,28},{1940,112,240,28},{2280,820,200,200},{2200,740,320,320}};
        Require(exact(HudLayoutFor(1920,1080),fhd),"HUD rects at 1920x1080");
        Require(exact(HudLayoutFor(3840,2160),uhd),"HUD rects x2 at 3840x2160");
        const HudLayout w=HudLayoutFor(2560,1080);
        Require(exact(w,wide),"HUD rects at 2560x1080: bar left-anchored, the rest right-anchored");
        Require(Near(2560-(w.minimap.x+w.minimap.w),1920-(1560+320)),"ultrawide minimap keeps the right margin");
    }
}
