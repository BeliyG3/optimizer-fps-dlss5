#pragma once
// GPU skinning (gpu_skinning.h): the static tables the compute pass reads, built once from the glTF scene. They describe the same
// work as Animation (animation.cpp): every dynamic primitive of every node (pwgltf::Primitives(scene, 1), in that order) is one
// item; each of its source vertices is deformed once and written to every corner of the dynamic range that uses it.
// Characters that share a mesh share its source vertices and morph deltas; each node has its own palette and weights.
#include "animation_pose.h"
#include "tri_vertex.h"
#include <vector>

// One source vertex as shaders/skin.hlsl reads it (SkinSource): the glTF object-space position and normal, four palette entries
// (16 bits each, local to the node: 0 is the node's own transform, 1 + j its skin joint j) and their weights. Joints the CPU
// path skips (weight 0, index past the skin) have weight 0 here; a vertex without any weight takes entry 0 with weight 1.
struct SkinSourceVertex { Vec3 position, normal; unsigned joints[2]; float weights[4]; };
static_assert(sizeof(SkinSourceVertex)==48);
// One (node, primitive), as SkinItem: its source vertices, its node's palette and morph weights, its morph deltas
// (targetCount x vertexCount, target-major), whether --wind sways it, and its first vertex in the item vertex numbering.
struct SkinItem { unsigned source, firstVertex, vertexCount, palette, targets, targetCount, weights, wind; };
static_assert(sizeof(SkinItem)==32);
// A node with dynamic primitives: its palette (entries rows, from `palette`) and its morph weights (weightCount, from `weights`).
struct SkinNode { size_t node=0; unsigned palette=0, entries=1, weights=0, weightCount=0; };

struct SkinLayout {
    std::vector<SkinSourceVertex> sources;
    std::vector<Vec3> targets;
    std::vector<SkinItem> items;
    std::vector<SkinNode> nodes;
    std::vector<unsigned> vertexItem; // per item vertex: its item
    std::vector<unsigned> offsets; // per item vertex + 1: its corners are outputs[offsets[g], offsets[g+1])
    std::vector<unsigned> outputs; // corner index in the dynamic range (triangle * 3 + corner, after the order)
    std::vector<float> wind; // per item vertex: the --wind weight (WindWeights), empty when no item has wind
    unsigned paletteEntries=0, weightCount=0, corners=0;
    unsigned ItemVertices() const { return unsigned(vertexItem.size()); }
};

// `order`: Scene's dynamicOrder (empty: source order), as Animation::Start takes it. `uvOutput`, when not null, is the dynamic
// range of the scene vertices: its UVs are written as Animation::Start writes them (the GPU pass leaves them alone).
SkinLayout BuildSkinLayout(const pwgltf::Scene &scene, const std::vector<unsigned> &order, TriVertex *uvOutput=nullptr);
// The palette of every layout node at `pose` (AnimationPose::Evaluate'd): entry 0 = ToBench * world, entry 1 + j = ToBench *
// world * joints[j] (the pose's joint in the mesh basis, as DeformVertex applies it), as 3x4 rows.
void WritePalette(const SkinLayout &layout, const AnimationPose &pose, Affine3x4 *rows);
// The morph weights of every layout node at `pose`, zero past the node's current weights.
void WriteMorphWeights(const SkinLayout &layout, const AnimationPose &pose, float *weights);
