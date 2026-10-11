// Foliage CPU checks: the class (foliage_class.h) by node and material name, and the class of every triangle
// on both of the scene paths, flattened (--wind) and instanced (scene_instances.h); --motion-span and --dump-raw options.
#include "exposure_track.h"
#include "foliage_class.h"
#include "scene_instances.h"
#include "tests_raster_support.h"
#include "wind.h"
#include <cstdio>
namespace {
using namespace raster_tests;
void ClassifierChecks()
{
    Require(VegClassOf("veg_grass_03","grass_atlas")==VegClass::Grass,"grass by node name");
    Require(VegClassOf("veg_fern_07","fern_02_leaves")==VegClass::Fern,"fern by node name");
    Require(VegClassOf("veg_tree_1","island_tree_02_leaves")==VegClass::Leaves,"tree leaves");
    Require(VegClassOf("veg_tree_1","island_tree_02_bark")==VegClass::Trunk,"bark material is trunk");
    Require(VegClassOf("veg_tree_1","island_tree_02")==VegClass::Trunk,"bare island_tree_<n> material is trunk");
    Require(VegClassOf("prop_bench_0","bench_wood")==VegClass::None,"props are not foliage");
    Require(VegClassOf("VEG_Grass_1","x")==VegClass::None,"prefix is case-sensitive like IsWindNode");
    Require(WithVegClass(0xffffffffu,VegClass::Fern)==((0xffffffffu&~VegClassMask)|(3u<<16)),"WithVegClass keeps the other flag bits");
}
void AddQuad(pwgltf::Primitive &p, float x0, float x1)
{
    const uint32_t base=uint32_t(p.positions.size());
    p.positions.insert(p.positions.end(),{{x0,0,0},{x1,0,0},{x1,1,0},{x0,1,0}});
    p.normals.insert(p.normals.end(),4,Vec3{0,0,1});
    p.indices.insert(p.indices.end(),{base,base+1,base+2,base,base+2,base+3});
}
// Nodes 0 veg_grass_0 and 1 veg_grass_1 share mesh 0 (instanced without --wind), node 2 prop_x has mesh 1 (single use: flattened).
pwgltf::Scene Patch()
{
    pwgltf::Scene s; s.meshes.resize(2);
    pwgltf::Primitive grass; grass.materialName="grass_atlas"; AddQuad(grass,0,1);
    pwgltf::Primitive wood; wood.materialName="bench_wood"; AddQuad(wood,0,2);
    s.meshes[0].primitives={grass}; s.meshes[1].primitives={wood};
    const char *names[]={"veg_grass_0","veg_grass_1","prop_x"}; const int meshes[]={0,0,1};
    s.nodes.resize(3);
    for(size_t i=0;i<3;++i) { s.nodes[i].name=names[i]; s.nodes[i].mesh=meshes[i]; s.roots.push_back(int(i)); }
    s.nodes[1].translation={3,0,0};
    return s;
}
uint32_t ClassBits(VegClass c) { return (WithVegClass(1u|8u,c)&VegClassMask)>>VegClassShift; } // as scene.cpp sets the flags
// The flattened triangles in node order, then the instanced ones: veg_grass_* triangles carry class 1, prop_x class 0.
void ScenePathChecks(bool wind)
{
    auto s=Patch();
    if(wind) Require(MarkWindNodes(s)==2,"--wind marks the two veg_ nodes");
    const auto plan=PlanInstances(s,true);
    Require(plan.groups.size()==(wind ? 0u : 1u),wind ? "--wind: the veg_ nodes are dynamic, flattened" : "without --wind the grass is instanced");
    const auto flat=CollectFlatTriangles(s,plan);
    const auto instanced=BuildInstancedGeometry(s,plan,unsigned(flat.vegClass.size()));
    Require(flat.vegClass.size()==flat.roughness.size() && instanced.vegClass.size()==instanced.roughness.size(),"a class per triangle");
    std::vector<VegClass> classes=flat.vegClass; classes.insert(classes.end(),instanced.vegClass.begin(),instanced.vegClass.end());
    Require(classes.size()==(wind ? 6u : 4u),"4 grass triangles (2 per node, 1 stored mesh if instanced) and 2 prop triangles");
    const size_t propFirst=wind ? 4 : 0; // flattened node order: veg_grass_0, veg_grass_1, prop_x; instanced after
    for(size_t t=0;t<classes.size();++t) {
        const bool prop=t>=propFirst && t<propFirst+2;
        Require(ClassBits(classes[t])==(prop ? 0u : 1u),prop ? "the prop's triangles have class 0" : "every veg_grass triangle has class 1");
    }
}
void TestMotionSpanWindExact()
{
    // the wind offset is a pure function of the frame: span K equals the K-frame difference
    const Vec3 rest{1.f,0.5f,2.f}; const float weight=0.8f, strength=1.f; const unsigned t=120, K=8;
    Vec3 span=WindOffset(t,rest,weight,strength)-WindOffset(t-K,rest,weight,strength);
    Vec3 chain{0,0,0};
    for(unsigned k=0;k<K;++k) chain=chain+(WindOffset(t-k,rest,weight,strength)-WindOffset(t-k-1,rest,weight,strength));
    Require(Length(span-chain)<1e-5f,"span K = sum of K one-frame steps for wind");
}
void TestMotionSpanOptions()
{
    Require(Parse({"bench","--motion-span","8","--camera","combo"}).motionSpan==8,"span parsed");
    Require(Rejected({"bench","--motion-span","8","--camera","follow"}),"span>1 with follow rejected");
    Require(Rejected({"bench","--motion-span","8","--interactive"}),"span>1 with --interactive rejected");
    Require(!Rejected({"bench","--motion-span","1","--camera","follow"}),"span 1 with follow accepted");
    Require(Rejected({"bench","--motion-span","0"}) && Rejected({"bench","--motion-span","65"}),"span outside 1..64 rejected");
    Require(Parse({"bench"}).motionSpan==1,"default span 1");
    {
        const Options c=Parse({"bench","--motion-span","cadence","--cadence","8","--cadence-phase","1","--camera","combo"});
        Require(c.cadence==8 && c.cadencePhase==1,"cadence parsed");
        Require(c.SpanAt(1)==8,"full pass frame points to the previous full pass");
        Require(c.SpanAt(2)==1 && c.SpanAt(8)==7 && c.SpanAt(9)==8 && c.SpanAt(10)==1,"carried frames point to the last full pass");
        Require(c.SpanAt(0)==7,"frame before the phase wraps");
        const Options k=Parse({"bench","--motion-span","8"});
        Require(k.cadence==0 && k.SpanAt(5)==8,"plain K unchanged");
        Require(Parse({"bench"}).SpanAt(3)==1,"default span 1 at any frame");
    }
    Require(Rejected({"bench","--motion-span","cadence","--camera","combo"}),"cadence needs --cadence");
    Require(Rejected({"bench","--motion-span","cadence","--cadence","9","--camera","combo"}),"cadence 2..8");
    Require(Rejected({"bench","--motion-span","cadence","--cadence","8","--cadence-phase","8","--camera","combo"}),"phase below N");
    Require(Rejected({"bench","--motion-span","cadence","--cadence","8","--camera","follow"}),"cadence with follow rejected");
    Require(Rejected({"bench","--motion-span","cadence","--cadence","8","--interactive"}),"cadence with interactive rejected");
    Require(Rejected({"bench","--cadence","8"}),"--cadence without --motion-span cadence");
    Require(Parse({"bench","--dump-raw","motion"}).dumpRaw=="motion" && Parse({"bench"}).dumpRaw.empty(),"--dump-raw motion parsed");
    Require(Rejected({"bench","--dump-raw","colour"}),"--dump-raw accepts only motion and depth");
    Require(Parse({"bench","--dump-raw","depth"}).dumpRaw=="depth","--dump-raw depth parsed");
    Require(Parse({"bench","--dump-raw","motion,depth"}).dumpRaw=="motion,depth","--dump-raw motion,depth parsed");
    Require(Rejected({"bench","--dump-raw","motion,colour"}) && Rejected({"bench","--dump-raw","depth,depth"}),"--dump-raw lists only motion and depth, once each");
    {
        const Options p=Parse({"bench","--dump-raw","prevdepth"});
        Require(p.DumpsRaw("prevdepth") && !p.DumpsRaw("motion") && !p.DumpsRaw("depth"),"--dump-raw prevdepth parsed, alone");
        Require(!Parse({"bench","--dump-raw","motion,depth"}).DumpsRaw("prevdepth"),"motion,depth dumps no prevdepth");
        Require(Rejected({"bench","--dump-raw","prevdepth,depth"}) && Rejected({"bench","--dump-raw","motion,prevdepth"}),"prevdepth only alone");
    }
}
// --start-frame and --exposure-record / --exposure-replay (application.cpp, exposure_track.h).
void TestShortRunOptions()
{
    const Options s=Parse({"bench","1080","--camera","combo","--start-frame","920","--dump","952,1079"});
    Require(s.startFrame==920 && Parse({"bench"}).startFrame==0,"--start-frame parsed, default 0");
    Require(Rejected({"bench","100","--start-frame","100"}) && Rejected({"bench","--start-frame","-1"}),"--start-frame within 0..frames-1");
    Require(Rejected({"bench","--start-frame","8","--camera","follow"}) && Rejected({"bench","--start-frame","8","--interactive"}),
            "--start-frame with follow or --interactive rejected");
    Require(!Rejected({"bench","--start-frame","0","--camera","follow"}),"--start-frame 0 with follow accepted");
    Require(Rejected({"bench","--start-frame","8","--auto-exposure","1"}),"--start-frame under auto exposure without a replay rejected");
    Require(Parse({"bench","--start-frame","8","--auto-exposure","1","--exposure-replay","e.txt"}).exposureReplay=="e.txt","replay parsed");
    Require(!Rejected({"bench","--start-frame","8"}),"--start-frame without auto exposure needs no replay");
    Require(Parse({"bench","--exposure-record","e.txt"}).exposureRecord=="e.txt","record parsed");
    Require(Rejected({"bench","--exposure-record","a","--exposure-replay","b"}),"record and replay together rejected");
    Require(Rejected({"bench","--exposure-replay","a","--interactive"}),"replay with --interactive rejected");
    Require(Rejected({"bench","200","--start-frame","100","--dump","99"}) && Rejected({"bench","200","--start-frame","100","--resize-at","50"}),
            "events before --start-frame rejected");
    Require(Rejected({"bench","200","--start-frame","100","--nr","native","--nr-pause","90,110"}),"a pause starting before S rejected");
    Require(!Rejected({"bench","200","--start-frame","100","--dump","100"}),"a dump at S accepted");
    const Options r=Parse({"bench","200","--nr","native","--nr-reset-at","101,150"});
    Require(r.NrResetAt(101) && r.NrResetAt(150) && !r.NrResetAt(100) && !Parse({"bench"}).NrResetAt(0),"--nr-reset-at parsed, default none");
    Require(Rejected({"bench","200","--nr-reset-at","101"}) && Rejected({"bench","200","--nr","native","--nr-reset-at","200"}),
            "--nr-reset-at needs NR and a frame below the count");
    Require(Rejected({"bench","200","--start-frame","100","--nr","native","--nr-reset-at","99"}),"a reset before S rejected");
    const Options v=Parse({"bench","200","--render","raster","--start-frame","100","--variants","3","--variant-set","1,17,1,2,17,0"});
    Require(v.variants==3 && v.variantSet.size()==2 && v.variantSet[1].frame==2 && v.variantSet[1].id==17 && Parse({"bench"}).variants==1,
            "--variants and --variant-set parsed");
    Require(Rejected({"bench","--render","raster","--variants","2","--variant-set","2,17,1"}) &&
            Rejected({"bench","--render","raster","--variants","2","--variant-set","0,17,1"}),"--variant-set K within 1..K-1");
    Require(Rejected({"bench","200","--render","raster","--variants","2","--set-at","150,17,1"}) &&
            Rejected({"bench","--render","pathtrace","--variants","2"}) && Rejected({"bench","--render","raster","--variants","2","--dump-raw","depth"}),
            "--variants without frame events, path tracing or raw dumps");
    // the track round-trips each float exactly (%.9g), frames keep their numbers, a frame before 0 has no reading
    const ExposureReading a{0.00225f,1.0f/3,3.0e-7f,true}, b{1.17549435e-38f,123456.789f,7,true};
    const auto frames=ParseExposureTrack("# header\n"+ExposureLine(919,a)+ExposureLine(920,b));
    Require(frames.size()==2 && frames.at(919).valid && frames.at(919).scale==a.scale && frames.at(919).average==a.average &&
            frames.at(919).highlight==a.highlight && frames.at(920).scale==b.scale && frames.at(920).average==b.average,"exposure track exact");
    bool threw=false; try { ParseExposureTrack("12 1.0 x\n"); } catch(const std::exception &) { threw=true; }
    Require(threw,"malformed exposure line rejected");
    ExposureTrack none; Require(!none.At(-1).valid && !none.Recording() && !none.Replaying(),"no reading before frame 0");
}
}
void FoliageChecks()
{
    ClassifierChecks(); ScenePathChecks(false); ScenePathChecks(true); TestMotionSpanWindExact(); TestMotionSpanOptions(); TestShortRunOptions();
    std::puts("[test] foliage class checks passed");
}
