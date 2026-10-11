# Builds lab_scene_outdoor.blend and .glb: the lab with an 8 m x 5 m opening in the hall's east wall, where the bench12
# combo camera leaves the hall (frame 883), and a sunlit yard beyond it, for the cascaded shadows and outdoor tests:
#
#   "C:\Program Files\Blender Foundation\Blender 5.2\blender.exe" --background --factory-startup --python-exit-code 1 ^
#       tools\bench\assets\lab_scene.blend --python tools\bench\blender\build_lab_outdoor.py -- --out tools\bench\assets
#
# lab_scene.blend is opened and saved under the new name before anything is exported; it and lab_scene.glb are never
# written. The .glb is export_bench_scene.py's export (the characters of lab_characters.blend appended, the tinted base
# colours composed) and is checked against lab_scene.glb: the lab's nodes first, in the same order and with the same
# triangles (except shell_walls, which has the opening), the yard's nodes after them, the animations unchanged.
# Also writes lab_scene_outdoor.opening.json: the opening, the wall faces removed, the yard's objects.
# The yard's geometry: lab_outdoor_geometry.py. The bake: bake_lab_lightmaps.py on lab_scene_outdoor.blend.
import argparse
import contextlib
import io
import json
import math
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import bpy
import numpy as np

import lab_outdoor_geometry as yard
from character_export import append_characters, validate_glb
from lab_lightmap_check import accessor, compare_geometry, read_glb
from scene_export import compose_tinted_base_colors, export_glb

NAME = "lab_scene_outdoor"
ANCHOR = (0.36, -6.03)  # bench12's camera anchor in the lab (hero_girl's bounds centre, "[info] scripted camera follows")
OPENING_WIDTH, OPENING_HEIGHT = 8.0, 5.0


def combo_eye(frame, yaw_speed=45.0, move_speed=2.0):
    """bench12 camera.h CameraAt for --camera combo with the defaults, as Blender (x, y); the eye is 1.9 m high."""
    yaw = forward = 0.0
    for _ in range(frame):
        yaw += yaw_speed / 60 * math.pi / 180 * 0.6
        forward += move_speed / 60 * 0.7
    r = forward - 5.0
    return ANCHOR[0] + r * math.sin(yaw), ANCHOR[1] + r * math.cos(yaw), math.degrees(yaw) % 360


def combo_exit(x):
    """(frame, y) where the combo camera's eye crosses the plane x (linear between frames)."""
    previous = combo_eye(0)
    for frame in range(1, 2000):
        eye = combo_eye(frame)
        if previous[0] < x <= eye[0]:
            t = (x - previous[0]) / (eye[0] - previous[0])
            return frame - 1 + t, previous[1] + t * (eye[1] - previous[1])
        previous = eye
    raise RuntimeError(f"the combo camera never crosses x = {x}")


def check_against_lab(path, lab_path, yard_names, expected_changed=(yard.WALL,), tag="outdoor", extra_people=()):
    """The outdoor export against lab_scene.glb: the lab's nodes first and unchanged except shell_walls, then the yard's
    nodes; the same skins and animations (channels and their sampled values). The garden (build_lab_garden.py) checks
    itself against lab_scene_outdoor.glb the same way, with no node changed; with extra_people (garden v2.4: the
    people's armature names) the lab's channels and skins must be there unchanged and the rest animate new nodes only."""
    doc, binary = read_glb(path)
    lab, lab_binary = read_glb(lab_path)
    count = len(lab['nodes'])
    names = [n.get('name') for n in doc['nodes']]
    if names[:count] != [n.get('name') for n in lab['nodes']]:
        raise RuntimeError("the lab's nodes are not first, in lab_scene.glb's order")
    extra = names[count:]
    if sorted(extra) != sorted(yard_names):
        raise RuntimeError(f"the nodes after the reference's are not the new ones: {extra}")
    changed = []
    for i in range(count):
        one = dict(doc, nodes=[doc['nodes'][i]])
        with contextlib.redirect_stdout(io.StringIO()):  # compare_geometry prints a line per call
            same = compare_geometry(one, binary, dict(lab, nodes=[lab['nodes'][i]]), lab_binary)
        if same['primitives_with_other_triangles'] or same['max_corner_distance'] > 1e-5 or same['material_changes']:
            changed.append(names[i])
    if changed != list(expected_changed):
        raise RuntimeError(f"lab nodes changed: {changed} (expected {list(expected_changed)})")
    animations = []
    for a, b in zip(doc.get('animations', []), lab.get('animations', [])):
        if extra_people:  # garden v2.4: the people's channels come on top of the lab's (they animate new nodes only)
            mine = {(c['target']['node'], c['target']['path']): c for c in a['channels']}
            pairs = [(mine.get((c['target']['node'], c['target']['path'])), c) for c in b['channels']]
            theirs = {(c['target']['node'], c['target']['path']) for c in b['channels']}
            animations.append(all(key[0] >= count for key in mine if key not in theirs))
        else:
            pairs = list(zip(a['channels'], b['channels']))
            animations.append(len(a['channels']) == len(b['channels']))
        for ca, cb in pairs:
            if ca is None:
                animations.append(False)
                continue
            sa, sb = a['samplers'][ca['sampler']], b['samplers'][cb['sampler']]
            animations.append(ca['target'] == cb['target'] and all(
                np.array_equal(accessor(doc, binary, sa[k]), accessor(lab, lab_binary, sb[k])) for k in ('input', 'output')))
    if len(doc.get('animations', [])) != len(lab.get('animations', [])) or not all(animations):
        raise RuntimeError("the animations differ from lab_scene.glb's")
    skins = [s.get('joints') for s in doc.get('skins', [])]
    if extra_people:  # the people's skins (joints among the new nodes only) come on top of the lab's
        new = [j for j in skins if min(j) >= count]
        skins = [j for j in skins if min(j) < count]
        if len(new) != len(extra_people) or any(max(j) >= count for j in skins):
            raise RuntimeError(f"{len(new)} new skins for {len(extra_people)} people, or a lab skin with a new joint")
    if skins != [s.get('joints') for s in lab.get('skins', [])]:
        raise RuntimeError("the skins differ from lab_scene.glb's")
    result = dict(lab_nodes=count, yard_nodes=len(extra), changed_lab_nodes=changed,
                  animations=len(doc.get('animations', [])), channels_checked=len(animations), skins=len(doc.get('skins', [])),
                  people=len(extra_people))
    print(f"[{tag}] check against {Path(lab_path).name}:", result, flush=True)
    return result


def main():
    argv = sys.argv[sys.argv.index('--') + 1:] if '--' in sys.argv else []
    parser = argparse.ArgumentParser(prog="build_lab_outdoor.py")
    parser.add_argument('--out', type=Path, help="output folder (default: the folder of lab_scene.blend)")
    args = parser.parse_args(argv)
    source = Path(bpy.data.filepath)
    if source.name != 'lab_scene.blend':
        raise SystemExit("open tools/bench/assets/lab_scene.blend (see the command above)")
    out = (args.out or source.parent).resolve()
    blend, glb = out / f"{NAME}.blend", out / f"{NAME}.glb"
    if blend.resolve() == source.resolve():
        raise SystemExit("refusing to overwrite lab_scene.blend")

    frame, exit_y = combo_exit((yard.WALL_INNER + yard.WALL_OUTER) / 2)
    y0, y1 = exit_y - OPENING_WIDTH / 2, exit_y + OPENING_WIDTH / 2
    print(f"[outdoor] combo camera crosses the east wall at frame {frame:.2f}, y {exit_y:.3f}; "
          f"eye at frame 900 {combo_eye(900)}", flush=True)
    removed = yard.cut_opening(y0, y1, OPENING_HEIGHT)
    print(f"[outdoor] opening y {y0:.3f}..{y1:.3f}, z 0..{OPENING_HEIGHT}: removed {len(removed)} wall faces "
          f"{[(tuple(round(c, 3) for c in f[0]), tuple(round(c, 2) for c in f[1]), round(f[2], 3)) for f in removed]}",
          flush=True)
    yard_names = yard.build_yard(exit_y)
    print(f"[outdoor] yard objects: {yard_names}", flush=True)

    bpy.ops.wm.save_as_mainfile(filepath=str(blend))
    print("[outdoor] saved", blend, flush=True)
    rigs = append_characters()
    compose_tinted_base_colors()
    export_glb(str(glb), rigs)
    validate_glb(str(glb), rigs)
    check = check_against_lab(glb, source.with_suffix('.glb'), yard_names)
    report = dict(exit_frame=frame, exit_y=exit_y, opening=dict(x=[yard.WALL_INNER, yard.WALL_OUTER], y=[y0, y1],
                  z=[0.0, OPENING_HEIGHT]), removed_faces=[dict(centre=c, normal=n, area=a) for c, n, a in removed],
                  yard_objects=yard_names, check=check)
    (out / f"{NAME}.opening.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"[outdoor] written {glb} ({glb.stat().st_size / 1e6:.1f} MB) and {NAME}.opening.json", flush=True)


if __name__ == "__main__":  # build_lab_garden.py imports combo_eye and check_against_lab
    main()
