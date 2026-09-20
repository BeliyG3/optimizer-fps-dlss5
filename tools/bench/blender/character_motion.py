"""Sample local poses and bake shared-period routes at 30 Hz."""
import math
import json
from pathlib import Path
import bpy
import numpy as np
from mathutils import Quaternion, Vector

FPS = 30


def sample_clip(rig):
    start, end = map(round, rig.animation_data.action.frame_range)
    bones = list(rig.pose.bones)
    poses, feet = [], []
    for frame in range(start, end + 1):
        bpy.context.scene.frame_set(frame)
        poses.append([[*b.location, *b.rotation_quaternion, *b.scale] for b in bones])
        feet.append([list(rig.matrix_world @ b.head) for b in bones if b.name.endswith(('LeftFoot', 'RightFoot'))])
    poses, feet = np.array(poses), np.array(feet)
    bpy.context.scene.frame_set(start)
    # Shoulder axis is stable even when a dancer crosses or points their feet.
    left = next(b for b in rig.pose.bones if b.name.endswith('LeftArm'))
    right = next(b for b in rig.pose.bones if b.name.endswith('RightArm'))
    lateral = rig.matrix_world.to_3x3() @ (right.head-left.head)
    forward = Vector((0, 0, 1)).cross(lateral)
    facing = math.atan2(forward.y, forward.x)
    print(f'[clip] {rig.name}: {end-start} intervals, {(end-start)/FPS:.4f}s; facing {math.degrees(facing):.1f} deg', flush=True)
    hip = next(i for i, b in enumerate(bones) if b.name.endswith('Hips'))
    basis = rig.matrix_world.to_3x3() @ rig.data.bones[bones[hip].name].matrix_local.to_3x3()
    delta = basis @ Vector(poses[-1, hip, :3] - poses[0, hip, :3])
    delta.z = 0
    poses[:, hip, :3] -= np.linspace(0, 1, len(poses))[:, None] * np.array(basis.inverted() @ delta)
    return bones, poses, feet, facing


def route(t, period, name):
    x, low, high = ((.35 if name == 'hero' else -.45), -6.1, 13) if name in ('hero','olivia') else (6.8, -8, 12)
    if name in ('chad','olivia'):
        t = (t + period / 2) % period
    travel = period / 2 - 1.5
    if t < travel:
        return (x, low + (high-low)*t/travel), math.pi/2
    if t < period/2:
        return (x, high), math.pi/2 + math.pi*(t-travel)/1.5
    if t < period/2 + travel:
        return (x, high-(high-low)*(t-period/2)/travel), 3*math.pi/2
    return (x, low), 3*math.pi/2 + math.pi*(t-period/2-travel)/1.5


def write_curves(rig, rows, frames):
    rig.animation_data_clear()
    rig.keyframe_insert('location', frame=1)
    action = rig.animation_data.action
    action.name = rig.name + '_shared_loop'
    bag = action.layers[0].strips[0].channelbag(rig.animation_data.action_slot)
    for curve in list(bag.fcurves):
        bag.fcurves.remove(curve)
    for path, values in rows:
        for index in range(values.shape[1]):
            curve = bag.fcurves.new(path, index=index)
            curve.keyframe_points.add(len(frames))
            curve.keyframe_points.foreach_set('co', np.column_stack((frames, values[:, index])).ravel())
            for key in curve.keyframe_points:
                key.interpolation = 'LINEAR'
            curve.update()


def stance_speed(clip):
    feet = clip[2]
    velocities = np.linalg.norm(np.diff(feet[:, :, :2], axis=0), axis=2)*FPS
    lower = np.argmin(feet[:-1, :, 2], axis=1)
    return float(np.median(velocities[np.arange(len(lower)), lower]))


def bake(rig, clip, total_frames):
    bones, poses, feet, facing = clip
    name = rig.name.removesuffix('_armature')
    period = total_frames / FPS
    count = len(poses)-1
    walker = name in ('hero', 'olivia', 'chad')
    cycles = max(1, round(total_frames / count))
    if walker and name != 'hero':
        speed = (19.1 if name == 'olivia' else 20)/(period/2-1.5)
        cycles = max(1, round(total_frames/count * speed/stance_speed(clip)))
    phases = (np.arange(total_frames+1) * cycles * count / total_frames) % count
    a = phases.astype(int)
    f = (phases-a)[:, None, None]
    for i in range(1, len(poses)):
        flip = np.sum(poses[i-1, :, 3:7]*poses[i, :, 3:7], axis=1) < 0
        poses[i, flip, 3:7] *= -1
    values = poses[a]*(1-f) + poses[a+1]*f
    # Cross-fade clip ends: two source frames for gait, six for dances/talking.
    blend_frames = 2 if walker else 6
    blend = np.clip((phases-(count-blend_frames))/blend_frames, 0, 1)
    for i in range(len(values)):
        target = poses[0].copy()
        flip = np.sum(values[i, :, 3:7]*target[:, 3:7], axis=1) < 0
        target[flip, 3:7] *= -1
        values[i] = values[i]*(1-blend[i]) + target*blend[i]
    values[:, :, 3:7] /= np.linalg.norm(values[:, :, 3:7], axis=2)[:, :, None]
    rows = []
    for i, bone in enumerate(bones):
        bone.rotation_mode = 'QUATERNION'
        for prop, sl in (('location', slice(0, 3)), ('rotation_quaternion', slice(3, 7)), ('scale', slice(7, 10))):
            rows.append((bone.path_from_id(prop), values[:, i, sl]))
    base = rig.matrix_world.copy()
    base_q = base.to_quaternion()
    locations, rotations = [], []
    gaps = json.loads((Path(__file__).resolve().parents[1]/'assets/lab_gaps.json').read_text())
    rx,ry = gaps['right']
    lx,ly = gaps['left']
    # The protected booth plinth intrudes 35mm into the centered 0.45m footprint.
    # A 0.10m shift toward the aisle keeps both people inside the cleared gap.
    lx += .10
    fixed = {'hiphop': ((-1.6, -4.9), -0.25), 'salsa': ((1.5, 17), -math.pi/2),
             'mannequin': ((rx, ry-.5), math.pi/2), 'dummy': ((rx, ry+.5), -math.pi/2),
             'olivia_talking': ((lx, ly-.5), math.pi/2), 'chad_talking': ((lx, ly+.5), -math.pi/2)}
    for k in range(total_frames+1):
        xy, angle = route(k/FPS, period, name) if walker else fixed[name]
        locations.append((xy[0], xy[1], base.translation.z))
        q = Quaternion((0, 0, 1), angle-facing) @ base_q
        if rotations and q.dot(Quaternion(rotations[-1])) < 0:
            q.negate()
        rotations.append(tuple(q))
    rig.rotation_mode = 'QUATERNION'
    rows += [('location', np.array(locations)), ('rotation_quaternion', np.array(rotations))]
    write_curves(rig, rows, np.arange(total_frames+1))
    if walker:
        speed = (19.1 if name in ('hero','olivia') else 20)/(period/2-1.5)
        velocities = np.linalg.norm(np.diff(feet[:, :, :2], axis=0), axis=2)*FPS
        lower = np.argmin(feet[:-1, :, 2], axis=1)
        stance = float(np.median(velocities[np.arange(len(lower)), lower])) * cycles*count/total_frames
        print(f'[motion] {name}: route {speed:.3f} m/s; stance estimate {stance:.3f} m/s; slip estimate {abs(speed-stance):.3f} m/s (turns excluded)', flush=True)
    print(f'[motion] {name}: {cycles} cycles; {blend_frames/FPS:.3f}s end cross-fade', flush=True)


