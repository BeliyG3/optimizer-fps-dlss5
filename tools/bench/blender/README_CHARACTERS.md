# Animated lab characters — round 2

Requires Blender 5.2 at `C:\Program Files\Blender Foundation\Blender 5.2\blender.exe`,
the local FBX inputs in `CHARACTERS_SPEC.md` and `CHARACTERS_SPEC2.md`, and the
existing `assets/lab_scene.blend`. Scripts use Blender's bundled Python and NumPy.
No installation or compilation is required. Close interactive Blender before
running the hall repair. The operator verified that prerequisite for this run.

Run these PowerShell commands from the repository root:

```powershell
$blender = 'C:\Program Files\Blender Foundation\Blender 5.2\blender.exe'
& $blender --background --factory-startup tools/bench/assets/lab_scene.blend --python tools/bench/blender/fix_lab_scene_round2.py
& $blender --background --factory-startup --python tools/bench/blender/build_characters.py
& $blender --background --factory-startup tools/bench/assets/lab_scene.blend --python tools/bench/blender/verify_lab_round2.py
& $blender --background --factory-startup tools/bench/assets/lab_characters.blend --python tools/bench/blender/verify_character_poses.py
& $blender --background --factory-startup tools/bench/assets/lab_scene.before_round2.blend --python tools/bench/blender/verify_lab_protected.py
& $blender --background --factory-startup tools/bench/assets/lab_scene.blend --python tools/bench/blender/verify_lab_protected.py
& $blender --background --factory-startup tools/bench/assets/lab_scene.blend --python tools/bench/blender/export_bench_scene.py -- tools/bench/assets/lab_scene_chars_test.glb
& $blender --background --factory-startup tools/bench/assets/lab_scene.blend --python tools/bench/blender/verify_round2_resources.py
# Only after the temporary export and checks pass:
& $blender --background --factory-startup tools/bench/assets/lab_scene.blend --python tools/bench/blender/export_bench_scene.py -- tools/bench/assets/lab_scene.glb
# Only after the production export passes:
Remove-Item -LiteralPath tools/bench/assets/lab_scene_chars_test.glb
```

Blender can return success after a Python exception: check logs for tracebacks,
all nine final `[verify]` rig summaries, and `[round2 verify] PASSED`.
`fix_lab_scene_round2.py` exclusively creates `lab_scene.before_round2.blend` if
absent, then edits and saves the source scene. A completed run is a no-op on rerun.
It restores `lab_gaps.json` from the scene's recorded coordinates if necessary.
For deliberate restaging from the original backup, append `-- --rebuild` to its
command; this discards later hall changes, but never overwrites the backup.
The builder saves only `lab_characters.blend`. Export and verification never save
either blend file. The exporter retains its previous behavior when the character
blend is absent.

## Script responsibilities

- `fix_lab_scene_round2.py`: backup, hall repair orchestration, smooth cable mesh,
  gap coordinates, and the detailed `assets/round2_hall_report.json` change list.
- `lab_geometry.py`: world bounds, connected mesh components, box removal with
  atlas UV preservation, and splitting multi-instance library roots.
- `lab_staging.py`: tabletop free-space placement and bay furniture packing.
  Racks/cabinets use outer wall positions; smaller equipment can occupy separated
  space in front. Eight crate pairs stack at most two high.
- `build_characters.py`: nine-character build orchestration and common period.
- `character_assets.py`: FBX import, rest-height normalization, heroine material
  restoration, triangle budgets, and packed textures capped at 2048px.
- `character_motion.py`: pose sampling, gait cadence, shared-period routes,
  gap-based talking positions, interpolation, and 30fps baking.
- `character_round2.py`: safe mesh/material sharing after geometry, vertex-group,
  and weight comparisons; exhaustive salsa mesh-floor sampling and root lift.
- `character_export.py`: append rigs in memory, remove four placeholders, validate
  GLB animation endpoints, skins, triangles, channels, and anchor retention.
- `export_bench_scene.py`: existing material composition and SCENE animation export.
- `verify_lab_round2.py`: full-length bay lane checks, furniture spacing,
  stationary footprint clearances, and walker separation.
- `verify_lab_protected.py`: before/after fingerprints of protected transforms,
  geometry, topology, UVs, and material assignments.
- `verify_round2_resources.py`: twelve-sided hose topology, floor/route clearance,
  and actual GLB mesh/material sharing. Requires the temporary GLB.
- `verify_character_poses.py`: loop matrices, initial headings/floor diagnostics,
  and `assets/characters_preview.png` lineup (geometry, not textured rendering).
- `preview_lab_round2.py`: optional `assets/round2_staging_preview.png` overview;
  hides overhead architecture in memory only.

The heroine and Olivia use x=+0.35 and -0.45, respectively, on the same -6.1..13m
route, half a period apart. Chad walks x=+6.8, y=-8..12. All loops last
43.166667 seconds (1,295 intervals at 30fps). Walks blend over two source frames;
dances and talks over six. Whole-cycle retiming avoids a hard cut at the period.
Walking continues during the 1.5-second turns. Foot-slip estimates describe
cadence mismatch, not measured contact sliding or IK foot locking.

The left talking pair shifts 0.10m toward the aisle within its gap because the
protected phone-booth plinth would intersect Chad's centered 0.45m footprint.
The two people remain 1m apart, parallel to the aisle. Salsa is lifted 5.987mm
based on the minimum evaluated mesh vertex across all 1,296 baked frames.

Olivia/Chad copies share mesh data, materials, and images in the character blend.
The exporter duplicates mesh payloads for their separate skins but reuses
materials. The loader in `pw_gltf.h` merges animations; the single SCENE animation
also satisfies an animation-0-only player. No git or `tools/bench12` work is needed.


## Garden people (garden v2.4)

`build_lab_garden.py` adds 17 copies (v2.5b; 16 in v2.4) of these characters outdoors (`garden_people_layout.py`: who stands or walks where
and the clearance checks; `garden_people.py`: the copies and their loops; `garden_people_seat.py`: the seated pose). A
copy is a duplicate rig `people_<character>_<role>_<k>_armature` with mesh objects sharing the source's mesh data; its
bones replay the source's 43.166667 s loop shifted by a few frames, its root follows its own line or spot. Walkers keep
the lab walkers' cadence, so their lines are speed x (period / (2 x round trips) - 1.5 s) long. The X Bot sitter uses
the salsa rig. At export, `character_export.append_characters` points the copies at the appended characters' mesh
data (`garden_people.use_lab_meshes`) and `glb_share_skins.py` then lets every copy's mesh node use the lab character's
glTF mesh and inverse bind matrices (checked per triangle corner), so the glb holds no extra character meshes.

### Sitters: seated loops (garden v2.5, v2.5b), Mixamo sitting clips

The four sitters loop a seated animation (`garden_people_sit_motion.py`). With the Mixamo clips in
`tools/bench/assets/mixamo/out/` (as now) they play those (`garden_people_mixamo_clip.py` reads a clip,
`garden_people_mixamo.py` fits it on the bench, `garden_people_contact.py` measures the contacts):

| sitter | where | clip used | Mixamo animation |
| --- | --- | --- | --- |
| Olivia (`people_olivia_sit_0`) | double bench 1 | `olivia__sitting_idle.fbx`, 6.2 s loop x7, phase 0 s | Sitting Idle ("Sitting In Chair Hands Resting On Thighs"), Ch11 |
| X Bot (`people_xbot_sit_2`) | single bench 0 | `xbot__sitting_idle.fbx`, 6.2 s loop x7, phase 6.1 s | Sitting Idle, X Bot |
| Chad (`people_chad_sit_1`) | double bench 0, x -1.85, turned 25 degrees to his left | `chad__sitting_talking.fbx`, 43.2 s loop x1, phase 3.7 s | Sitting Talking ("Sitting And Talking To Another Person", legs crossed), Ch16 |
| Olivia (`people_olivia_sit_3`) | double bench 0, x -0.45, turned 25 degrees to her right | `olivia__sitting_talking.fbx` mirrored, phase 11 s | the same, left and right swapped |

Chad and the second Olivia talk to each other (v2.5b): the talking clip looks about 65 degrees to the talker's left,
so Chad sits on the right and turns left, and Olivia plays the clip mirrored (she looks right, her left leg crossed).
`chad__sitting_idle.fbx` and `xbot__sitting_talking.fbx` are there too; which kind a sitter takes is its style in
`garden_people_layout.PEOPLE` ('talk' / 'talk_mirrored' look for `__sitting_talking.fbx` first, the others for
`__sitting_idle.fbx`). Download settings for more clips: Format **FBX Binary (.fbx)**, Skin **With Skin**, Frames per
Second **30**, Keyframe Reduction **none**, **In Place** if offered, sliders at their defaults; name them
`<character>__sitting_idle.fbx` / `<character>__sitting_talking.fbx` (character: olivia, chad or xbot). Then rebuild
the garden (`build_lab_garden.py`, `bake_lab_lightmaps.py --no-bake` for the baked glb).

What the build does with a clip: it maps the clip's bones onto the sitter's rig by name (`mixamorig:*`), carries the
rotations over relative to the rest pose (any armature axis or unit; mirrored on request), cross-fades the clip's last
15 frames into its first ones and plays it a whole number of times per period (retimed by under about 10 % for clips up
to 9 s), shifted by the sitter's phase. It turns the clip so the sitter's hips face the bench's front plus its yaw,
puts the hips over the seat and then forward until both knees are 7 cm in front of the seat's front edge (the shins
hang in front of the seat), keeps the pelvis on the seat frame by frame (root height), and lowers each planted foot
onto the ground by pitching its shin towards the vertical, then its thigh (the foot back by as much). A foot more than
5 cm above the clip's floor is lifted (the crossed leg): it is not planted and takes the planted thigh's pitch, so it
stays on the other knee. While a sloping thigh or a calf cuts the seat's front edge deeper than 1 cm, the sitter is
fitted again further forward (v2.5b; Olivia sits 26 cm further forward than at first, on the front half of the seat:
the bench is higher than the Mixamo chair). Every sampled frame is checked on the skinned mesh (v2.5c, `garden_people_contact.py`):
the lowest buttock or thigh underside over the seat (vertices following the hips, thighs or spine whose normals point
down, each against the seat surface under it by a ray from 5 cm above the seat; v2.5b's rays from 0.3 m hit the arm
rest beside Chad and lifted him), the buttocks alone (the same within 15 cm of the hips joint along the seat's depth,
so a thigh on the seat's edge cannot hold a floating pelvis) and each planted foot's sole (vertices whose normals point
down) against the ground must touch, within -1..+0.5 cm; the back must not go into the bench (only the torso against
the back rest), a lifted foot not into the ground, and no body part more than 1 cm inside the bench (the seat tested as
a 3.5 cm slab, the rest by the nearest face); the walkers' bodies stay at least 0.3 m from the sitters' knees and feet
and the talking partners' joints 18 cm apart (their crossed feet). The build fails otherwise. The log prints `[people] ... Mixamo <file>`, the contact ranges and the
depth per body part; `lab_scene_garden.layout.json` keeps them under `loop`. `build_lab_garden.py -- --mixamo
<folder>` reads the clips from another folder.

Without the clips the sitters loop a procedural idle (`garden_people_idle.py`) on top of the static seated pose
(`garden_people_seat.py`): breathing (about 4 s a breath), head turns of 20-28 degrees held a few seconds, small
forearm and wrist movements on the thighs; Chad gestures with his right hand as when talking. The pelvis and legs are
not animated. Loops: 10.8 s (4 per 43.2 s period), X Bot 8.6 s (5), phases 0/3.7/6.1/11 s; the mirrored partner
gestures with the other hand and looks the other way.
