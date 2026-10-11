// GPU skinning: the layout, palettes and per-vertex math (skin_layout.h, skin_math.h = shaders/skin.hlsl) against the CPU path
// (Animation) on a small scene: two copies of a skinned, morphed mesh with joints past the skin and weightless vertices, a
// rigidly animated node and a --wind node.
#include "animation.h"
#include "skin_math.h"
#include <stdexcept>

namespace {
void Require(bool value, const char *message) { if(!value) throw std::runtime_error(message); }
bool Close(Vec3 a, Vec3 b, float tolerance) { return std::abs(a.x-b.x)<=tolerance && std::abs(a.y-b.y)<=tolerance && std::abs(a.z-b.z)<=tolerance; }
pwgltf::Sampler Keys(std::vector<float> values, int components, bool quaternion=false)
{
    pwgltf::Sampler s; s.times={0,1,2}; s.values=std::move(values); s.components=components; s.quaternion=quaternion; return s;
}
pwgltf::Scene SkinScene()
{
    pwgltf::Scene scene;
    pwgltf::Primitive skinned;
    for(unsigned i=0;i<400;++i) {
        const float x=float(i%20)*0.05f, y=float(i/20)*0.05f;
        skinned.positions.push_back({x,y,0.1f*std::sin(x*7)}); skinned.normals.push_back(Normalize(Vec3{0.2f,0.3f+x,0.9f}));
        skinned.uvs.push_back(x); skinned.uvs.push_back(y);
        skinned.joints.insert(skinned.joints.end(),{0,1,2,uint16_t(i%5 ? 3 : 999)});
        if(i%7) skinned.skinWeights.insert(skinned.skinWeights.end(),{0.5f,0.3f,0.15f,0.05f});
        else skinned.skinWeights.insert(skinned.skinWeights.end(),{0,0,0,0});
    }
    for(unsigned y=0;y+1<20;++y) for(unsigned x=0;x+1<20;++x) {
        const uint32_t a=y*20+x; skinned.indices.insert(skinned.indices.end(),{a,a+1,a+21,a,a+21,a+20});
    }
    skinned.targets.resize(2);
    for(unsigned i=0;i<400;++i) { skinned.targets[0].push_back({0.02f,0.01f*float(i%3),0}); if(i<300) skinned.targets[1].push_back({0,0,0.03f}); }
    pwgltf::Primitive leaf;
    leaf.positions={{0,0,0},{1,0,0},{0,2,0},{1,2,0.5f}}; leaf.normals={{0,0,1},{0,0,1},{0,0,1},{0,0.6f,0.8f}};
    leaf.indices={0,1,2,1,3,2}; leaf.uvs={0,0,1,0,0,1,1,1}; leaf.materialName="leaf";
    scene.meshes.resize(2); scene.meshes[0].primitives={skinned}; scene.meshes[1].primitives={leaf};
    scene.nodes.resize(6);
    scene.nodes[0].name="hero"; scene.nodes[0].mesh=0; scene.nodes[0].skin=0;
    scene.nodes[1].name="copy"; scene.nodes[1].mesh=0; scene.nodes[1].skin=0; scene.nodes[1].weights={0.3f,0.6f};
    scene.nodes[1].translation={2,0,-1}; scene.nodes[1].scale={1.5f,0.8f,1.2f}; scene.nodes[1].rotation={0,0.258819f,0,0.965926f};
    scene.nodes[2].name="root_joint"; scene.nodes[2].children={3};
    scene.nodes[3].name="child_joint"; scene.nodes[3].parent=2; scene.nodes[3].translation={0,0.5f,0};
    scene.nodes[4].name="veg_bush"; scene.nodes[4].mesh=1; scene.nodes[4].translation={-3,0,2}; scene.nodes[4].wind=true;
    scene.nodes[5].name="crate"; scene.nodes[5].mesh=1;
    scene.roots={0,1,2,4,5};
    pwgltf::Skin skin; skin.joints={2,3,2,3}; skin.inverseBind.assign(4,Identity());
    skin.inverseBind[1].m[1][3]=-0.5f; skin.inverseBind[2].m[0][0]=0.75f; skin.inverseBind[3].m[0][3]=0.125f;
    scene.skins={skin};
    auto channel=[&](int node, int path, pwgltf::Sampler s) { pwgltf::Channel c; c.node=node; c.path=path; c.sampler=std::move(s); scene.channels.push_back(c); };
    channel(2,0,Keys({0,0,0, 0.4f,0.1f,0, 0.2f,0.3f,-0.2f},3));
    channel(3,1,Keys({0,0,0,1, 0,0,0.382683f,0.92388f, 0.3f,0,0,0.953939f},4,true));
    channel(0,3,Keys({0,0, 1,0.5f, 0.2f,1},2));
    channel(5,0,Keys({1,0,1, 1.5f,0.2f,1, 1,0.4f,0.6f},3));
    scene.animationEnd=2;
    return scene;
}
}
void SkinningChecks()
{
    const pwgltf::Scene scene=SkinScene();
    const auto refs=pwgltf::Primitives(scene,1);
    Require(refs.size()==4,"Skinning fixture: four dynamic primitives");
    size_t corners=0; for(auto ref:refs) corners+=scene.meshes[size_t(scene.nodes[ref.node].mesh)].primitives[ref.primitive].indices.size();
    std::vector<unsigned> order(corners/3);
    for(size_t i=0;i<order.size();++i) order[i]=unsigned((i*7)%order.size()); // a permutation: gcd(7, count) == 1
    Require(order.size()%7!=0,"Skinning fixture: the order is a permutation");
    std::vector<TriVertex> cpu(corners), gpu(corners);
    Animation animation; animation.source=scene; animation.windStrength=1.5f; animation.Start(cpu.data(),order);
    const SkinLayout layout=BuildSkinLayout(scene,order,gpu.data());
    Require(layout.items.size()==4 && layout.nodes.size()==4 && layout.corners==corners,"Skin layout items, nodes and corners");
    Require(layout.sources.size()==400+4,"Copies of a skinned mesh share their source vertices");
    Require(layout.items[0].source==layout.items[1].source && layout.items[2].source==layout.items[3].source,"Shared sources per mesh");
    Require(layout.paletteEntries==5+5+1+1 && !layout.wind.empty(),"Palette entries: 1 + joints per skinned node, 1 per rigid node");
    for(size_t i=0;i<corners;++i) Require(cpu[i].uv[0]==gpu[i].uv[0] && cpu[i].uv[1]==gpu[i].uv[1],"Layout UVs equal Animation::Start's");
    for(const auto &s:layout.sources) {
        const float total=s.weights[0]+s.weights[1]+s.weights[2]+s.weights[3];
        Require(total>0,"Every source vertex has a positive weight");
    }
    AnimationPose current, previous; current.Initialize(scene,refs); previous.Initialize(scene,refs);
    std::vector<Affine3x4> paletteNow(layout.paletteEntries), paletteBefore(layout.paletteEntries);
    std::vector<float> weightsNow(layout.weightCount), weightsBefore(layout.weightCount);
    const struct { float time, previous; unsigned wind; } frames[]={{0,0,0},{0.5f,0,1},{1,0.5f,2},{1,1,3},{1.75f,1,4},{0.2f,1.9f,5},{0.2f,0.2f,5}};
    float worst=0;
    for(const auto &f:frames) {
        animation.Evaluate(f.time,f.previous,f.wind);
        current.Evaluate(scene,f.time); previous.Evaluate(scene,f.previous);
        std::vector<Mat4> reference; scene.WorldsAt(f.time,reference);
        for(size_t i=0;i<reference.size();++i) for(size_t r=0;r<4;++r) for(size_t c=0;c<4;++c)
            Require(reference[i].m[r][c]==current.worlds[i].m[r][c],"Indexed pose worlds equal pwgltf::Scene::WorldsAt exactly");
        WritePalette(layout,current,paletteNow.data()); WriteMorphWeights(layout,current,weightsNow.data());
        WritePalette(layout,previous,paletteBefore.data()); WriteMorphWeights(layout,previous,weightsBefore.data());
        const SkinFrame now{paletteNow.data(),weightsNow.data(),WindPhase(f.wind)};
        const SkinFrame before{paletteBefore.data(),weightsBefore.data(),WindPhase(f.wind>0 ? f.wind-1 : 0)};
        for(unsigned g=0;g<layout.ItemVertices();++g) {
            Vec3 normal{};
            const Vec3 position=SkinVertex(layout,g,now,1.5f,&normal), old=SkinVertex(layout,g,before,1.5f,nullptr);
            for(unsigned k=layout.offsets[g];k<layout.offsets[g+1];++k) {
                const TriVertex &v=cpu[layout.outputs[k]];
                worst=std::max({worst,std::abs(v.pos.x-position.x),std::abs(v.pos.y-position.y),std::abs(v.pos.z-position.z)});
                Require(Close(v.pos,position,2e-5f),"GPU skin position equals the CPU path");
                Require(Close(v.prevPos,old,2e-5f),"GPU skin previous position equals the CPU path");
                Require(Close(v.normal,normal,2e-5f),"GPU skin normal equals the CPU path");
            }
        }
    }
    std::printf("[test] gpu skinning math: %u item vertices, %u corners, largest position difference %.2g m\n",layout.ItemVertices(),
                layout.corners,double(worst));
}
