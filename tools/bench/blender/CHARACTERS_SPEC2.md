# Lab scene, round 2: hall fixes + new character staging

Read `CHARACTERS_SPEC.md`, `CHARACTERS_REPORT.md`, `README_CHARACTERS.md`, `build_characters.py`, `export_bench_scene.py` first.
Blender is NOT running now, so this time `tools/bench/assets/lab_scene.blend` MAY be modified - by a background script only,
after copying it to `lab_scene.before_round2.blend` (once; do not overwrite an existing backup). Work through
`blender.exe --background --factory-startup file.blend --python script.py`. Do not touch git or tools/bench12.

## A. Hall fixes: new script `tools/bench/blender/fix_lab_scene_round2.py` (idempotent; opens, edits, saves lab_scene.blend)
Facts about the file: collection `LabLayout`; desks are EMPTY roots named `prop_metal_office_desk*` with mesh children
(one desk each, scaled 1.15, table top at z = 0.91), standing in two rows at x = -2.45 and x = +2.45, rotated so the drawers face
-Y (the camera side); rows every 2.2 m along Y starting near y = -3.8. Monitors are NOT separate objects: all screens of a side
are boxes inside the meshes `lamp_screens_left` / `lamp_screens_right` (6 quads per box, UVs select a tile of an atlas), their
stands are boxes inside `screen_stands`; two monitors per desk at desk centre + (+-0.45, +0.34, z 1.13 + 0.12), stands at
(+-0.45, +0.36, z 0.98). Small props are EMPTY roots `prop_*` with mesh children (Poly Haven models; some assets contain
several objects side by side, e.g. the school chair asset is a row of chairs, the wet floor sign asset several signs).
1. Remove ONE desk per row to make room for two people talking: in the RIGHT row (x = +2.45) the second desk from the camera
   (second smallest y), in the LEFT row (x = -2.45) the third desk from the camera. Remove the desk root with its children, the two
   monitor boxes and the two stand boxes that belong to it (delete those connected components from the three meshes; find them
   by position), and move whatever small props stood on that desk to a neighbouring desk top.
   Print the centres of the two gaps: the character script needs them (write them to `assets/lab_gaps.json`).
2. Props left in the air or on the floor by earlier rearrangements: for every small `prop_*` root (bounding box under 1 m in
   every direction) whose bottom is neither on the floor (z < 0.03) nor on a desk top (|z - 0.91| < 0.03 above a desk's
   footprint), put it on the nearest desk top with free space (keep 0.25 m from the desk's edges and from other props and from
   the monitors' strip at the back edge), keeping its yaw. A laptop lying on the floor in the aisle near (-1.0, 0.5) is the
   known case; the same pass must catch the rest. List what was moved.
3. The side bays (4.8 < |x| < 9.5, under the mezzanines) are a pile: carts, racks, the lounge chair, crates and the chair rows
   intersect each other. Re-stage them along the OUTER walls (|x| about 8.6..9.3, facing the aisle), one after another along Y
   with 0.4 m gaps, no intersections (use world bounding boxes), racks and the drawer cabinet against the wall, carts and the
   lounge chair in front of free wall stretches, crates stacked at most two high next to racks; keep clear a 1.6 m wide walking
   lane in each bay at |x| = 6.8 for the full length y = -9..13, and keep clear a 1.2 m circle around the character spots
   listed in part B. Do not move the red phone booth (`booth_*`, `lamp_booth_sign`, `dress_hazard`) or anything named
   `shell_*`, `lamp_*`, `dress_*`, `sign_*`, `ring_*`, `screens_big*`.
4. The yellow floor cable: `dress_cables` is a crude polyline of boxes. Replace its geometry with smooth round cables: 2-3
   curves (bezier/NURBS through 6-10 control points each, 0.035 m diameter, converted to mesh, 12-sided, smooth shaded),
   lying on the floor (z = radius), wandering from a desk leg across the aisle edge to a cart, like a yellow hose; keep the
   object's name and material, keep clear of the walking routes' centre lines by 0.3 m except for ONE crossing of the central
   aisle around y = 6 (characters step over it, as in the reference game).
Save lab_scene.blend. Print a summary of every change.

## B. Characters: update `build_characters.py` (same rules as round 1: own file lab_characters.blend, one common loop period)
New inputs in `assets/mixamo/out/`: `olivia__talking.fbx` (woman, hand on hip, talking), `chad__talking.fbx` (man talking).
Cast (9 animated characters now):
- hero (as before): walks the central aisle from y = -6.1 to the rotunda and back, on the line x = +0.35.
- Olivia A: walks the SAME central aisle the opposite way on the line x = -0.45 (they pass shoulder to shoulder, never closer
  than 0.7 m centre to centre), starting at the far end, so she walks towards the camera first and passes the heroine
  roughly in the middle of the hall; turn, walk back. Same period.
- Chad A: keeps walking the right bay lane (x = +6.8) as in round 1.
- Mannequin + Dummy: standing and talking in the RIGHT row's gap (from lab_gaps.json), 1.0 m apart, facing each other, the pair's
  axis parallel to the aisle (one at y - 0.5, one at y + 0.5), both visible from the camera behind the heroine.
- Olivia B (talking clip) + Chad B (talking clip): the same in the LEFT row's gap. Separate armature + mesh copies of Olivia and
  Chad (share mesh data and materials/images between the two copies of a character to keep the glb small if the exporter
  allows it; otherwise plain copies).
- X Bot hip hop near the camera at (-1.6, -4.9) and X Bot salsa in the rotunda at (1.5, 17.0) as before; lift the salsa clip so
  no foot goes below the floor (round 1 reported -4.4 mm).
Nobody may stand inside furniture: check every standing character's 0.45 m radius footprint against the world bounding boxes of
the static scene's objects after part A and report the clearances.
Then run the exporter exactly as in round 1 (test glb first, then the real `lab_scene.glb`), and report as in round 1, plus
the list of part A changes. Delete the test glb at the end.
