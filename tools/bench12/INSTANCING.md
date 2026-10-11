# Instanced meshes

A glTF mesh that two or more static nodes share is stored once, in its object space, and drawn and traced per node with
the node's transform. Before, the loader flattened every node into world space (3 vertices per triangle), so the garden's
151 props on 27 meshes cost 1.01 M triangles in the vertex and material buffers, in the static BLAS and in every raster
pass. On by default; `--no-instancing` flattens everything as before (same images, the comparison and fallback switch).

## Which meshes

`PlanInstances` (`scene_instances.h/.cpp`) groups the mesh nodes that qualify by mesh, ground flag and palette colour (the
per-node inputs of their vertices); a group of two or more is instanced. A node qualifies when it is static
(`pwgltf::DynamicNode` false: no skin, no animated node or ancestor, not a `--wind` `veg_` node) and its mesh has no morph
target and no emissive primitive. Everything else stays flattened in world space:

- **Characters and other skinned or animated nodes:** they are skinned per node every frame (on the GPU, see "GPU skinning"
  below; `--cpu-skinning`: `Animation`'s CPU workers) into their own part of the dynamic range, refitted in the one dynamic BLAS. Copies of a character that share a mesh with their
  own skins therefore already share the source mesh (the glTF data the CPU skins from) and the textures; each copy needs its
  own skinned output for raster and the BLAS anyway. Checked with a test scene that duplicates four lab rigs twice
  (a local script, not in the repo: new joint nodes and skin, the same meshes, retargeted channels).
- **Emissive meshes:** the lamp light lists, tubes and spot shadows read world-space triangles.
- **Single-use meshes:** nothing to save, and one draw per mesh in every pass instead of one draw for all.
- **`--lightmap`:** no instancing at all, a mesh's TEXCOORD_1 (its lightmap UVs) belongs to one node.

## Storage

- **Scene buffers** (`scene.cpp`): the flattened triangles `[0, flatTriangles)` as before (static, then dynamic), then each
  instanced mesh's triangles once in object space (bench coordinates, opaque ones first, `prevPos` = `pos`), with their
  materials. `Scene::instancedMeshes` holds each mesh's range and instances, `Scene::instances` the instances
  (`GpuInstance`: world and previous-frame rows, first triangle, opaque count), `Scene::instanceBoxes` their exact world
  bounds (every vertex transformed as the flattening would, so the sun shadow fit and the probe grid see the same bounds).
- **The scene table** (all roots): t0 vertices, t1 materials, t2 lights, t3 environment, **t4 the instances**
  (`SceneInstance` in `shaders/scene.hlsli`), the textures from t5.
- **Shaders:** `WorldVertex` / `WorldPosition` / `TrianglePosition` take the instance (`NoInstance` for flattened
  triangles) and apply its rows; an instance's normal is the world transform's linear part applied and normalised, as the
  flattening did. `GetSurface`, `Accept`, the prepass, the probe and path-trace lamp normals use them, so position, normal,
  tangent frame and motion (previous rows; equal to the current ones for these static nodes) match the flattened ones.

## Raster

`raster_instances.h/.cpp`: every pass that draws the scene (depth/normal AO prepass, tube depth prepass, scene pass, single
sun map, the four cascades, the lamp spot maps) draws the flattened range as before, then one `DrawInstanced` per mesh for
its instances inside the pass's view. The culling is on the CPU against the four side planes of the pass's view-projection
(`BoxOutside`; the camera's with four pixels of margin for the jitter): exact (no instance that reaches a pixel is dropped).
The visible instance indices of all passes go into one per-frame upload buffer that the input assembler reads per instance
(`uint instance : INSTANCE`, `InstanceInput` in `raster_pipeline.cpp`; the graphics roots allow the input assembler), so
`StartInstanceLocation` picks a draw's run. Entry 0 is `NoInstance`, the flattened draws' entry, so the other draws (the
baked mode's dynamic and list passes) read it unchanged. The vertex shader adds the mesh's first vertex itself
(`StoredVertex`): `SV_VertexID` does not include `StartVertexLocation` on this driver.

## Path tracing and probes

`accel.cpp`: one BLAS per instanced mesh (opaque geometry, then the alpha-tested one, as the static BLAS), and a TLAS of the
static BLAS (InstanceID 0), the dynamic BLAS (1) and each instance on its mesh's BLAS with its world rows (InstanceID
2 + instance; `TopInstances` in `accel_instances.h`). `TriangleIndex` and `SceneInstanceOf` in `shaders/scene.hlsli` turn a
hit back into the stored triangle and its instance. The probes trace a TLAS of the same instances without the dynamic BLAS
(`Accel::StaticInstances`), so the props still shadow and block the probes. The log prints the BLAS sizes (`[accel]`), the
instancing summary and the scene buffer sizes (`[info]`).

## Checks

`tests_instances.cpp`: the grouping (shared mesh -> one group of its static nodes; skinned, animated, emissive, ground and
single nodes stay flattened; none with the switch off), the object-space geometry (opaque first; rows times object
position and normal equal the flattened vertices; exact bounds), the TLAS instances (static, dynamic, 2 + i with the
instance rows; the probes' list without the dynamic one) and the culling planes.

## GPU skinning

The dynamic range (characters, rigidly animated nodes, `--wind` `veg_` nodes) is skinned by a compute pass (`gpu_skinning.h/.cpp`,
`shaders/skin.hlsl`); `--cpu-skinning` keeps `Animation`'s CPU workers and the per-frame upload as before (the comparison switch).

- **Tables** (`skin_layout.h/.cpp`, built once): every dynamic primitive of every node (`pwgltf::Primitives(scene, 1)`, the CPU
  path's order) is an item. Its source vertices (glTF object-space position and normal, four palette entries and weights) are
  stored once per mesh primitive and skin size, so copies of a character share them (garden v2.4: 1.65 M item vertices,
  0.48 M of them shared); morph deltas once per primitive; each item vertex lists the corners of the dynamic range it is
  written to, numbered exactly as `Animation::Start` numbers them (the scene's `dynamicOrder`), and the UVs are written into
  the vertex buffer once at load.
- **Per frame** (CPU, `GpuSkinning::Evaluate`): the poses only (`AnimationPose`, now with its channels indexed per node: the
  scan of every channel for every node cost ~6 ms a frame with the people), then each node's palette uploaded for this frame
  and the previous one: entry 0 = ToBench x world, entry 1 + j = ToBench x world x joint j (the joint in the mesh basis that
  `DeformVertex` applies), plus the morph weights. The previous palette is the last frame's current one when time steps on
  (as `Animation`'s reuse), else evaluated at the previous time. Wind: the two sines of the frame (and frame - 1) are
  computed in double on the CPU; the shader adds each vertex's own phase by the angle-sum identity.
- **Pass** (`GpuSkinning::Record`, before the BLAS work in `Pathtrace::Render`): one thread per item vertex blends its
  palette entries (weights normalised by their sum, as the CPU path), adds the morph deltas and the wind sway at both frames,
  and stores position, previous position and normal into every corner (a `RWByteAddressBuffer` view of the scene vertex
  buffer, created with UAV access). The transition back to `NON_PIXEL_SHADER_RESOURCE` orders it before the dynamic BLAS
  refit and every reader; nothing downstream changed.
- **Checks:** `tests_skinning.cpp` runs the shader's math in C++ (`skin_math.h`) against `Animation` on a scene with two
  copies of a skinned, morphed mesh (joints past the skin, weightless vertices), a rigidly animated node and a wind node:
  positions, previous positions and normals within 2e-5 (largest 3.6e-7 m); UVs equal; the indexed poses equal
  `pwgltf::Scene::WorldsAt` bit for bit. Bench dumps GPU vs CPU: see task-5b-report.md.
- **Timing** (the per-stage report: `[gpu stage]` lines from `Device::Mark`, `[info] cpu stages` from the main loop): the CPU
  path cost 16-17 ms of CPU skinning plus a 22 ms GPU copy of the 260 MB upload (5.9 M corners x 44 bytes over PCIe), in series
  because every frame waits for its fence; the GPU pass takes 1.6-1.9 ms and the CPU 0.7 ms for the poses.

## Culling small objects

Raster only (path tracing and the static probes see every instance). `raster_cull.h/.cpp`, `raster_cull_passes.cpp`,
`raster_occlusion.h/.cpp`, `raster_occlusion_check.cpp`, `shaders/raster_occlusion.hlsl`.

- **Small:** an instance whose world bounding sphere (around its exact world box) has a radius below `--cull-small-radius`
  (0.75 m). The hall, buildings, ground and trees are far larger; flattened geometry (static and dynamic) is never culled.
  Garden v2.4: 661 of 761 instances are small.
- **Wide cone (every pass):** each frame a small instance is dropped from all raster passes (AO/depth prepass, tube prepass,
  scene pass, single sun map or the four cascades, the lamp spot maps) when its sphere lies wholly outside a cone of
  `--cull-cone-deg` (170) full angle around the camera's view direction (`SphereOutsideCone`: the angle to the axis minus the
  sphere's angular radius beyond the half angle; a sphere around the eye always stays), or farther than
  `--cull-small-distance` (0 = no limit). Objects just outside the 62 degree view stay in, so they keep casting shadows into
  it. The shadow passes keep their own frustum culling on top.
- **Hi-Z occlusion (camera passes only):** the camera's list keeps the small instances apart. The first camera pass of the
  frame (the AO prepass with `--ao`, else the tube prepass, else the scene pass) draws the occluders first: the flattened
  triangles (static and dynamic: this frame's depth, so moving people occlude correctly) and the large instances. Then, in the
  middle of that pass, `RasterOcclusion::Record` builds a Hi-Z from the depth buffer (level 0 = 2x2 pixels per texel, the
  farthest depth, padded to powers of two; every level halves) and a compute pass tests each small candidate's world box: the
  eight corners projected with the camera (any corner in front of the near plane: visible), the pixel rectangle grown by one
  pixel for the jitter, the Hi-Z level where it spans at most 2x2 texels; occluded when the box's nearest depth is farther
  than the farthest occluder depth there. Survivors are appended per mesh to a visible list and their `DrawInstanced`
  arguments; every camera pass draws them with one `ExecuteIndirect`. The shadow passes never use it (an object behind a
  wall can still cast a visible shadow). This is the current frame's depth, not the previous frame's: no reprojection, no
  latency, so nothing can pop on a turn or a camera cut; it is exactly conservative for the depth test.
- **Switches:** `--no-small-culling` (neither), `--no-occlusion-culling` (the cone only), `--cull-check` (everything is drawn;
  after the scene pass an occlusion query per Hi-Z-culled candidate, drawn with the scene pass's vertex shader against the
  finished depth with depth EQUAL, counts its visible samples; any culled-but-visible instance is printed as
  `[cull-check] culled but visible: instance ...`), `--cull-debug` (the culled ones drawn over the scene pass with a
  multiplying red blend and no depth test: their silhouettes tint the pixels in front of them). The run ends with
  `[cull] per frame: ...` (cone-dropped draws per pass kind) and `[cull] occlusion: ...` (candidates, occluded).
- **Checks:** `tests_culling.cpp`: the cone (inside, straddling, behind, around the eye, distance, 360 degrees; 2000 random
  spheres declared outside have no sampled point inside the cone) and the Hi-Z test (`hiz_math.h`, the shader's math with
  the sizes rounded up instead of padded): 18 000 random boxes against random walls with holes and nearer blocks in both
  depth conventions, every occluded one checked pixel by pixel. Bench (garden v2.4, 1080p, render scale 1, six views
  including a snap turn): dumps with and without culling are bit-identical, and `--cull-check` found 0 culled-but-visible
  instances in 26 000 culled candidates (task-5b-report.md).
