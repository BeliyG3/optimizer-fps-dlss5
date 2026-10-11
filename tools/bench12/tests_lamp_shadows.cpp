#include "tests_raster_support.h"
#include "lightmap.h"
#include "lightmap_uv.h"
#include "raster_lamp_shadows.h"
#include "raster_lights.h"
#include <cmath>
#include <vector>
using namespace raster_tests;
namespace {
constexpr float Pi=3.14159265f;
// A flat emissive box (two triangles per face is not needed here: the fit reads corners and areas) centred at `centre`.
void AddLamp(std::vector<LampSource> &sources, std::vector<LampTriangle> &triangles, unsigned group, Vec3 centre, Vec3 size)
{
    const Vec3 h=size*0.5f;
    const Vec3 a=centre+Vec3{-h.x,-h.y,-h.z}, b=centre+Vec3{h.x,-h.y,-h.z}, c=centre+Vec3{h.x,-h.y,h.z}, d=centre+Vec3{-h.x,-h.y,h.z};
    const Vec3 top=Vec3{0,size.y,0};
    for(const LampTriangle t:{LampTriangle{a,b,c},LampTriangle{a,c,d},LampTriangle{a+top,c+top,b+top}}) {
        const float area=0.5f*Length(Cross(t[1]-t[0],t[2]-t[0]));
        sources.push_back({unsigned(sources.size()),group,area,{1,1,1}}); triangles.push_back(t);
    }
}
// The point in the map's texture space: (u, v) in [0, 1] inside the frustum, depth 0 at the near plane and 1 at the far one.
Vec3 MapCoordinate(const LampShadowFrustum &f, Vec3 p)
{
    const Vec4 clip=Mul(f.viewProjection,Vec4{p.x,p.y,p.z,1});
    return {clip.x/clip.w*0.5f+0.5f,-clip.y/clip.w*0.5f+0.5f,clip.z/clip.w};
}
bool InsideMap(const LampShadowFrustum &f, Vec3 p)
{
    const Vec3 s=MapCoordinate(f,p);
    return s.x>=0 && s.x<=1 && s.y>=0 && s.y<=1 && s.z>=0 && s.z<=1;
}
}
// --lamp-shadows: the spot frusta, the light slices, the baked lamp visibility, the JSON's lamp map, the option, the list
// of triangles without baked light.
void LampShadowChecks()
{
    const float floorY=0;
    {   // A field of three ceiling tubes 4 m up over 10 x 6 m: one map, 90..120 degrees, the lit area on the floor inside it,
        // the emitters in front of the near plane (they never cast), the floor in front of the far plane.
        std::vector<LampSource> sources; std::vector<LampTriangle> triangles;
        for(float x:{-4.0f,0.0f,4.0f}) AddLamp(sources,triangles,0,{x,4,0},{2,0.06f,6});
        const auto frusta=FitLampShadows(sources,triangles,1,floorY);
        Require(frusta.size()==1 && frusta[0].group==0,"field: one map for the group");
        const auto &f=frusta[0];
        Require(f.fov>=0.5f*Pi-1e-4f && f.fov<=2*Pi/3+1e-4f,"field: the field of view is 90..120 degrees");
        const float e=0.5f*std::sqrt(10.0f*10.0f+6.0f*6.0f);
        Require(std::abs(f.centre.y-4)<0.05f && Near(f.litRadius,e+std::sqrt(3.0f)*(f.centre.y-floorY)) && Near(f.apex.y,f.centre.y+e),
                "field: lit radius e + sqrt(3) h, apex e above the tubes");
        bool covered=true;
        for(int k=0;k<16;++k) {
            const float a=2*Pi*float(k)/16, r=0.99f*f.litRadius;
            covered=covered && InsideMap(f,{f.apex.x+r*std::cos(a),floorY+0.01f,f.apex.z+r*std::sin(a)});
        }
        Require(covered,"field: the lit area's rim on the floor is inside the map");
        Require(InsideMap(f,{0,floorY+0.01f,0}) && InsideMap(f,{3,1.7f,-2}),"field: the floor and a character's head are inside");
        Require(MapCoordinate(f,{0,4-0.03f,0}).z<0 && MapCoordinate(f,{4,4.03f,2}).z<0,"field: the emitters are in front of the near plane");
        Require(Near(f.texelScale,2*std::tan(0.5f*f.fov)/LampShadowResolution),"field: texel scale");
        // Depth grows downward: a caster above a receiver has the smaller depth.
        Require(MapCoordinate(f,{1,1.8f,1}).z<MapCoordinate(f,{1,0.01f,1}).z,"field: depth grows away from the apex");
    }
    {   // A lone lamp 7.8 m up (a yard pole's head): the apex at the lamp, 120 degrees, the lit radius sqrt(3) h.
        std::vector<LampSource> sources; std::vector<LampTriangle> triangles;
        AddLamp(sources,triangles,1,{10,7.8f,-3},{0.6f,0.15f,0.4f});
        const auto frusta=FitLampShadows(sources,triangles,3,floorY);
        Require(frusta.size()==1 && frusta[0].group==1,"pole: one map, for group 1 (groups 0 and 2 have no emitters)");
        const auto &f=frusta[0];
        Require(std::abs(f.apex.x-10)<1e-4f && std::abs(f.apex.z+3)<1e-4f && f.apex.y-7.8f<0.5f,"pole: the apex just above the lamp");
        Require(f.fov>2*Pi/3-0.05f,"pole: about 120 degrees");
        Require(InsideMap(f,{10+0.95f*f.litRadius,floorY+0.01f,-3}),"pole: the lit radius on the ground is inside");
        const auto slices=LampShadowSlices(frusta,3);
        Require(slices.size()==3 && slices[0]==NoLampShadow && slices[1]==0 && slices[2]==NoLampShadow,"pole: the slice of each group");
    }
    {   // At most MaxLampShadows maps, in group order.
        std::vector<LampSource> sources; std::vector<LampTriangle> triangles;
        for(unsigned g=0;g<MaxLampShadows+3;++g) AddLamp(sources,triangles,g,{float(g),3,0},{0.5f,0.05f,0.5f});
        const auto frusta=FitLampShadows(sources,triangles,MaxLampShadows+3,floorY);
        Require(frusta.size()==MaxLampShadows && frusta.back().group==MaxLampShadows-1,"at most MaxLampShadows maps, the first groups");
    }
    {   // The baked lamp visibility: 1 with no casters (every test 1), with no group, or with no power; else the weighted mean.
        std::vector<LampSource> sources; std::vector<LampTriangle> triangles;
        AddLamp(sources,triangles,0,{-5,4,0},{1,0.05f,1}); AddLamp(sources,triangles,1,{5,4,0},{1,0.05f,1});
        const auto frusta=FitLampShadows(sources,triangles,2,floorY);
        const std::vector<float> power{1,1};
        Require(Near(LampVisibility(frusta,power,{1,1},{0,0,0}),1) && Near(LampVisibility(frusta,power,{1,1},{-4,0,1}),1),"no casters: lamp visibility 1");
        Require(Near(LampVisibility({},{},{},{0,0,0}),1) && Near(LampVisibility(frusta,{0,0},{0,0},{0,0,0}),1),"no group or no power: 1");
        const Vec3 halfway=(frusta[0].centre+frusta[1].centre)*0.5f-Vec3{0,4,0};
        Require(Near(LampVisibility(frusta,power,{0,1},halfway),0.5f),"halfway between two equal groups, one shadowed: 0.5");
        Require(LampVisibility(frusta,power,{0,1},{-5,0,0})<0.3f && LampVisibility(frusta,power,{0,1},{5,0,0})>0.7f,"the nearer group weighs more");
        Require(LampVisibility(frusta,{3,1},{0,1},halfway)<0.3f,"the stronger group weighs more");
    }
    {   // The raster lights' groups (the slices the shader reads per light): groups without emitters make no light.
        std::vector<LampSource> sources; std::vector<LampTriangle> triangles;
        AddLamp(sources,triangles,2,{0,3,0},{1,0.05f,1}); AddLamp(sources,triangles,0,{4,3,0},{1,0.05f,1});
        std::vector<Vec3> centroids; for(const auto &t:triangles) centroids.push_back((t[0]+t[1]+t[2])*(1.0f/3));
        std::vector<LampGroup> groups(3); std::vector<unsigned> lightGroups;
        const auto lights=BuildRasterLights(sources,centroids,groups,&lightGroups);
        Require(lights.size()==2 && lightGroups==std::vector<unsigned>({0,2}),"raster lights report their groups in order");
    }
    {   // The triangles without baked light: static ones without lightmap UVs and dynamic ones, in the order of `vertices`.
        std::vector<LightmapUv> uvs;
        for(float atlas:{0.0f,-1.0f,0.0f,-1.0f}) for(int k=0;k<3;++k) uvs.push_back({0.5f,0.5f,atlas});
        Require(UnbakedTriangles(uvs)==std::vector<unsigned>({1,3}),"the unbaked triangle list");
        Require(UnbakedTriangles({}).empty(),"no lightmap: no list");
    }
    {   // The JSON's lamp map: optional, with "rest" only, on every atlas or none.
        const std::string base=R"({"version":1,"atlases":[{"full":"f.hdr","sun":"s.hdr","rest":"r.hdr","lamp":"l.hdr","size":8192}],"sun":{"dir":[0.45,-0.77,0.45],"strength":1500.0,"angle":0.5},"scaleFull":1.3,"scaleSun":1.0})";
        auto edited=[&](const std::string &from, const std::string &to) { std::string s=base; s.replace(s.find(from),from.size(),to); return s; };
        auto fails=[](const std::string &text) {
            try { (void)ParseLightmapJson(text); } catch(const std::runtime_error &e) { return std::string(e.what()).find("lamp")!=std::string::npos; }
            return false;
        };
        const LightmapBake bake=ParseLightmapJson(base);
        Require(bake.atlases.size()==1 && bake.atlases[0].lamp=="l.hdr" && bake.atlases[0].rest=="r.hdr","lightmap JSON with a lamp map");
        Require(ParseLightmapJson(edited(",\"lamp\":\"l.hdr\"","")).atlases[0].lamp.empty(),"lightmap JSON without a lamp map, as before");
        Require(fails(edited(",\"rest\":\"r.hdr\"","")),"a lamp map without a rest map");
        Require(fails(edited("\"size\":8192}]","\"size\":8192},{\"full\":\"f1.hdr\",\"sun\":\"s1.hdr\",\"rest\":\"r1.hdr\",\"size\":8192}]")),"a lamp map on some atlases only");
        Require(fails(edited("\"lamp\":\"l.hdr\"","\"lamp\":2")),"a lamp map that is not a file name");
    }
    Require(Parse({"bench","--render","raster","--lamp-shadows"}).lampShadows && !Parse({"bench"}).lampShadows,"--lamp-shadows parses with --render raster");
    Require(RejectedWith({"bench","--lamp-shadows"},"--lamp-shadows requires --render raster"),"--lamp-shadows rejected without raster");
    std::printf("[test] lamp shadow checks passed\n");
}
