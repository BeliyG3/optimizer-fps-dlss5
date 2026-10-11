"""Finding and reading the garden sitters' Mixamo clips (garden v2.5; garden_people_mixamo.py fits them on the bench).

A sitter looks for `<character>__sitting_idle.fbx` (a 'talk' sitter first for `<character>__sitting_talking.fbx`) in
tools/bench/assets/mixamo/out (character: olivia, chad or xbot, from the person's name; README_CHARACTERS.md says which
clips and settings). Without the file it keeps the procedural loop (garden_people_idle.py).

`load` imports the FBX, reads its bone channels from the action (fcurves evaluated on every frame: no scene evaluation)
and the feet's lowest joint heights per frame (forward kinematics of the legs from the same channels, in the imported
object's world), then removes the imported objects and data again. A foot is on the floor at a frame when its lowest
joint (ankle, toe, toe end) is within PLANT of the lowest one in the whole clip.
"""
from pathlib import Path

import bpy
import numpy as np
from mathutils import Euler, Matrix, Quaternion

import garden_people_layout as people_layout

CLIPS = {'idle': 'sitting_idle', 'talk': 'sitting_talking'}
PLANT = 0.05   # m above the clip's floor
LEG = ('UpLeg', 'Leg', 'Foot', 'ToeBase', 'Toe_End')


def default_folder():
    """tools/bench/assets/mixamo/out: next to the scene being built (as lab_characters.blend is)."""
    return Path(bpy.data.filepath).with_name('mixamo') / 'out'


def find(folder, person):
    """The clip to use for the sitter, or None (the procedural idle)."""
    character, style = person['name'].split('_')[1], person['idle'][2]
    for kind in (('talk', 'idle') if style.startswith('talk') else ('idle',)):
        path = Path(folder) / f"{character}__{CLIPS[kind]}.fbx"
        if path.is_file():
            return path
    return None


def _fcurves(obj):
    data = obj.animation_data
    if data is None or data.action is None:
        raise RuntimeError(f"{obj.name}: the FBX has no animation")
    return {(c.data_path, c.array_index): c for c in data.action.layers[0].strips[0].channelbag(data.action_slot).fcurves}


def _channel(curves, bone, prop, size, frames, default):
    out = np.empty((len(frames), size))
    for i in range(size):
        curve = curves.get((bone.path_from_id(prop), i))
        out[:, i] = [curve.evaluate(f) for f in frames] if curve else default[i]
    return out


def _rotations(curves, bone, frames):
    if any((bone.path_from_id('rotation_quaternion'), i) in curves for i in range(4)):
        rot = _channel(curves, bone, 'rotation_quaternion', 4, frames, bone.rotation_quaternion)
    else:
        euler = _channel(curves, bone, 'rotation_euler', 3, frames, bone.rotation_euler)
        order = bone.rotation_mode if len(bone.rotation_mode) == 3 else 'XYZ'
        rot = np.array([tuple(Euler(e, order).to_quaternion()) for e in euler])
    rot /= np.linalg.norm(rot, axis=1, keepdims=True)
    for i in range(1, len(rot)):
        if np.dot(rot[i], rot[i - 1]) < 0:
            rot[i] = -rot[i]
    return rot


def _feet_low(rig, clip, count):
    """(frames x 2: left, right) the lowest of each leg's ankle/toe joints, world z of the imported (static) object."""
    bones = {b.name.split(':')[-1]: b.bone for b in rig.pose.bones}
    world = rig.matrix_world
    low = np.empty((count, 2))
    for f in range(count):
        def basis(key):
            _, _, loc, rot = clip[key]
            return Matrix.Translation(loc[f]) @ Quaternion(rot[f]).to_matrix().to_4x4()
        hips = bones['Hips'].matrix_local @ basis('Hips')
        for k, side in enumerate(('Left', 'Right')):
            pose, parent, heights = hips, bones['Hips'], []
            for name in LEG:
                bone = bones[side + name]
                pose = pose @ parent.matrix_local.inverted() @ bone.matrix_local @ basis(side + name)
                parent = bone
                if name in ('Foot', 'ToeBase', 'Toe_End'):
                    heights.append((world @ pose.translation).z)
            low[f, k] = min(heights)
    return low


def load(path):
    """({bone key: (rest quaternion, rest head, locations n x 3, rotations n x 4)}, planted n x 2 bools) of the clip
    on every frame; the imported objects and data are removed again."""
    kinds = (bpy.data.objects, bpy.data.meshes, bpy.data.materials, bpy.data.images, bpy.data.armatures, bpy.data.actions)
    before = [set(kind) for kind in kinds]
    bpy.ops.import_scene.fbx(filepath=str(path))
    added = [set(kind) - old for kind, old in zip(kinds, before)]
    rigs = [o for o in added[0] if o.type == 'ARMATURE']
    if len(rigs) != 1:
        raise RuntimeError(f"{path.name}: {len(rigs)} armatures")
    rig, = rigs
    curves = _fcurves(rig)
    start, end = map(round, rig.animation_data.action.frame_range)
    frames = np.arange(start, end + 1, dtype=float)
    clip = {}
    for bone in rig.pose.bones:
        rest = bone.bone.matrix_local
        clip[bone.name.split(':')[-1]] = (rest.to_quaternion(), np.array(rest.translation),
                                          _channel(curves, bone, 'location', 3, frames, bone.location),
                                          _rotations(curves, bone, frames))
    low = _feet_low(rig, clip, len(frames))
    planted = low - low.min() < PLANT
    for kind, blocks in zip(kinds, added):
        for block in blocks:
            kind.remove(block)
    print(f"[people] {path.name}: {len(frames) - 1} intervals ({(len(frames) - 1) / people_layout.FPS:.2f} s), "
          f"{len(clip)} bones, feet on the floor {planted.mean(axis=0).round(3).tolist()} of the frames "
          f"(lowest joint {low.min():.3f} m, highest {low.max():.3f} m)", flush=True)
    return clip, planted
