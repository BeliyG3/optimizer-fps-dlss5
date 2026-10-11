# Bench assets

Not in git. The procedural scene needs nothing; the glTF scenes are local files:

- `bench_scene.glb` / `bench_scene.blend` — the author's Blender scene (export with `tools/bench/blender/export_bench_scene.py`), used via `pw_bench.exe --gltf assets/bench_scene.glb`.
- `lab_scene_baked.glb`, `lab_lightmap_full_0.hdr`, `lab_lightmap_sun_0.hdr`, `lab_lightmap.json` — the lab with its lighting baked into lightmaps for pw_bench12's raster mode (`tools/bench/blender/bake_lab_lightmaps.py`, see `tools/bench12/RASTER_BAKED.md`).
- `lab_scene_outdoor.blend` / `lab_scene_outdoor.glb` — the lab with an 8 m x 5 m opening in the east wall and a sunlit outdoor yard (`tools/bench/blender/build_lab_outdoor.py`, see `tools/bench12/RASTER_BAKED.md`); the `.glb` is the path-traced and dynamic-raster scene.
- `lab_scene_outdoor_baked.glb` — the outdoor lab's baked export (lightmap UVs and `extras.lightmapAtlas`), for `--lightmap`.
- `lab_outdoor_lightmap_{full,sun,rest}_0.hdr`, `lab_outdoor_lightmap_*_noisy.hdr` — the outdoor lab's lightmap atlases (8192 x 8192, Radiance) and the bakes before the denoise.
- `lab_outdoor_lightmap.json` — the outdoor lightmap's atlas list, sun, sky and `scaleFull`/`scaleSun`; used as `--lightmap`.
- `polyhaven/` — Poly Haven assets (CC0): `island_tree_02` and `fern_02` (glTF 2k), `autumn_field_puresky_4k.hdr`
  (downloaded by `polyhaven/fetch.py`), and the textures `grass_ground` and `grass_path_2` (2k JPEG maps), and the garden
  v2.2 props (glTF 2k): `modular_street_seating`,
  `wooden_picnic_table`, `outdoor_table_chair_set_01`, `rock_moss_set_01`, `rock_moss_set_02`, `boulder_01`,
  `dead_tree_trunk`, `dry_branches_medium_01`, `potted_plant_02`, `Barrel_01`, `Barrel_02`, `metal_jerrycan_green`,
  `wooden_bucket_01`, `hand_truck`, `compost_bags`, `cement_bag`, `garden_gloves_01`, `old_military_crate`; read only.
- `autumn_field_puresky_nosun.hdr` / `.json` — the HDRI with its sun disc removed, aligned to the bench sun and scaled (`tools/bench/hdri_prepare.py`); the bench's `--hdri` for the garden.
- `lab_scene_garden.blend` / `lab_scene_garden.glb` / `lab_scene_garden.layout.json` — the outdoor lab with a garden:
  a path along the combo camera's route, card grass, trees and ferns (`veg_*` nodes, moved by `--wind`) and props
  (`prop_*` nodes: lit dynamically, not baked; v2.4: 270 nodes sharing 27 meshes, the last 119 in the distant yard) and
  17 people (`people_*`: copies of the lab's Olivia, Chad and X Bot walking, talking, dancing and sitting; skinned, so
  animated and never baked; each copy's mesh nodes share the lab character's glTF mesh)
  (`tools/bench/blender/build_lab_garden.py`, see `tools/bench12/RASTER_BAKED.md`). The build needs
  `lab_characters.blend` next to `lab_scene_outdoor.blend`; `lab_scene_garden.blend` keeps the people's own copy of the
  character data (about 140 MB of it). v2.5: the sitters loop Mixamo seated clips from `mixamo/out/`
  (`*__sitting_idle.fbx` / `*__sitting_talking.fbx`; a procedural idle when they are missing) (`tools/bench/blender/README_CHARACTERS.md`, "Sitters").
- `garden_grass_atlas.png` — the grass blade atlas the garden build renders.
- `garden_ground_{diff,nor}.jpg`, `garden_path_{diff,nor}.jpg` — the soil's and the path's textures the garden build composes from `grass_ground` and `grass_path_2` (also packed into `lab_scene_garden.blend`).
- `garden_props/` — the props' textures scaled to at most 1024 (JPEG) by the garden build (`tools/bench/blender/garden_props.py`; also packed into `lab_scene_garden.blend`).
- `lab_scene_garden_baked.glb`, `lab_garden_lightmap_{full,sun,lamp,rest}_0.hdr` (and `*_noisy.hdr`), `lab_garden_lightmap.json` — the garden's bake under the HDRI sky (foliage not baked), for `--lightmap` (and `--lamp-shadows`).
- `external/` — third-party models (for example the `cutegirl` head from Sketchfab, CC BY-NC-SA) used by `run_cg.sh`; set `BENCH_GLTF` to point at your copy.

`pw_bench.exe --scene 3d` runs without any of these.
