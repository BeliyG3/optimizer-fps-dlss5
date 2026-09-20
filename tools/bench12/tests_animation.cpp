#include "animation.h"
#include "follow_camera.h"
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace {
void Require(bool value,const char *message) { if(!value) throw std::runtime_error(message); }
bool Near(float a,float b) { return std::abs(a-b)<1e-5f; }
bool Same(Vec3 a,Vec3 b) { return Near(a.x,b.x) && Near(a.y,b.y) && Near(a.z,b.z); }
pwgltf::Scene Fixture(const std::filesystem::path &directory)
{
    std::vector<unsigned char> binary;
    std::string views,accessors;
    unsigned count=0;
    auto accessor=[&](const auto &data,unsigned elements,const char *type,int component) {
        while(binary.size()%4) binary.push_back(0);
        const size_t offset=binary.size(), bytes=data.size()*sizeof(data[0]);
        auto start=reinterpret_cast<const unsigned char *>(data.data()); binary.insert(binary.end(),start,start+bytes);
        if(count) { views+=','; accessors+=','; }
        views+="{\"buffer\":0,\"byteOffset\":"+std::to_string(offset)+",\"byteLength\":"+std::to_string(bytes)+"}";
        accessors+="{\"bufferView\":"+std::to_string(count)+",\"componentType\":"+std::to_string(component)+
            ",\"count\":"+std::to_string(elements)+",\"type\":\""+type+"\"}";
        return count++;
    };
    accessor(std::vector<float>{0,0,0,1,0,0,1,1,0,0,1,0},4,"VEC3",5126);
    accessor(std::vector<uint16_t>{0,1,2,0,2,3},6,"SCALAR",5123);
    accessor(std::vector<uint16_t>(16,0),4,"VEC4",5123);
    accessor(std::vector<float>{1,0,0,0,1,0,0,0,1,0,0,0,1,0,0,0},4,"VEC4",5126);
    accessor(std::vector<float>{0,2},2,"SCALAR",5126);
    accessor(std::vector<float>{0,0,0,2,0,0},2,"VEC3",5126);
    std::string json="{\"asset\":{\"version\":\"2.0\"},\"buffers\":[{\"byteLength\":"+std::to_string(binary.size())+
        "}],\"bufferViews\":["+views+"],\"accessors\":["+accessors+R"(],
        "meshes":[{"primitives":[{"attributes":{"POSITION":0,"JOINTS_0":2,"WEIGHTS_0":3},"indices":1}]}],
        "nodes":[{"name":"hero","mesh":0,"skin":0},{"mesh":0,"translation":[4,0,0]},{}],
        "skins":[{"joints":[2]}],"scenes":[{"nodes":[0,1,2]}],"scene":0,
        "animations":[{"samplers":[{"input":4,"output":5}],"channels":[{"sampler":0,"target":{"node":2,"path":"translation"}}]}]})";
    while(json.size()%4) json+=' ';
    while(binary.size()%4) binary.push_back(0);
    const uint32_t header[]={0x46546c67,2,uint32_t(28+json.size()+binary.size()),uint32_t(json.size()),0x4e4f534a};
    const uint32_t bin[]={uint32_t(binary.size()),0x004e4942};
    auto path=directory/"test_animation.glb";
    {
        std::ofstream file(path,std::ios::binary); file.write(reinterpret_cast<const char *>(header),sizeof(header));
        file.write(json.data(),std::streamsize(json.size())); file.write(reinterpret_cast<const char *>(bin),sizeof(bin));
        file.write(reinterpret_cast<const char *>(binary.data()),std::streamsize(binary.size()));
        Require(bool(file),"Write animation fixture");
    }
    pwgltf::Scene scene; Require(pwgltf::Load(path.string().c_str(),scene),"Load skinned animation GLB");
    std::filesystem::remove(path); return scene;
}
void SplitChecks(pwgltf::Scene &scene)
{
    auto dynamic=pwgltf::Primitives(scene,1), fixed=pwgltf::Primitives(scene,0);
    Require(dynamic.size()==1 && dynamic[0].node==0 && fixed.size()==1 && fixed[0].node==1,"Skinned/static split");
    std::vector<pwgltf::Vertex> full,subset,statics; std::vector<Vec3> previous,subsetPrevious,staticPrevious;
    pwgltf::VertexWorkspace scratch; scratch.Reserve(scene,dynamic);
    pwgltf::BuildVertices(scene,1,0.25f,full,previous);
    pwgltf::BuildVerticesSubset(scene,1,0.25f,dynamic,scratch,subset,subsetPrevious);
    pwgltf::BuildVerticesSubset(scene,1,0.25f,fixed,scratch,statics,staticPrevious);
    Require(full.size()==12 && subset.size()==6 && statics.size()==6,"Quad subset vertex counts");
    for(size_t i=0;i<subset.size();++i) {
        Require(Same(full[i].pos,subset[i].pos) && Same(full[i].normal,subset[i].normal) &&
            Same(previous[i],subsetPrevious[i]) && full[i].material==subset[i].material,"Subset equals legacy full evaluation");
        Require(Near(subset[i].pos.x-subsetPrevious[i].x,0.25f),"Skinning previous positions evaluate t-dt");
        Require(Same(full[i+6].pos,statics[i].pos) && Same(statics[i].pos,staticPrevious[i]),"Static subset unchanged");
    }
    pwgltf::BuildVerticesSubset(scene,1,0,dynamic,scratch,subset,subsetPrevious);
    for(size_t i=0;i<subset.size();++i) Require(Same(subset[i].pos,subsetPrevious[i]),"Paused skin has zero object motion");
    pwgltf::BuildVerticesSubset(scene,0.1f,0,dynamic,scratch,subset,subsetPrevious,nullptr,nullptr,1.9f);
    Require(Near(subset[0].pos.x,0.1f) && Near(subsetPrevious[0].x,1.9f),"Loop boundary uses previous wrapped time");
    scene.nodes[1].parent=2; scene.nodes[2].children={1}; scene.roots={0,2};
    Require(pwgltf::Primitives(scene,1).size()==2,"Animated ancestor makes mesh dynamic");
    scene.nodes[1].parent=-1; scene.nodes[2].children.clear(); scene.roots={0,1,2};
    auto morph=scene.channels[0]; morph.node=1; morph.path=3; morph.sampler.components=1; morph.sampler.values={0,1};
    scene.channels.push_back(morph);
    Require(pwgltf::DynamicNode(scene,1),"Morph weight channel makes node dynamic");
    scene.channels.pop_back();
}
void TimeChecks()
{
    Require(Near(LoopAnimation(2.25,2),0.25f) && Near(LoopAnimation(-0.25,2),1.75f) && LoopAnimation(4,0)==0,"Time looping");
    AnimationClock clock{0,2};
    for(int i=0;i<60;++i) clock.Advance(AnimationDelta(false,999),1,true,false);
    Require(Near(clock.Current(),1),"Deterministic 60Hz animation");
    clock.Advance(0.5,1,false,false); Require(Near(clock.Current(),1),"Paused clock");
    clock.Advance(999,1,false,true); Require(Near(clock.Current(),1+1.0f/60),"Single frame step");
    clock.Seek(-0.25f); Require(Near(clock.Current(),1.75f),"Scrub wraps");
    Require(AnimationDelta(true,1)==1.0/15 && AnimationDelta(true,-1)==0,"Interactive delta clamping");
    FollowCamera follow; Options options;
    auto pose=follow.Update({3,0,4},{0,0,1},options,1.0f/60);
    Require(Same(pose.eye,{3,1.9f,-1}) && Same(pose.target,{3,1,4}),"Follow default third-person offset");
    auto next=follow.Update({4,0,4},{1,0,0},options,1.0f/60);
    Require(CameraMoved(next,pose) && next.eye.x> -1 && next.eye.x<3,"Follow smooths position and heading");
}
void RangeChecks(pwgltf::Scene source)
{
    source.nodes[1].skin=0;
    source.nodes[1].scale={2,0.5f,1.5f};
    source.nodes[1].rotation={0,0.258819f,0,0.965926f};
    source.skins[0].joints={2,2,2,2};
    source.skins[0].inverseBind.resize(4,Identity());
    source.skins[0].inverseBind[1].m[0][3]=0.125f;
    source.skins[0].inverseBind[2].m[1][1]=0.75f;
    auto &p=source.meshes[0].primitives[0];
    p.positions.clear(); p.normals.clear(); p.indices.clear(); p.joints.clear(); p.skinWeights.clear();
    p.targets.resize(1); p.uvs.clear();
    // A single large primitive must be divisible among workers. Two nodes instance
    // the same skin with different mesh transforms; all output destinations differ.
    for(unsigned i=0;i<24000;++i) {
        p.positions.push_back({float(i%100)*0.01f,float(i/100)*0.01f,0.1f});
        p.normals.push_back({0.3f,0.9f,0.1f}); p.indices.push_back(i);
        p.uvs.push_back(float(i%3)*0.25f); p.uvs.push_back(0.75f);
        p.targets[0].push_back({0.05f,0.01f,0});
        p.joints.insert(p.joints.end(),{0,1,2,uint16_t(i%5 ? 3 : 999)});
        if(i%7) p.skinWeights.insert(p.skinWeights.end(),{0.1f,0.4f,0.2f,0.1f});
        else p.skinWeights.insert(p.skinWeights.end(),{0,0,0,0});
    }
    auto morph=source.channels[0]; morph.node=0; morph.path=3;
    morph.sampler.components=1; morph.sampler.values={0,1}; source.channels.push_back(morph);
    source.nodes[1].weights={0.3f};
    std::vector<TriVertex> output(48000);
    std::vector<unsigned> order(output.size()/3);
    for(size_t i=0;i<order.size();++i) order[i]=unsigned(order.size()-i-1);
    Animation animation; animation.source=std::move(source); animation.Start(output.data(),order);
    auto refs=pwgltf::Primitives(animation.source,1); pwgltf::VertexWorkspace scratch;
    std::vector<pwgltf::Vertex> expected; std::vector<Vec3> previous;
    const float times[][2]={{0,0},{0.5f,0},{1,0.5f},{1,1},{1,1},{0.2f,1.9f},{0.2f,0.1f},{0.2f,0.2f},{0.1f,0.2f}};
    for(const auto &pair:times) {
        animation.Evaluate(pair[0],pair[1]);
        pwgltf::BuildVerticesSubset(animation.source,pair[0],0,refs,scratch,expected,previous,nullptr,nullptr,pair[1]);
        for(size_t i=0;i<output.size();++i) {
            const size_t original=size_t(order[i/3])*3+i%3;
            Require(Same(output[i].pos,expected[original].pos) && Same(output[i].prevPos,previous[original]),"Range/morph/skin instance positions");
            Require(Same(output[i].normal,expected[original].normal),"Blended affine normals match legacy");
            Require(output[i].uv[0]==expected[original].uv[0] && output[i].uv[1]==expected[original].uv[1],"Precomputed final-order UVs");
        }
    }
    Require(animation.Evaluate(0.1f,0.1f) && !animation.geometryChanged,"Pause uploads previous=current without refit");
    Require(!animation.Evaluate(0.1f,0.1f) && !animation.geometryChanged && animation.cpuMs==0,"Second paused frame skips all work");
    for(const auto &v:output) Require(Same(v.pos,v.prevPos),"Pause transition zeroes motion exactly once");
}
}
void AnimationChecks(const std::filesystem::path &directory)
{
    Animation animation; animation.source=Fixture(directory); SplitChecks(animation.source); TimeChecks();
    RangeChecks(animation.source);
    // A second dynamic primitive exercises independent workers and deterministic concatenation.
    animation.source.meshes[0].primitives.push_back(animation.source.meshes[0].primitives[0]);
    std::vector<TriVertex> output(12); animation.Start(output.data());
    auto refs=pwgltf::Primitives(animation.source,1); pwgltf::VertexWorkspace scratch;
    std::vector<pwgltf::Vertex> expected; std::vector<Vec3> previous;
    for(float t:{0.0f,0.5f,1.5f,1.5f,0.1f}) {
        float old=LoopAnimation(double(t)-0.1,2);
        animation.Evaluate(t,old);
        pwgltf::BuildVerticesSubset(animation.source,t,0,refs,scratch,expected,previous,nullptr,nullptr,old);
        Require(expected.size()==output.size(),"Worker subset count");
        for(size_t i=0;i<expected.size();++i)
            Require(Same(expected[i].pos,output[i].pos) && Same(previous[i],output[i].prevPos) &&
                Same(expected[i].normal,output[i].normal),"Worker evaluation equals serial");
    }
    animation.Evaluate(1,1);
    for(size_t i=0;i<output.size();++i) Require(Same(output[i].pos,output[i].prevPos),"Paused worker positions");
}
