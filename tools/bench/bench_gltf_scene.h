// A .glb scene (--gltf) on the GPU: its base-colour textures, the point lights from its emissive
// surfaces and one dynamic vertex buffer rebuilt per frame (world positions now / previous).
#pragma once
#include "bench_d3d.h"
#include "bench_math.h"

#include "pw_gltf.h" // .glb scenes (--gltf): meshes, node animation, camera

#include <vector>

struct BenchOptions;
struct BenchDevice;

struct MeshVertex { float pos[3], prev[3], normal[3], colour[3], misc[3], uv[2]; };

struct GltfScene {
    pwgltf::Scene gscene;
    std::vector<pwgltf::Vertex> gVerts; std::vector<Vec3> gPrev; std::vector<MeshVertex> gGpu; std::vector<pwgltf::DrawRange> gRanges; std::vector<pwgltf::Light> gLights;
    Vec3 gLo{0, 0, 0}, gHi{0, 0, 0}; // world bounding box of the glTF scene: the shadow map is fitted to it
    ComPtr<ID3D11Buffer> meshVb;
    // Base-colour textures of the .glb, decoded with WIC into sRGB textures with a full mip chain.
    std::vector<ComPtr<ID3D11ShaderResourceView>> meshTextures;
    ComPtr<ID3D11ShaderResourceView> whiteTexture;
    ComPtr<ID3D11SamplerState> meshSampler;
    double skinMsTotal = 0.0; int skinFrames = 0; // time spent deforming on the CPU (printed at the end)
};

// Loads o.gltfPath (the file's camera becomes the default); false after printing the failure.
bool LoadGltf(BenchOptions &o, GltfScene &gl);
// Textures, lights, bounds and the vertex buffer; false after printing the failure.
bool CreateGltfResources(BenchDevice &d, GltfScene &gl);
// Deforms the scene at time t (morphs, CPU skinning, node animation) and uploads the vertices.
void UploadMesh(const BenchOptions &o, BenchDevice &d, GltfScene &gl, float t);
