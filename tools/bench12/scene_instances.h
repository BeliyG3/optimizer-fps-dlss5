#pragma once
// Instanced meshes: a glTF mesh that two or more static nodes share is stored once, in its object space (bench coordinates),
// after the flattened triangles of the scene buffers; each node becomes an instance with its world transform. Raster draws
// such a mesh once per pass for its visible instances (raster_instances.h), the path tracer builds one BLAS per mesh and a
// TLAS instance per node (accel.cpp). Everything else stays flattened into world space as before: skinned, animated and
// wind-swayed nodes (characters skin per node, on the GPU by default, into their own part of the dynamic range), emissive meshes (the
// lamps' light lists hold world-space triangles), meshes with morph targets, single-use meshes (no saving, a draw call per
// pass each), and every mesh with --lightmap (its TEXCOORD_1 is per node). --no-instancing flattens everything.
#include "animation_source.h"
#include "foliage_class.h"
#include <string>
#include <utility>
#include <vector>

// One instance as the shaders read it (shaders/scene.hlsli SceneInstance, t4): the rows of its world and previous-frame
// transforms (object space to world, bench coordinates), its mesh's first triangle in the scene buffers and how many of the
// mesh's triangles are opaque (they come first, as the BLAS's first geometry).
struct GpuInstance { float world[3][4]; float previous[3][4]; unsigned firstTriangle, opaqueTriangles, padding[2]; };
static_assert(sizeof(GpuInstance)==112);

struct InstanceGroup { int mesh=-1; std::vector<size_t> nodes; };
struct InstancePlan {
    std::vector<bool> instanced; // per node: drawn through a group, so not flattened
    std::vector<InstanceGroup> groups; // in the order of their first node
};
// enabled false (--no-instancing, --lightmap): no groups. A node qualifies when it is static (pwgltf::DynamicNode false) and
// unskinned, and its mesh has no morph target and no emissive primitive. Qualifying nodes are grouped by mesh and by the
// per-node inputs of their vertices (the ground flag, the palette colour); groups of two or more are instanced.
InstancePlan PlanInstances(const pwgltf::Scene &scene, bool enabled);

// A mesh stored once: its triangles [firstTriangle, firstTriangle+triangles) in the scene buffers, the opaque ones first,
// and its instances [firstInstance, firstInstance+instances) in Scene::instances.
struct InstancedMesh { unsigned firstTriangle=0, triangles=0, opaqueTriangles=0, firstInstance=0, instances=0; };
struct InstanceBox { Vec3 lo, hi; }; // an instance's world-space bounds
// The groups of a plan in object space, ready for the scene buffers: 3 vertices per triangle (pwgltf::Vertex, bench
// coordinates), each triangle's roughness and foliage class (the group's first node names it), mesh after mesh with its
// opaque triangles first; the instances with their transforms and exact world bounds (every vertex transformed as
// BuildVertices would), and the bounds of them all.
struct InstancedGeometry {
    std::vector<pwgltf::Vertex> vertices;
    std::vector<float> roughness;
    std::vector<VegClass> vegClass;
    std::vector<InstancedMesh> meshes;
    std::vector<GpuInstance> instances;
    std::vector<InstanceBox> boxes;
    InstanceBox bounds{};
};
// firstTriangle: the scene buffers' triangle index of the first instanced triangle (the flattened triangles' count).
InstancedGeometry BuildInstancedGeometry(const pwgltf::Scene &scene, const InstancePlan &plan, unsigned firstTriangle);
// The rows of a glTF world matrix in bench coordinates: ToBench(TransformPoint(world, p)) == rows * ToBench(p).
void BenchRows(const Mat4 &world, float rows[3][4]);
// The per-triangle inputs of the flattened nodes (the plan's non-instanced mesh nodes), in node and primitive order as
// BuildVerticesSubset emits them: roughness, material name, dynamic (pwgltf::DynamicNode), --wind, the index among the
// dynamic triangles (0 for static ones) and the foliage class.
struct FlatTriangles {
    std::vector<float> roughness;
    std::vector<std::string> materialNames;
    std::vector<bool> dynamic, wind;
    std::vector<unsigned> dynamicIndex;
    std::vector<VegClass> vegClass;
};
FlatTriangles CollectFlatTriangles(const pwgltf::Scene &scene, const InstancePlan &plan);
