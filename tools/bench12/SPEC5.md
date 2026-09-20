# pw_bench12, milestone 5: animated characters (skinned glTF), correct motion vectors for them

Read `SPEC.md`..`SPEC4.md`, `README.md` and the sources first. Same rules (module layout, files under ~300 lines, README and
tests updated, /W4 /WX). `../bench/pw_gltf.h` may be extended backwards-compatibly (the D3D11 bench must keep building).

Today the scene is frozen at time zero and lives in one static BLAS. The lab scene will contain several skinned characters
with baked animations (walk cycles with a root path through the hall, dances in place), exported from Blender as glTF
(skins, TRS animation channels, possibly morph targets). `pw_gltf.h` already evaluates them on the CPU:
`BuildVertices(scene, t, dt, vertices, previousPositions, ...)` returns positions at `t` and at `t - dt`.

## What to build
- Split the triangle list into STATIC and DYNAMIC at load: a primitive is dynamic when its node has a skin, morph weights
  animation, or any animated ancestor (TRS channel). Everything else is static and keeps today's path (one BLAS, built once).
  Add to `pw_gltf.h` what is needed to evaluate only the dynamic subset each frame without rebuilding the static 1.6 M
  triangles (e.g. a node filter / a per-primitive "dynamic" flag and a BuildVertices variant restricted to it). The static
  call must produce exactly today's output.
- Dynamic geometry: its own vertex buffer region (pos, prevPos, normal, uv), rewritten every frame from the CPU evaluation at
  `time` and `time - frameDelta`; its own BLAS (`ALLOW_UPDATE | PREFER_FAST_BUILD`), refit every frame and fully rebuilt every
  N frames (default 60) or when the refit's quality would degrade (large motion: keep it simple, a counter is fine); a TLAS
  with two instances (static, dynamic), rebuilt every frame (it is tiny). Opaque and alpha-masked geometries as today.
  The shader finds a triangle's data from InstanceID/GeometryIndex/PrimitiveIndex: keep ONE vertex/material buffer with the
  dynamic triangles placed after the static ones and pass the base offset, so the rest of the shading code is unchanged.
- Motion vectors of dynamic triangles come from prevPos (interpolated with the barycentrics, as already done): verify the
  path; static triangles keep prevPos = pos. Emissive dynamic triangles are not lamps (ignore them as emitters).
- Time: `--anim-speed S` (default 1), `--anim-start T`, animation loops over the longest channel; scripted runs advance by
  exactly 1/60 s per frame (deterministic), interactive runs by the real frame time (clamped to 1/15 s); a menu section
  "Animation": play/pause, speed, time scrubber, "step one frame", number of dynamic triangles, CPU ms spent on skinning,
  BLAS refit ms. Pausing must give zero motion vectors on the characters and lets accumulation converge.
- CPU cost: skin on worker threads (split the dynamic primitives across `hardware_concurrency` threads), reuse buffers, no
  per-frame allocations. Upload through a persistently mapped upload buffer + copy, or write directly into an upload-heap
  buffer the shader reads (pick one, say why).
- Camera: `--camera follow` and a menu toggle "Follow character": third-person camera behind the node named by
  `--follow-node NAME` (default: first node whose name contains "hero" or "girl"), following its animated world position and
  heading with a little smoothing, same distance/height as the static third-person view. Free flight still works when off.
- Accumulation resets while anything moves (animation playing), as it does for camera motion.

## Verification you must leave
CPU tests: static/dynamic split of a small synthetic glTF (one skinned quad with a two-key animation + one static mesh), the
restricted BuildVertices equals the full one on the dynamic subset, prevPos at `t - dt` differs where expected and equals pos
when paused, time looping. The real lab scene has no animation yet: it must load and render exactly as before (dynamic set
empty: no dynamic BLAS, TLAS with one instance). List what needs a GPU to verify.
