// Minimal glTF 2.0 (.glb) scene loader for pw_bench: meshes (positions, indices), node hierarchy
// with TRS, translation / rotation / scale / morph-weight animations (LINEAR / STEP), skins (joints +
// inverse bind matrices, skinned on the CPU in BuildVertices), one camera node, material base colour.
// Coordinates: glTF is right-handed Y-up, the bench is D3D left-handed Y-up -> bench = (x, y, -z).
// Requires Vec3 / Mat4 from pw_bench.cpp to be declared before this header is included.
//
// Header-only, split by responsibility; include this header, not the parts:
//   pw_gltf_json.h      tiny JSON reader
//   pw_gltf_scene.h     scene model, animation sampling, world / joint matrices, camera pose, trace
//   pw_gltf_accessor.h  buffer-view / accessor reading (incl. sparse accessors)
//   pw_gltf_load.h      .glb file -> Scene
//   pw_gltf_vertices.h  morph / skin evaluation, the per-frame vertex list, mesh bounds
#pragma once
#include "pw_gltf_json.h"
#include "pw_gltf_scene.h"
#include "pw_gltf_accessor.h"
#include "pw_gltf_load.h"
#include "pw_gltf_vertices.h"
