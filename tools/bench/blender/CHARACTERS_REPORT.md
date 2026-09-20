# Character and hall verification report — round 2

Built with Blender 5.2 in background mode on 2026-09-19. The operator verified Blender was closed; process enumeration was skipped.

## Outputs

- `assets/lab_scene.blend`: hall changes saved after an exclusive, once-only backup; 89,690,321 bytes.
- `assets/lab_scene.before_round2.blend`: preserved original; 89,583,897 bytes.
- `assets/lab_characters.blend`: nine independent rigs, 23 meshes; 247,818,306 bytes.
- Temporary and production GLBs both passed validation: 202,605,560 bytes (193.22 MiB) each. Production was exported only after the corrected temporary export passed. The temporary GLB was deleted after final validation.
- `assets/lab_gaps.json`: right gap (2.45, -1.60), left gap (-2.45, 0.60).

## Hall changes

- Removed right row second desk `prop_metal_office_desk.003` and left row third desk `prop_metal_office_desk.004`, including all children. Selection used world geometry centres because several root origins are offset.
- Removed exactly four monitor boxes and four stand boxes by connected component and position. Surviving UV atlas coordinates and materials were retained.
- Split the multi-instance laptop root into three complete laptops, three school-chair roots into six chairs, and the floor-sign root into two signs. Mesh geometry and materials were preserved.
- Moved eight tabletop props to actual free desk space at z=0.91, with 0.25m edge/prop/monitor-strip spacing for relocated items. Kept small-prop yaw. The larger chemistry display from the removed desk was rotated to fit.
- The displaced aisle laptop was at approximately (-0.62, 2.45, bottom 0.896) in this file, rather than the approximate floor position in the spec; it was caught by the same support/footprint pass.
- Restaged 60 bay roots. Racks and cabinets sit against the outer walls; carts, lounge chair, chairs, tools, signs and other loose equipment occupy free wall stretches. Eight crate pairs are stacked two high; others stand singly. Some small items occupy separated space in front of wall fixtures.
- Both full-length bay lanes (x=±6.8, width 1.6m, y=-9..13) are clear. Furniture bounds have at least 0.4m planar separation when their height ranges overlap; stacked crates have touching support planes. Movable bay furniture is also more than 1.2m from all standing-character spots.
- Replaced `dress_cables` with three smooth, capped, welded, twelve-sided hose meshes from Bezier curves with 7/8/8 control points. Diameter 0.035m; z bounds 0..0.035m. Name and material retained. One central crossing lies near y=6; outer hoses go around the camera-side ends of bay walking routes to reach carts without crossing their centre lines.
- Fingerprints confirmed 50 protected booth, shell, light, dressing, sign, ring and large-screen objects unchanged (transforms, vertices, topology, UVs and material assignments). Only the expressly requested monitor components and cable geometry are exceptions.

### Relocated tabletop props

| Prop | Destination desk | New centre X / Y |
|---|---|---|
| `prop_chemistry_set` | `prop_metal_office_desk` | -2.081 / -4.012 |
| `prop_circuit_board` | `prop_metal_office_desk.002` | -3.170 / -1.770 |
| `prop_classic_laptop_item_2` | `prop_metal_office_desk.007` | 1.681 / 2.519 |
| `prop_chemistry_set.001` | `prop_metal_office_desk.001` | 2.048 / -4.012 |
| `prop_classic_laptop_item_3` | `prop_metal_office_desk.007` | 2.317 / 2.468 |
| `prop_small_plastic_torch` | `prop_metal_office_desk.001` | 3.038 / -4.073 |
| `prop_garden_hose_wall_mounted_01` | `prop_metal_office_desk.010` | -3.294 / 6.972 |
| `prop_Drill_01` | `prop_metal_office_desk.008` | -3.323 / 4.703 |

## Characters, animation and geometry

32 objects appended: nine rigs and 23 meshes. One animation, nine skins, 1,737 channels, 43.166667 seconds, 1,295 frame intervals at 30fps. All rigs animate simultaneously in animation 0. Maximum endpoint component error: 0.00000017; world bone matrix loop checks passed (maximum 0.00000048).

| Armature | Channels | Root channels | Joints | Triangles |
|---|---:|---:|---:|---:|
| `hero_armature` | 173 | 2 | 57 | 79,999 |
| `olivia_armature` | 197 | 2 | 65 | 52,666 |
| `chad_armature` | 197 | 2 | 65 | 57,914 |
| `hiphop_armature` | 195 | 0 | 65 | 49,112 |
| `salsa_armature` | 195 | 0 | 65 | 49,112 |
| `mannequin_armature` | 195 | 0 | 65 | 28,880 |
| `dummy_armature` | 195 | 0 | 65 | 37,668 |
| `olivia_talking_armature` | 195 | 0 | 65 | 52,666 |
| `chad_talking_armature` | 195 | 0 | 65 | 57,914 |

Total character triangles: 465,931. Heroine decimated from 130,829 to 79,999; other characters remain below 120k. Bind heights are hero 1.75m, Olivia 1.70m, Chad 1.80m, both X Bots 1.72m, Mannequin 1.82m, Dummy 1.80m.

- Heroine walks x=+0.35, y=-6.1..13. Olivia A walks x=-0.45 over the same route, starting at the far end. Minimum sampled centre separation is 0.800157m; continuous lateral separation is at least 0.80m. Chad A remains at x=+6.8, y=-8..12.
- Right-gap pair faces each other at y=-2.10 and -1.10. Left-gap pair faces each other at y=0.10 and 1.10. Both pair axes are parallel to the aisle, with 1.00m spacing.
- Necessary placement adjustment: the left pair uses x=-2.35, 0.10m toward the aisle from the gap centre. Exact centre placement put Chad B's required footprint 35mm inside the protected booth plinth. The adjusted pair stays inside the gap and clears it by 65mm.
- Salsa: full evaluated meshes checked at all 1,296 baked frames. Original minimum -4.987mm; applied +5.987mm root lift; final minimum +1.000mm. Hip hop and salsa retain their specified XY positions.

### Standing footprint clearances

Clearance is nearest obstacle distance minus the specified 0.45m radius. Positive values pass. Checks include all static mesh obstacles within body-height range, excluding floor surfaces; disconnected atlas/architectural batches are tested component by component to avoid treating empty space between objects as solid.

| Character | Position X / Y | Clearance | Nearest object |
|---|---|---:|---|
| `hiphop_armature` | -1.60 / -4.90 | 0.011m | `metal_office_desk_drawer_01` |
| `salsa_armature` | 1.50 / 17.00 | 2.111m | `metal_office_desk.008` |
| `mannequin_armature` | 2.45 / -2.10 | 0.611m | `metal_office_desk.041` |
| `dummy_armature` | 2.45 / -1.10 | 0.557m | `metal_office_desk.009` |
| `olivia_talking_armature` | -2.35 / 0.10 | 0.371m | `booth_plinth` |
| `chad_talking_armature` | -2.35 / 1.10 | 0.065m | `booth_plinth` |

## Materials and motion quality

- All twelve heroine material slots mapped by name, with no fallback or missing mappings. Original material graphs/images restored. Character textures packed and capped at 2048px. Existing dress-tint export composition retained.
- Olivia and Chad talking copies share mesh data, materials and images in the character blend after rest-geometry, vertex-group-order and skin-weight comparisons. The actual GLB duplicates their mesh payloads for independent skins but shares materials.

| Walker | Route speed | Retimed stance estimate | Cadence mismatch estimate |
|---|---:|---:|---:|
| Heroine | 0.951m/s | 0.959m/s | 0.008m/s |
| Olivia | 0.951m/s | 0.961m/s | 0.010m/s |
| Chad | 0.996m/s | 1.003m/s | 0.007m/s |

These are median lower-foot speed estimates, excluding turns; they are not measured contact-slip residuals or an IK foot-lock guarantee. Hero/Olivia/Chad use 37/31/26 cycles. Hip hop/salsa/Mannequin/Dummy/Olivia B/Chad B use 3/10/11/11/7/1 cycles. Two source frames blend walk endings; six blend dance/talk endings. Whole-cycle retiming closes the common period without hard cuts; velocity continuity is not guaranteed. Turns last 1.5 seconds with walking retained.

## Checks and limitations

- Temporary and production GLB JSON/binary checks passed: one animation, duration, zero start, increasing sample times, endpoints, unique channel targets, animated joints in all nine skins, skin attributes, triangle budgets, root channels, removal of four placeholders, and retained `CharacterAnchor`.
- All nine initial heading and loop matrix assertions passed. The lineup and hall overview PNGs were rendered and visually inspected. These workbench previews check geometry/staging, not the fully textured bench renderer.
- Hall idempotence passed: rerunning the repair left the saved blend hash unchanged. Original backup hash matches round 1. Protected-object and full-length lane checks passed again after the final cable correction.
- Cable topology, diameter/floor envelope, and route clearance checks passed. Exported mesh/material sharing was inspected in the temporary GLB JSON.
- Python syntax checks passed for all 20 scripts in the Blender folder. No C++ compilation or bench runtime test was run. Neither git nor `tools/bench12` was touched.
- Other starting talking-pose floors remain slightly below zero: Mannequin about 0.9mm, Dummy 0.1mm, Olivia B 0.5mm, Chad B 0.1mm. Full-loop floor correction was required and performed for salsa only.
- Hip hop has only 11mm spare clearance beyond its prescribed footprint. These footprint checks do not prove animated hands/body volumes never touch surrounding furniture.
- Blender emitted unsupported FBX `Short` custom-property warnings; validation passed. Workbench rendering printed `Unable to delete file` after successfully saving the previews.

## Files and reproduction

Created `fix_lab_scene_round2.py`, `lab_geometry.py`, `lab_staging.py`, `character_round2.py`, `verify_lab_round2.py`, `verify_lab_protected.py`, `verify_round2_resources.py`, and `preview_lab_round2.py`. Updated `build_characters.py`, `character_assets.py`, `character_motion.py`, `character_export.py`, `verify_character_poses.py`, `README_CHARACTERS.md`, and this report. The existing `export_bench_scene.py` orchestrator required no change.

Responsibilities and exact commands are in `README_CHARACTERS.md`. Machine-readable changes/checks and logs are under `assets/round2_*` and `assets/characters_round2_*`. All per-node channel counts appear in the export logs.

Source SHA-256: `E7D6F0A83D08852CCB2A3E7371796ACAF5A6D9CDBE0B7C80C4E804CABB40FD4E`.

Backup SHA-256: `EF84DD3C0E89C8BDDEFC6E150275AABE497BEE22F3B6CEF778C2AF6B43BA5E0B`.

## Complete bay move list

| Object | Final centre X / Y / Z | Stack level |
|---|---|---:|
| `prop_drawer_cabinet` | -9.106 / -11.229 / 0.941 | 1 |
| `prop_drawer_cabinet.001` | -9.106 / -9.679 / 0.941 | 1 |
| `prop_drawer_cabinet.002` | 9.106 / -11.229 / 0.941 | 1 |
| `prop_worn_metal_rack.002` | -9.050 / -8.242 / 0.950 | 1 |
| `prop_worn_metal_rack.003` | -9.050 / -6.892 / 0.950 | 1 |
| `prop_worn_metal_rack.004` | -9.050 / -5.542 / 0.950 | 1 |
| `prop_worn_metal_rack.008` | -9.050 / -4.192 / 0.950 | 1 |
| `prop_worn_metal_rack.009` | 9.050 / -9.792 / 0.950 | 1 |
| `prop_worn_metal_rack.010` | 9.050 / -8.442 / 0.950 | 1 |
| `prop_worn_metal_rack.014` | 9.050 / -7.092 / 0.950 | 1 |
| `prop_worn_metal_rack.006` | -9.050 / -2.842 / 0.950 | 1 |
| `prop_worn_metal_rack.012` | 9.050 / -5.742 / 0.950 | 1 |
| `prop_worn_metal_rack.001` | -9.050 / -1.492 / 0.950 | 1 |
| `prop_worn_metal_rack.007` | -9.050 / -0.142 / 0.950 | 1 |
| `prop_worn_metal_rack.013` | 9.050 / -4.392 / 0.950 | 1 |
| `prop_worn_metal_rack` | -9.050 / 1.208 / 0.950 | 1 |
| `prop_worn_metal_rack.005` | -9.050 / 2.558 / 0.950 | 1 |
| `prop_worn_metal_rack.011` | 9.050 / -3.042 / 0.950 | 1 |
| `prop_rollershutter_door` | 8.160 / -10.260 / 1.200 | 1 |
| `prop_modular_electric_cables.001` | -8.278 / -10.762 / 0.379 | 1 |
| `prop_modular_electric_cables` | 8.141 / -7.262 / 0.379 | 1 |
| `prop_modular_electric_cables.002` | 8.141 / -4.762 / 0.379 | 1 |
| `prop_old_military_crate` | -8.860 / 4.358 / 0.150 | 1 |
| `prop_old_military_crate.001` | -8.860 / 4.358 / 0.451 | 2 |
| `prop_industrial_storage_cart.001` | -8.793 / 6.505 / 0.689 | 1 |
| `prop_industrial_storage_cart` | 8.793 / -1.345 / 0.689 | 1 |
| `prop_modular_industrial_pipes_01.002` | -8.162 / -8.599 / 0.977 | 1 |
| `prop_tool_cart.001` | 8.973 / 0.537 / 0.482 | 1 |
| `prop_tool_cart` | -8.973 / 8.387 / 0.482 | 1 |
| `prop_mid_century_lounge_chair` | -8.755 / 9.954 / 0.585 | 1 |
| `prop_SchoolChair_01.001_item_1` | 8.910 / 2.035 / 0.503 | 1 |
| `prop_SchoolChair_01.002_item_1` | -8.910 / 11.335 / 0.503 | 1 |
| `prop_SchoolChair_01_item_1` | -8.910 / 12.635 / 0.503 | 1 |
| `prop_portable_generator` | 7.902 / 0.309 / 0.288 | 1 |
| `prop_SchoolChair_01.002_item_2` | -8.029 / -7.092 / 0.503 | 1 |
| `prop_SchoolChair_01.001_item_2` | 8.029 / -2.942 / 0.503 | 1 |
| `prop_SchoolChair_01_item_2` | -8.029 / -5.942 / 0.503 | 1 |
| `prop_metal_tool_chest` | -8.064 / -4.807 / 0.326 | 1 |
| `prop_metal_tool_chest.001` | -8.064 / -3.707 / 0.326 | 1 |
| `prop_metal_tool_chest.002` | 7.824 / 1.493 / 0.326 | 1 |
| `prop_plastic_crate_02.012` | 7.823 / 2.503 / 0.127 | 1 |
| `prop_plastic_crate_02.006` | 9.147 / 3.153 / 0.127 | 1 |
| `prop_plastic_crate_02.007` | 9.147 / 3.153 / 0.381 | 2 |
| `prop_plastic_crate_02.010` | -8.064 / -2.697 / 0.127 | 1 |
| `prop_plastic_crate_02.011` | -8.064 / -2.697 / 0.381 | 2 |
| `prop_plastic_crate_02.002` | -8.064 / -1.747 / 0.127 | 1 |
| `prop_plastic_crate_02.003` | -8.064 / -1.747 / 0.381 | 2 |
| `prop_plastic_crate_02` | 8.304 / 3.453 / 0.127 | 1 |
| `prop_plastic_crate_02.001` | 8.304 / 3.453 / 0.381 | 2 |
| `prop_plastic_crate_02.004` | -8.064 / -0.797 / 0.127 | 1 |
| `prop_plastic_crate_02.005` | -8.064 / -0.797 / 0.381 | 2 |
| `prop_plastic_crate_02.008` | -8.064 / 0.153 / 0.127 | 1 |
| `prop_plastic_crate_02.009` | -8.064 / 0.153 / 0.381 | 2 |
| `prop_WetFloorSign_01_item_2` | -8.160 / 1.030 / 0.315 | 1 |
| `prop_WetFloorSign_01_item_1` | 9.171 / 4.000 / 0.315 | 1 |
| `prop_plastic_crate_01` | 8.305 / 4.299 / 0.132 | 1 |
| `prop_plastic_crate_01.001` | 8.305 / 4.299 / 0.396 | 2 |
| `prop_korean_fire_extinguisher_01` | -8.051 / 1.790 / 0.330 | 1 |
| `prop_korean_fire_extinguisher_01.001` | 9.166 / 4.740 / 0.330 | 1 |
| `prop_sledgehammer_01` | -8.847 / 13.604 / 0.045 | 1 |
