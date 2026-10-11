"""A procedural seated idle for a garden sitter (garden v2.5), on top of its static seated pose (garden_people_seat.py).

The pelvis, the legs and the feet keep the static pose (their channels stay constant, so nothing on the seat or the
ground slides); the upper body moves:
- breathing: the spine and chest pitch back and forth (Spine/Spine1/Spine2 0.3/0.6/0.9 degrees) and the shoulders rise a
  little, about 4 s a breath; the upper arms and the neck take the inverse, so the hands stay on the thighs and the
  head does not bob;
- looking around: the head turns (yaw split neck 40 % / head 60 %) to a side and holds a few seconds, eased
  (smoothstep), then back; a small pitch with it;
- hands: the forearms lift off the thighs by up to 3 degrees and slide a little, the hands tap (wrist lift);
- 'talk' ('talk_mirrored': the other hand, looks to the other side): one arm gestures, as when telling something: the forearm comes up off the thigh (to 50 degrees), the upper
  arm with it, the hand turns and beats in time with small head nods; it rests on the thigh between two gestures.
A sitter's loop is PERIOD / loops long (8.6 s for 5 loops, 10.8 s for 4) and every oscillation makes a whole number of
cycles per loop, so the 43.2 s animation period (all the scene's clips loop over it) repeats seamlessly. `phase` (s)
shifts each sitter's loop, so they do not move in sync.

The offsets are rotations about world axes (up, the sitter's forward, its left) applied in the static pose's armature
space: a bone's local rotation becomes C^-1 R C q0, with C its armature-space orientation over its static local
rotation q0, so its orientation turns by R on top of whatever its parents do.
"""
import math

import numpy as np
from mathutils import Quaternion, Vector

FPS = 30
BREATH = 4.0                                   # s, about; a whole number of breaths per loop
SPINE = (('Spine', 0.3), ('Spine1', 0.6), ('Spine2', 0.9))   # degrees of chest pitch per bone
SHOULDER_RISE = 0.8                            # degrees
# Head looks: (u, yaw degrees, pitch degrees) keys over the loop (u 0..1), eased between keys (equal keys hold).
LOOKS = {
    'idle_a': [(0.0, 0, 0), (0.08, 0, 0), (0.18, 26, 3), (0.42, 26, 3), (0.52, 0, 0), (0.62, 0, 0), (0.72, -21, -2),
               (0.88, -21, -2), (0.97, 0, 0), (1.0, 0, 0)],
    'idle_b': [(0.0, 0, 2), (0.12, 0, 2), (0.22, -28, 0), (0.48, -28, 0), (0.58, -6, 4), (0.70, -6, 4), (0.78, 22, 1),
               (0.90, 22, 1), (0.98, 0, 2), (1.0, 0, 2)],
    'talk': [(0.0, 0, 0), (0.06, 0, 0), (0.14, 24, 2), (0.50, 24, 2), (0.58, 8, 0), (0.64, 8, 0), (0.70, 24, 2),
             (0.88, 24, 2), (0.95, 0, 0), (1.0, 0, 0)],
}
# 'talk': when the gesturing arm is up (0..1 keys, eased), and its side.
GESTURE = [(0.0, 0), (0.10, 0), (0.17, 1), (0.46, 1), (0.53, 0), (0.62, 0), (0.68, 1), (0.84, 1), (0.90, 0), (1.0, 0)]
TALK_SIDE = 'Right'


def track(keys, u, column=1):
    """The eased (smoothstep) value of the periodic key list at u in [0, 1)."""
    for a, b in zip(keys, keys[1:]):
        if a[0] <= u <= b[0]:
            s = 0.0 if b[0] == a[0] else (u - a[0]) / (b[0] - a[0])
            s = s * s * (3 - 2 * s)
            return a[column] + (b[column] - a[column]) * s
    return keys[-1][column]


def _bones(rig):
    return {b.name.split(':')[-1]: b for b in rig.pose.bones}


def _direction(rig, bone):
    world = rig.matrix_world
    return ((world @ bone.tail) - (world @ bone.head)).normalized()


def offsets(u, loop, style, axes, dirs):
    """{bone suffix: [(world axis, radians), ...]} at loop phase u for the style; applied in order."""
    up, fwd, left = axes
    out = {}

    def add(name, axis, degrees):
        out.setdefault(name, []).append((axis, math.radians(degrees)))

    breaths = max(1, round(loop / BREATH))
    b = math.sin(2 * math.pi * breaths * u)
    chest = 0.0
    for name, amp in SPINE:
        add(name, left, -amp * b)  # about left: + pitches forward; inhaling lifts the chest back
        chest += amp * b
    rise = SHOULDER_RISE * (0.5 + 0.5 * b)
    add('LeftShoulder', fwd, rise)
    add('RightShoulder', fwd, -rise)
    for side, sign in (('Left', 1.0), ('Right', -1.0)):
        add(f'{side}Arm', fwd, -sign * rise)   # undo the shoulder ...
        add(f'{side}Arm', left, chest)         # ... and the chest, so the hands stay on the thighs
    add('Neck', left, chest)
    mirrored = style.endswith('_mirrored')  # v2.5b: the talking pair's partner looks and gestures the other way
    style = style.removesuffix('_mirrored')
    yaw, pitch = track(LOOKS[style], u, 1) * (-1 if mirrored else 1), track(LOOKS[style], u, 2)
    add('Neck', up, 0.4 * yaw)
    add('Head', up, 0.6 * yaw)
    gesture = track(GESTURE, u) if style == 'talk' else 0.0
    beats = max(1, round(loop * 1.3))
    beat = math.sin(2 * math.pi * beats * u)
    add('Head', left, pitch + 2.5 * gesture * beat)
    for k, side in enumerate(('Left', 'Right')):
        fore_dir, hand_dir = dirs[side]
        lift_axis, wrist_axis = fore_dir.cross(up).normalized(), hand_dir.cross(up).normalized()
        if style == 'talk' and side == (('Left' if TALK_SIDE == 'Right' else 'Right') if mirrored else TALK_SIDE):
            arm_dir = dirs[side + 'Arm']
            add(f'{side}Arm', arm_dir.cross(up).normalized(), 12 * gesture)
            add(f'{side}ForeArm', lift_axis, 50 * gesture + 6 * gesture * math.sin(2 * math.pi * beats * u + 0.6))
            add(f'{side}ForeArm', up, 9 * gesture * math.sin(2 * math.pi * (beats // 2 or 1) * u))
            add(f'{side}Hand', fore_dir, -35 * gesture + 12 * gesture * beat)   # palm turns up, beats
            add(f'{side}Hand', wrist_axis, 10 * gesture * beat)
            continue
        slow = math.sin(2 * math.pi * (u + 0.37 * k))
        add(f'{side}ForeArm', lift_axis, 1.5 * (1 + slow))
        add(f'{side}ForeArm', up, 2.0 * math.sin(2 * math.pi * (u + 0.21 + 0.5 * k)))
        taps = 2 + k
        tap = max(0.0, math.sin(2 * math.pi * (taps * u + 0.3 * k))) ** 6
        add(f'{side}Hand', wrist_axis, 9 * tap)
    return out


def loop_rows(rig, rows, person, frames):
    """The sitter's rows (garden_people_seat.sit: the static pose, every channel constant) with the idle on top: the
    animated bones' rotation rows replaced. `rig` is in the static pose. Returns (rows, summary)."""
    loops, phase, style = person['idle']
    loop = (frames - 1) / FPS / loops
    world = rig.matrix_world.to_quaternion()
    to_arm = world.inverted()
    heading = person['heading']
    up, fwd = Vector((0, 0, 1)), Vector((math.cos(heading), math.sin(heading), 0))
    axes = (up, fwd, up.cross(fwd))
    bones = _bones(rig)
    dirs = {side: (_direction(rig, bones[f'{side}ForeArm']), _direction(rig, bones[f'{side}Hand'])) for side in ('Left', 'Right')}
    dirs.update({f'{side}Arm': _direction(rig, bones[f'{side}Arm']) for side in ('Left', 'Right')})
    sample = offsets(0.0, loop, style, axes, dirs)
    names = list(sample)
    base = {n: Quaternion(bones[n].rotation_quaternion) for n in names}
    frame_of = {n: bones[n].matrix.to_quaternion() @ base[n].inverted() for n in names}   # C
    values = {n: np.empty((frames, 4)) for n in names}
    peak = {n: 0.0 for n in names}
    for f in range(frames):
        u = ((f / FPS + phase) / loop) % 1.0
        for name, turns in offsets(u, loop, style, axes, dirs).items():
            r = Quaternion()
            for axis, angle in turns:
                r = Quaternion(to_arm @ axis, angle) @ r
            c = frame_of[name]
            q = c.inverted() @ r @ c @ base[name]
            if f and np.dot(values[name][f - 1], q) < 0:
                q.negate()
            values[name][f] = q
            peak[name] = max(peak[name], math.degrees(r.angle if r.angle <= math.pi else 2 * math.pi - r.angle))
    paths = {bones[n].path_from_id('rotation_quaternion'): n for n in names}
    out = [(path, values[paths[path]] if path in paths else v) for path, v in rows]
    summary = dict(kind='procedural', style=style, loops=loops, loop_s=round(loop, 3), phase_s=phase,
                   breaths_per_loop=max(1, round(loop / BREATH)), peak_degrees={n: round(p, 1) for n, p in peak.items()})
    return out, summary
