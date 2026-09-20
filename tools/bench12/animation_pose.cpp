#include "animation_pose.h"

void AnimationPose::Initialize(const pwgltf::Scene &scene,const std::vector<pwgltf::PrimitiveRef> &refs)
{
    worlds.reserve(scene.nodes.size()); local.reserve(scene.nodes.size()); stack.reserve(scene.nodes.size());
    nodes.resize(scene.nodes.size()); skins.resize(scene.skins.size());
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
void AnimationPose::Evaluate(const pwgltf::Scene &scene,float time)
{
    scene.WorldsAt(time,worlds,local,stack);
    for(size_t i:activeSkins) {
        const auto &skin=scene.skins[i];
        for(size_t j=0;j<skin.joints.size();++j) {
            const int node=skin.joints[j];
            skins[i][j]=node>=0 && size_t(node)<worlds.size() ? Mul(worlds[node],skin.inverseBind[j]) : Identity();
        }
    }
    for(size_t i:activeNodes) {
        auto &node=nodes[i]; scene.WeightsAt(int(i),time,node.weights);
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
