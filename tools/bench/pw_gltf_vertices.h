// Per-frame geometry from a Scene: morph + CPU skinning, the world-space vertex list (current and previous frame),
// draw ranges, emissive lights and mesh bounds. Part of pw_gltf.h; include that header.
// Requires Vec3 / Mat4 (Identity, Mul, Normalize, Vec3 operators) to be declared before it is included.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>

#include "pw_gltf_scene.h"

namespace pwgltf {

// Morph + skin one primitive into `pos` / `nrm`, per unique vertex, still in mesh-local space
// (glTF order: morph targets first, skinning after).  `jm` = joint matrices of the node's skin.
inline void DeformPrimitive(const Primitive &prim, const std::vector<float> &w, const std::vector<Mat4> &jm,
                            std::vector<Vec3> &pos, std::vector<Vec3> &nrm)
{
    const size_t n = prim.positions.size();
    pos.resize(n); nrm.resize(n);
    const bool morph = !prim.targets.empty() && !w.empty();
    const bool skin = !prim.joints.empty() && !jm.empty();
    for (size_t v = 0; v < n; ++v) {
        Vec3 p = prim.positions[v];
        Vec3 q = v < prim.normals.size() ? prim.normals[v] : Vec3{0, 1, 0};
        if (morph) for (size_t k = 0; k < prim.targets.size() && k < w.size(); ++k) {
            if (w[k] == 0.0f || v >= prim.targets[k].size()) continue;
            const Vec3 &d = prim.targets[k][v];
            p.x += d.x * w[k]; p.y += d.y * w[k]; p.z += d.z * w[k];
        }
        if (skin) {
            Vec3 sp{0, 0, 0}, sn{0, 0, 0}; float total = 0.0f;
            for (int k = 0; k < 4; ++k) {
                const float wk = prim.skinWeights[v * 4 + k];
                if (wk == 0.0f) continue;
                const size_t j = prim.joints[v * 4 + k];
                if (j >= jm.size()) continue;
                sp = sp + TransformPoint(jm[j], p) * wk;
                sn = sn + TransformDir(jm[j], q) * wk;
                total += wk;
            }
            if (total > 0.0f) { p = sp * (1.0f / total); q = Normalize(sn); }
        }
        pos[v] = p; nrm[v] = q;
    }
}

// One vertex of `prim` (node `n`, mesh `mesh`) with the given bench-space position and normal: its UVs, textures, material
// category and colour. BuildVerticesSubset passes the world-space position and normal; bench12's instanced meshes the
// object-space ones. `meshY`: the vertex's height in mesh space (localY).
inline Vertex PrimitiveVertex(const Node &n, const Mesh &mesh, const Primitive &prim, uint32_t index, bool emissive, Vec3 pos, Vec3 normal,
                              float meshY)
{
    Vertex v{};
    if (index * 2 + 1 < prim.uvs.size()) { v.uv[0] = prim.uvs[index * 2]; v.uv[1] = prim.uvs[index * 2 + 1]; }
    if (index * 2 + 1 < prim.uvs2.size()) { v.uv2[0] = prim.uvs2[index * 2]; v.uv2[1] = prim.uvs2[index * 2 + 1]; v.lightmapAtlas = std::max(mesh.lightmapAtlas, -1); }
    v.texture = prim.texture; v.alphaMask = prim.alphaMask; v.alphaCutoff = prim.alphaCutoff;
    v.metallic = prim.metallic; v.normalScale = prim.normalScale;
    v.metallicRoughnessTexture = prim.metallicRoughnessTexture; v.normalTexture = prim.normalTexture;
    v.pos = pos; v.normal = normal;
    // -1 ground, -2 plain, -4 emissive (colour = radiance), -5 glossy (roughness under 0.3: a polished floor)
    v.material = n.isGround ? -1.0f : (emissive ? -4.0f : (prim.roughness < 0.3f ? -5.0f : -2.0f));
    v.localY = (meshY - mesh.minY) * n.scale.y;
    v.stripe = prim.stripes ? 1.0f : 0.0f;
    if (!prim.hasColour && n.pwColour >= 0) memcpy(v.colour, kPalette[n.pwColour % 6], sizeof(v.colour));
    else memcpy(v.colour, prim.colour, sizeof(v.colour));
    if (emissive) memcpy(v.colour, prim.emission, sizeof(v.colour));
    return v;
}
// Builds the vertex list for one instant: world positions at time t and t - dt (previous frame),
// already converted to the bench's coordinate system.  Morph targets and CPU skinning are applied
// per unique vertex, then expanded through the index buffer.
struct PrimitiveRef { size_t node, primitive; };
inline bool DynamicNode(const Scene &scene, size_t index)
{
    if (scene.nodes[index].skin >= 0 || scene.nodes[index].wind) return true;
    for (const Channel &c : scene.channels) if (c.node == (int) index && c.path == 3) return true;
    for (int ancestor = (int) index; ancestor >= 0; ancestor = scene.nodes[ancestor].parent)
        for (const Channel &c : scene.channels) if (c.node == ancestor && c.path >= 0 && c.path < 3) return true;
    return false;
}
inline std::vector<PrimitiveRef> Primitives(const Scene &scene, int dynamic = -1)
{
    std::vector<PrimitiveRef> result;
    for (size_t i = 0; i < scene.nodes.size(); ++i) {
        const Node &n = scene.nodes[i];
        if (n.mesh < 0 || (size_t) n.mesh >= scene.meshes.size()) continue;
        if (dynamic >= 0 && DynamicNode(scene, i) != (dynamic != 0)) continue;
        for (size_t p = 0; p < scene.meshes[n.mesh].primitives.size(); ++p) result.push_back({i, p});
    }
    return result;
}
// Caller-owned scratch permits allocation-free repeated subset evaluation on independent workers.
struct VertexWorkspace {
    std::vector<Mat4> worlds, worldsPrev, local, jm, jmPrev;
    std::vector<int> stack;
    std::vector<Vec3> dp, dn, dpPrev, dnPrev;
    std::vector<float> wNow, wPrev;
    void Reserve(const Scene &scene, const std::vector<PrimitiveRef> &selection)
    {
        const size_t nodes = scene.nodes.size();
        worlds.reserve(nodes); worldsPrev.reserve(nodes); local.reserve(nodes); stack.reserve(nodes);
        size_t vertices = 0, weights = 0, joints = 0;
        for (const auto &ref : selection) {
            const auto &n = scene.nodes[ref.node];
            vertices = std::max(vertices, scene.meshes[n.mesh].primitives[ref.primitive].positions.size());
            weights = std::max(weights, n.weights.size());
            if (n.skin >= 0 && (size_t) n.skin < scene.skins.size()) joints = std::max(joints, scene.skins[n.skin].joints.size());
        }
        for (const auto &c : scene.channels) if (c.path == 3) weights = std::max(weights, (size_t) c.sampler.components);
        dp.reserve(vertices); dn.reserve(vertices); dpPrev.reserve(vertices); dnPrev.reserve(vertices);
        wNow.reserve(weights); wPrev.reserve(weights); jm.reserve(joints); jmPrev.reserve(joints);
    }
};
inline void BuildVerticesSubset(const Scene &scene, float t, float dt, const std::vector<PrimitiveRef> &selection,
    VertexWorkspace &scratch, std::vector<Vertex> &out, std::vector<Vec3> &prevOut,
    std::vector<DrawRange> *ranges = nullptr, std::vector<Light> *lights = nullptr,
    float previousTime = std::numeric_limits<float>::quiet_NaN())
{
    Trace("build vertices");
    out.clear(); prevOut.clear();
    if (ranges) ranges->clear();
    const float previous = std::isnan(previousTime) ? t - dt : previousTime;
    auto &worlds = scratch.worlds; auto &worldsPrev = scratch.worldsPrev;
    auto &jm = scratch.jm; auto &jmPrev = scratch.jmPrev;
    auto &dp = scratch.dp; auto &dn = scratch.dn; auto &dpPrev = scratch.dpPrev; auto &dnPrev = scratch.dnPrev;
    auto &wNow = scratch.wNow; auto &wPrev = scratch.wPrev;
    scene.WorldsAt(t, worlds, scratch.local, scratch.stack); scene.WorldsAt(previous, worldsPrev, scratch.local, scratch.stack);
    for (const auto &ref : selection) {
        const size_t i = ref.node;
        const Node &n = scene.nodes[i];
        if (n.mesh < 0 || (size_t) n.mesh >= scene.meshes.size()) continue;
        const Mat4 world = worlds[i], worldPrev = worldsPrev[i];
        const Mesh &mesh = scene.meshes[n.mesh];
        scene.WeightsAt((int) i, t, wNow); scene.WeightsAt((int) i, previous, wPrev);
        scene.JointMatrices(worlds, (int) i, jm); scene.JointMatrices(worldsPrev, (int) i, jmPrev);
        static const bool traceNodes = getenv("PW_GLTF_TRACE") != nullptr;
        if (traceNodes) { fprintf(stderr, "[gltf] node %zu mesh %d prims %zu weights %zu joints %zu\n", i, n.mesh, mesh.primitives.size(), wNow.size(), jm.size()); fflush(stderr); }
        { const Primitive &prim = mesh.primitives[ref.primitive];
            if (ranges) {
                // Consecutive primitives with the same texture and cut-out flag share one range.
                if (ranges->empty() || ranges->back().texture != prim.texture || ranges->back().alphaMask != prim.alphaMask) {
                    DrawRange r; r.start = (uint32_t) out.size(); r.texture = prim.texture; r.alphaMask = prim.alphaMask; ranges->push_back(r);
                }
            }
            const bool deformed = (!prim.targets.empty() && !wNow.empty()) || (!prim.joints.empty() && !jm.empty());
            DeformPrimitive(prim, wNow, jm, dp, dn);
            if (deformed) DeformPrimitive(prim, wPrev, jmPrev, dpPrev, dnPrev);
            const std::vector<Vec3> &pp = deformed ? dpPrev : dp;
            const bool emissive = prim.emission[0] + prim.emission[1] + prim.emission[2] > 0.0f;
            if (emissive && lights) {
                // One light per emissive primitive: area-weighted centre, radiance x area.
                Vec3 centre{0, 0, 0}; float area = 0.0f;
                for (size_t k = 0; k + 2 < prim.indices.size(); k += 3) {
                    const uint32_t ia = prim.indices[k], ib = prim.indices[k + 1], ic = prim.indices[k + 2];
                    if (ia >= dp.size() || ib >= dp.size() || ic >= dp.size()) continue;
                    const Vec3 a = ToBench(TransformPoint(world, dp[ia])), b = ToBench(TransformPoint(world, dp[ib])), c = ToBench(TransformPoint(world, dp[ic]));
                    const Vec3 e1 = b - a, e2 = c - a;
                    const Vec3 cr{e1.y * e2.z - e1.z * e2.y, e1.z * e2.x - e1.x * e2.z, e1.x * e2.y - e1.y * e2.x};
                    const float tri = 0.5f * std::sqrt(cr.x * cr.x + cr.y * cr.y + cr.z * cr.z);
                    centre = centre + (a + b + c) * (tri / 3.0f); area += tri;
                }
                if (area > 0.0f) { Light l; l.pos = centre * (1.0f / area); for (int k = 0; k < 3; ++k) l.power[k] = prim.emission[k] * area; lights->push_back(l); }
            }
            for (uint32_t index : prim.indices) {
                if (index >= prim.positions.size()) continue;
                Vertex v = PrimitiveVertex(n, mesh, prim, index, emissive, ToBench(TransformPoint(world, dp[index])),
                                           Normalize(ToBench(TransformDir(world, dn[index]))), dp[index].y);
                out.push_back(v);
                prevOut.push_back(ToBench(TransformPoint(worldPrev, pp[index])));
                if (ranges) ++ranges->back().count;
            }
        }
    }
}

inline void BuildVertices(const Scene &scene, float t, float dt, std::vector<Vertex> &out, std::vector<Vec3> &prevOut,
    std::vector<DrawRange> *ranges = nullptr, std::vector<Light> *lights = nullptr)
{
    VertexWorkspace scratch;
    BuildVerticesSubset(scene, t, dt, Primitives(scene), scratch, out, prevOut, ranges, lights);
}

// World-space bounds (bench coordinates) of the meshes in the subtree of `anchor` at time t,
// deformation included -- what the face camera frames.  Returns false if the subtree has no mesh.
inline bool MeshBounds(const Scene &scene, float t, int anchor, Vec3 &lo, Vec3 &hi)
{
    std::vector<Mat4> worlds, jm; scene.WorldsAt(t, worlds);
    std::vector<Vec3> dp, dn; std::vector<float> w;
    lo = {1e30f, 1e30f, 1e30f}; hi = {-1e30f, -1e30f, -1e30f};
    bool any = false;
    for (size_t i = 0; i < scene.nodes.size(); ++i) {
        const Node &n = scene.nodes[i];
        if (n.mesh < 0 || (size_t) n.mesh >= scene.meshes.size()) continue;
        bool under = anchor < 0 || (int) i == anchor;
        for (int p = n.parent; p >= 0 && !under; p = scene.nodes[p].parent) under = p == anchor;
        if (!under) continue;
        scene.WeightsAt((int) i, t, w); scene.JointMatrices(worlds, (int) i, jm);
        for (const Primitive &prim : scene.meshes[n.mesh].primitives) {
            DeformPrimitive(prim, w, jm, dp, dn);
            for (const Vec3 &p : dp) {
                const Vec3 q = ToBench(TransformPoint(worlds[i], p));
                lo.x = std::min(lo.x, q.x); lo.y = std::min(lo.y, q.y); lo.z = std::min(lo.z, q.z);
                hi.x = std::max(hi.x, q.x); hi.y = std::max(hi.y, q.y); hi.z = std::max(hi.z, q.z);
                any = true;
            }
        }
    }
    return any;
}

} // namespace pwgltf
