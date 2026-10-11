#include "skin_layout.h"
#include "wind.h"
#include <map>
#include <stdexcept>
#include <tuple>

namespace {
unsigned JointCount(const pwgltf::Scene &scene, size_t node)
{
    const int skin=scene.nodes[node].skin;
    return skin>=0 && size_t(skin)<scene.skins.size() ? unsigned(scene.skins[size_t(skin)].joints.size()) : 0u;
}
unsigned MorphWeightCount(const pwgltf::Scene &scene, size_t node)
{
    size_t count=scene.nodes[node].weights.size(); // AnimationPose::Initialize's capacity
    for(const auto &c:scene.channels) if(c.node==int(node) && c.path==3) count=std::max(count,size_t(c.sampler.components));
    return unsigned(count);
}
// DeformVertex's joint rules for one vertex of a node with `joints` skin joints.
void Joints(const pwgltf::Primitive &p, size_t v, unsigned joints, SkinSourceVertex &out)
{
    unsigned entry[4]={0,0,0,0}; float weight[4]={0,0,0,0}, total=0;
    if(!p.joints.empty() && joints) for(size_t k=0;k<4;++k) {
        const float w=v*4+k<p.skinWeights.size() ? p.skinWeights[v*4+k] : 0.0f;
        const size_t j=v*4+k<p.joints.size() ? p.joints[v*4+k] : size_t(joints);
        if(w==0 || j>=joints) continue;
        entry[k]=unsigned(1+j); weight[k]=w; total+=w;
    }
    if(!(total>0)) { entry[0]=0; weight[0]=1; for(size_t k=1;k<4;++k) { entry[k]=0; weight[k]=0; } }
    if(entry[0]>0xffffu || entry[1]>0xffffu || entry[2]>0xffffu || entry[3]>0xffffu) throw std::runtime_error("Skin joint index above 65535");
    out.joints[0]=entry[0]|(entry[1]<<16); out.joints[1]=entry[2]|(entry[3]<<16);
    for(size_t k=0;k<4;++k) out.weights[k]=weight[k];
}
}

SkinLayout BuildSkinLayout(const pwgltf::Scene &scene, const std::vector<unsigned> &order, TriVertex *uvOutput)
{
    SkinLayout layout;
    const auto refs=pwgltf::Primitives(scene,1);
    size_t count=0;
    for(auto ref:refs) {
        const auto &p=scene.meshes[size_t(scene.nodes[ref.node].mesh)].primitives[ref.primitive];
        for(auto i:p.indices) if(i<p.positions.size()) ++count;
    }
    if(count%3 || (!order.empty() && order.size()!=count/3)) throw std::runtime_error("Skin layout triangle order size");
    std::vector<size_t> inverse(count/3); std::vector<bool> seen(inverse.size());
    for(size_t i=0;i<inverse.size();++i) {
        const size_t index=order.empty() ? i : order[i];
        if(index>=inverse.size() || seen[index]) throw std::runtime_error("Skin layout triangle order is not a permutation");
        inverse[index]=i; seen[index]=true;
    }
    std::map<size_t,size_t> nodeIndex; // scene node -> layout.nodes
    std::map<std::tuple<size_t,size_t,unsigned>,unsigned> sharedSources; // (mesh, primitive, skin joints) -> first source vertex
    std::map<std::pair<size_t,size_t>,unsigned> sharedTargets; // (mesh, primitive) -> first delta
    std::vector<std::vector<float>> windWeights;
    size_t expanded=0;
    for(auto ref:refs) {
        const size_t mesh=size_t(scene.nodes[ref.node].mesh);
        const auto &p=scene.meshes[mesh].primitives[ref.primitive];
        auto found=nodeIndex.find(ref.node);
        if(found==nodeIndex.end()) {
            SkinNode node; node.node=ref.node; node.palette=layout.paletteEntries; node.entries=1+JointCount(scene,ref.node);
            node.weights=layout.weightCount; node.weightCount=MorphWeightCount(scene,ref.node);
            layout.paletteEntries+=node.entries; layout.weightCount+=node.weightCount;
            found=nodeIndex.emplace(ref.node,layout.nodes.size()).first; layout.nodes.push_back(node);
        }
        const SkinNode &node=layout.nodes[found->second];
        SkinItem item{}; item.firstVertex=layout.ItemVertices(); item.vertexCount=unsigned(p.positions.size());
        item.palette=node.palette; item.weights=node.weights;
        const auto sourceKey=std::make_tuple(mesh,ref.primitive,node.entries-1);
        if(auto shared=sharedSources.find(sourceKey);shared!=sharedSources.end()) item.source=shared->second;
        else {
            item.source=unsigned(layout.sources.size()); sharedSources.emplace(sourceKey,item.source);
            for(size_t v=0;v<p.positions.size();++v) {
                SkinSourceVertex s{}; s.position=p.positions[v]; s.normal=v<p.normals.size() ? p.normals[v] : Vec3{0,1,0};
                Joints(p,v,node.entries-1,s); layout.sources.push_back(s);
            }
        }
        item.targetCount=unsigned(std::min<size_t>(p.targets.size(),node.weightCount));
        if(item.targetCount) {
            const auto targetKey=std::make_pair(mesh,ref.primitive);
            if(auto shared=sharedTargets.find(targetKey);shared!=sharedTargets.end()) item.targets=shared->second;
            else {
                item.targets=unsigned(layout.targets.size()); sharedTargets.emplace(targetKey,item.targets);
                for(size_t k=0;k<p.targets.size();++k) for(size_t v=0;v<p.positions.size();++v)
                    layout.targets.push_back(v<p.targets[k].size() ? p.targets[k][v] : Vec3{});
            }
        }
        std::vector<float> wind;
        if(scene.nodes[ref.node].wind) wind=WindWeights(scene,ref.node,ref.primitive);
        item.wind=wind.empty() ? 0u : 1u;
        if(!wind.empty() && wind.size()!=p.positions.size()) throw std::runtime_error("Wind weights do not match the primitive");
        windWeights.push_back(std::move(wind));
        // The corners, as Animation::Start numbers them: per source vertex, in index order.
        std::vector<unsigned> starts(p.positions.size()+1,0);
        for(auto i:p.indices) if(i<p.positions.size()) ++starts[i+1];
        for(size_t i=1;i<starts.size();++i) starts[i]+=starts[i-1];
        const unsigned base=unsigned(layout.outputs.size());
        layout.outputs.resize(layout.outputs.size()+starts.back());
        auto cursor=starts;
        for(auto i:p.indices) if(i<p.positions.size()) {
            const size_t out=inverse[expanded/3]*3+expanded%3; ++expanded;
            layout.outputs[base+cursor[i]++]=unsigned(out);
            if(uvOutput) {
                uvOutput[out].uv[0]=size_t(i)*2+1<p.uvs.size() ? p.uvs[size_t(i)*2] : 0.0f;
                uvOutput[out].uv[1]=size_t(i)*2+1<p.uvs.size() ? p.uvs[size_t(i)*2+1] : 0.0f;
            }
        }
        if(layout.offsets.empty()) layout.offsets.push_back(0);
        for(size_t v=0;v<p.positions.size();++v) { layout.offsets.push_back(base+starts[v+1]); layout.vertexItem.push_back(unsigned(layout.items.size())); }
        layout.items.push_back(item);
    }
    layout.corners=unsigned(layout.outputs.size());
    bool anyWind=false; for(const auto &w:windWeights) anyWind=anyWind || !w.empty();
    if(anyWind) {
        layout.wind.reserve(layout.ItemVertices());
        for(size_t i=0;i<layout.items.size();++i) {
            if(windWeights[i].empty()) layout.wind.insert(layout.wind.end(),layout.items[i].vertexCount,0.0f);
            else layout.wind.insert(layout.wind.end(),windWeights[i].begin(),windWeights[i].end());
        }
    }
    if(layout.offsets.empty()) layout.offsets.push_back(0);
    return layout;
}
void WritePalette(const SkinLayout &layout, const AnimationPose &pose, Affine3x4 *rows)
{
    for(const auto &node:layout.nodes) {
        const Mat4 &world=pose.worlds[node.node];
        auto bench=[&](const Mat4 &m) { Affine3x4 r(m); for(size_t c=0;c<4;++c) r.m[2][c]=-r.m[2][c]; return r; }; // ToBench: z -> -z
        rows[node.palette]=bench(world);
        const auto &joints=pose.nodes[node.node].joints;
        for(unsigned j=0;j+1<node.entries;++j) {
            Mat4 joint=Identity();
            if(j<joints.size()) for(size_t r=0;r<3;++r) for(size_t c=0;c<4;++c) joint.m[r][c]=joints[j].m[r][c];
            rows[node.palette+1+j]=bench(Mul(world,joint));
        }
    }
}
void WriteMorphWeights(const SkinLayout &layout, const AnimationPose &pose, float *weights)
{
    for(const auto &node:layout.nodes) {
        const auto &current=pose.nodes[node.node].weights;
        for(unsigned k=0;k<node.weightCount;++k) weights[node.weights+k]=k<current.size() ? current[k] : 0.0f;
    }
}
