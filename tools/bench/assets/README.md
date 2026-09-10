# Bench assets

Not in git. The procedural scene needs nothing; the glTF scenes are local files:

- `bench_scene.glb` / `bench_scene.blend` — the author's Blender scene (export with `tools/bench/blender/export_bench_scene.py`), used via `pw_bench.exe --gltf assets/bench_scene.glb`.
- `external/` — third-party models (for example the `cutegirl` head from Sketchfab, CC BY-NC-SA) used by `run_cg.sh`; set `BENCH_GLTF` to point at your copy.

`pw_bench.exe --scene 3d` runs without any of these.
