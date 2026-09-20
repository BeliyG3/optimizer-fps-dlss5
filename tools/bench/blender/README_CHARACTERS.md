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

