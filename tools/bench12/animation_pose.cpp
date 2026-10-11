#include "animation_pose.h"

void AnimationPose::Initialize(const pwgltf::Scene &scene,const std::vector<pwgltf::PrimitiveRef> &refs)
{
    worlds.reserve(scene.nodes.size()); local.reserve(scene.nodes.size()); stack.reserve(scene.nodes.size());
    nodes.resize(scene.nodes.size()); skins.resize(scene.skins.size());
    nodeChannels.assign(scene.nodes.size(),{}); weightChannels.assign(scene.nodes.size(),{});
    for(size_t c=0;c<scene.channels.size();++c) {
        const int node=scene.channels[c].node;
        if(node<0 || size_t(node)>=scene.nodes.size()) continue;
        (scene.channels[c].path==3 ? weightChannels : nodeChannels)[size_t(node)].push_back(c);
    }
    restLocal.resize(scene.nodes.size());
    for(size_t i=0;i<scene.nodes.size();++i) if(nodeChannels[i].empty()) restLocal[i]=scene.LocalAt(int(i),0);
    std::vector<bool> usedNodes(nodes.size()),usedSkins(skins.size());
    for(auto ref:refs) usedNodes[ref.node]=true;
    for(size_t i=0;i<nodes.size();++i) if(usedNodes[i]) {
        activeNodes.push_back(i);
        size_t weights=scene.nodes[i].weights.size();
        for(const auto &c:scene.channels) if(c.node==int(i) && c.path==3) weights=std::max(weights,size_t(c.sampler.components));
        nodes[i].weights.reserve(weights);
        const int skin=scene.nodes[i].skin;
        if(skin>=0 && size_t(skin)<skins.size()) {
            usedSkins[skin]=true; nodes[i].joints.resize(scene.skins[skin].joints.size());
        }
    }
    for(size_t i=0;i<skins.size();++i) if(usedSkins[i]) {
        activeSkins.push_back(i); skins[i].resize(scene.skins[i].joints.size());
    }
}
// pwgltf::Scene::LocalAt and WorldsAt with the per-node channel lists: the same samples, TRS and products.
void AnimationPose::Worlds(const pwgltf::Scene &scene,float time)
{
    const size_t n=scene.nodes.size();
    worlds.assign(n,Identity()); local.resize(n);
    for(size_t i=0;i<n;++i) {
        if(nodeChannels[i].empty()) { local[i]=restLocal[i]; continue; }
        const auto &node=scene.nodes[i];
        Vec3 tr=node.translation, sc=node.scale; pwgltf::Quat q=node.rotation;
        for(size_t c:nodeChannels[i]) {
            const auto &channel=scene.channels[c];
            float v[4]={};
            channel.sampler.Sample(time,v);
            if(channel.path==0) tr={v[0],v[1],v[2]};
            else if(channel.path==1) q={v[0],v[1],v[2],v[3]};
            else if(channel.path==2) sc={v[0],v[1],v[2]};
        }
        local[i]=pwgltf::Scene::TRS(tr,q,sc);
    }
    stack.clear(); stack.insert(stack.end(),scene.roots.begin(),scene.roots.end());
    while(!stack.empty()) {
        const int i=stack.back(); stack.pop_back();
        const int p=scene.nodes[size_t(i)].parent;
        worlds[size_t(i)]=p>=0 ? Mul(worlds[size_t(p)],local[size_t(i)]) : local[size_t(i)];
        for(int c:scene.nodes[size_t(i)].children) if(c>=0 && size_t(c)<n) stack.push_back(c);
    }
}
void AnimationPose::Weights(const pwgltf::Scene &scene,size_t node,float time,std::vector<float> &out) const
{
    out=scene.nodes[node].weights; // pwgltf::Scene::WeightsAt
    for(size_t c:weightChannels[node]) {
        const auto &channel=scene.channels[c];
        out.assign(size_t(channel.sampler.components),0.0f);
        channel.sampler.Sample(time,out.data());
    }
}
void AnimationPose::Evaluate(const pwgltf::Scene &scene,float time)
{
    Worlds(scene,time);
    for(size_t i:activeSkins) {
        const auto &skin=scene.skins[i];
        for(size_t j=0;j<skin.joints.size();++j) {
            const int node=skin.joints[j];
            skins[i][j]=node>=0 && size_t(node)<worlds.size() ? Mul(worlds[node],skin.inverseBind[j]) : Identity();
        }
    }
    for(size_t i:activeNodes) {
        auto &node=nodes[i]; Weights(scene,i,time,node.weights);
        if(node.joints.empty()) continue;
        const auto &skin=scene.skins[scene.nodes[i].skin];
        const auto &palette=skins[scene.nodes[i].skin];
        const Mat4 inverse=pwgltf::InvertAffine(worlds[i]);
        // A shared skin can be instanced by nodes with different transforms. Convert its
        // one world palette to each mesh basis once, preserving the loader's normal rules.
        for(size_t j=0;j<palette.size();++j)
            node.joints[j]=Affine3x4(skin.joints[j]>=0 && size_t(skin.joints[j])<worlds.size() ? Mul(inverse,palette[j]) : Identity());
    }
}
