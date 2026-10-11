// --wind: a CPU sway of the veg_ foliage. The per-vertex weights are computed once per node from its rest geometry
// and its material names; WindOffset is the displacement at a bench frame. Animation (animation.cpp) adds it to the
// dynamic vertices of nodes with Node::wind, at frame and frame - 1, so the previous positions are exact.
#pragma once
#include "animation_source.h"
#include <string>
#include <vector>

// Wind direction in bench space (x, z) and the sway constants (docs: RASTER.md, "Wind").
constexpr float kWindDirX = 0.8f, kWindDirZ = 0.6f;
constexpr float kWindOmega = 3.7699112f;   // 2 pi * 0.6 Hz
constexpr float kWindWave = 0.35f;         // k, per metre
constexpr float kWindAmplitude = 0.08f, kWindFlutter = 0.03f;

// True for the nodes --wind moves: a mesh node whose name starts with "veg_".
bool IsWindNode(const pwgltf::Node &node);
// Sets Node::wind on every IsWindNode node of the scene and returns how many (0: the scene is left unchanged).
size_t MarkWindNodes(pwgltf::Scene &scene);
// The tree trunk material: weight 0. "trunk" or "bark" in the name, or Poly Haven's bare "island_tree_NN".
bool IsTrunkMaterial(const std::string &material);

// Weight of one vertex at the rest position `restPos` (bench space) of `node`, from the node's rest height range:
//   trunk material  0;   node name with "grass"  1 in the upper half of the node's height, else 0;
//   otherwise (height above the node's base / node height)^2.
// A merged grass node has quads at every height: WindWeights decides top and bottom per quad instead.
float WindWeight(const pwgltf::Scene &scene, size_t node, const Vec3 &restPos, const std::string &material);
// The weight of every vertex of one primitive (indexed like Primitive::positions), as above, except that grass takes
// the top and the bottom of each connected quad (vertices linked through the triangles), wherever it stands.
std::vector<float> WindWeights(const pwgltf::Scene &scene, size_t node, size_t primitive);

// Displacement of a vertex with rest position `restPos` (bench space) at bench frame `frame` (t = frame / 60 s):
//   S * w * (0.08 sin(omega t + k p) + 0.03 sin(2.7 omega t + 1.3 k p)) along (0.8, 0, 0.6), p = dot(restPos.xz, wind).
// Exactly zero at weight 0 or strength 0.
Vec3 WindOffset(unsigned frame, Vec3 restPos, float weight, float strength);
