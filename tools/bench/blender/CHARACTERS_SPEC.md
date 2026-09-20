# Animated characters for the lab scene (task for a scripted, background Blender run)

Blender 5.2: `C:\Program Files\Blender Foundation\Blender 5.2\blender.exe`. Run it only as
`blender.exe --background --factory-startup [file.blend] --python script.py -- args`. NEVER modify or save
`tools/bench/assets/lab_scene.blend`: the user has it open in a live Blender session. Read it, never write it.

## Inputs (not tracked by git, already on disk)
`tools/bench/assets/mixamo/out/*.fbx`, Mixamo exports (FBX binary, with skin, 30 fps, armature + mesh + one action):
- `heroine__female_walk.fbx`, `heroine__idle.fbx`: OUR heroine (mesh `girl_complete_03` of lab_scene.blend, 1.75 m, exported
  without textures and auto-rigged by Mixamo). Her materials and textures must come back from lab_scene.blend: same
  material slot order and names (the FBX keeps slot names; map by name, fall back to slot index), images packed or referenced
  from the original material datablocks (append the materials from lab_scene.blend).
- `olivia__female_walk.fbx` (woman in a white protective suit), `chad__walking.fbx` (man in scrubs): walk cycles IN PLACE.
- `xbot__hip_hop_dancing.fbx`, `xbot__salsa_dancing.fbx`: the same X Bot character, two dances.
- `mannequin__talking.fbx`, `dummy__talking.fbx`: two men standing and talking (different clips).

## Output
1. `tools/bench/blender/build_characters.py`: builds `tools/bench/assets/lab_characters.blend` from the FBX files, idempotent
   (rebuilds the file from scratch each run). Scene units metres, Z up, characters scaled to real height
   (heroine 1.75 m; others: keep Mixamo's proportions, normalise each to 1.65..1.85 m by its bind-pose height: Olivia 1.70,
   Chad 1.80, X Bot 1.72, Mannequin 1.82, Dummy 1.80).
2. `tools/bench/blender/export_bench_scene.py` (exists; extend, keep current behaviour when lab_characters.blend is absent):
   before exporting the glb, APPEND (not link) everything from `lab_characters.blend` into the scene opened from
   lab_scene.blend IN MEMORY ONLY, remove the static placeholders it replaces (objects `girl_complete_03`, `dummy_figure`,
   `npc_bench_left`, `npc_bench_right` of lab_scene.blend; keep the empty `CharacterAnchor`), export with animations
   (`export_animations=True`, skins, one merged animation or NLA tracks such that ALL characters animate simultaneously in the
   single glTF animation the bench plays: the bench's loader (`tools/bench/pw_gltf.h`) plays animation 0 only; check how it
   reads animations and make the export match: if needed bake every character's action into one action per armature and
   export with `export_animation_mode='SCENE'` or merge so that animation 0 drives every armature; verify by parsing the
   resulting glb JSON in the script and printing channel counts per node).
   The exporter must not write lab_scene.blend.

## Layout in the hall (Blender coordinates: X across, Y along the hall, Z up; the heroine's static placeholder stands at
(0, -6.1) facing +Y; central aisle between two rows of desks: desk rows at x = +-2.45 +-1.2, y from -3.8 to 14; free aisle
|x| < 1.1; side bays under the mezzanines at 4.8 < |x| < 9.5; the rotunda begins at y = 15 and ends at y = 21)
- Heroine (node name must contain "hero": name the armature `hero_armature`, mesh `hero_girl`): walks along the aisle from
  y = -6.1 to y = +13 at the natural speed of the walk cycle (measure the cycle's stride from the feet's motion, or use
  1.35 m/s), turns around (a 1.5 s turn in place: simply rotate the root while the walk cycle keeps playing), walks back, turns:
  a seamless loop. Root motion is a baked object animation of the armature; foot sliding must be small (match speed to cycle).
- Olivia: walks a loop in the LEFT bay (x = -6.8), y from -8 to +12 and back, same rules. Chad: the same in the RIGHT bay
  (x = +6.8), starting at the far end so the two are out of phase.
- X Bot hip hop: near the camera, at (2.1, -3.0)? NO - that is inside the desk rows. Put her at (-1.6, -4.9) facing the
  aisle (towards +X, slightly towards the camera at -Y), i.e. 1.2 m ahead-left of the heroine's start, large in the frame.
- X Bot salsa (second instance, own armature + mesh copy): in the rotunda at (1.5, 17.0) facing -Y (towards the camera).
- Mannequin and Dummy: facing each other 1.1 m apart in the right bay near the aisle end, at (5.6, -2.0) and (6.7, -2.0).
- All loops must share ONE common period so the glTF animation loops seamlessly: choose the period T as the heroine's full
  route time rounded to a whole number of her walk cycles; retime (scale) the other walkers' routes to T; repeat dance and
  talking clips to fill T, cross-fading or simply cutting at T if the clip length does not divide T (say which in a comment).
  Bake everything at 30 fps.
- Poly budget: print triangle counts per character; if one exceeds 120k triangles, add a Decimate modifier (collapse) to reach
  ~80k and apply it before export. Textures: keep as they come (the bench decodes them), but cap at 2048 px (resize larger
  images in the characters .blend).

## Verification you must do and report
- Run build_characters.py and the exporter to a TEMPORARY glb (`tools/bench/assets/lab_scene_chars_test.glb`), not over
  `lab_scene.glb`; print: objects appended, armatures, animation count and duration, channels per armature, triangle counts,
  file size. Then, only if everything is consistent, also write the real `tools/bench/assets/lab_scene.glb`.
- Compile nothing in tools/bench12; do not touch it. Do not touch git.
- Report anything that looked wrong (materials that did not map, feet sliding estimate, clips that do not loop).
