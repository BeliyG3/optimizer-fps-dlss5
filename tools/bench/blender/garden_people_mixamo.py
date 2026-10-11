"""Mixamo seated clips for the garden sitters (garden v2.5, v2.5b), used instead of the procedural idle when present.

garden_people_mixamo_clip.py finds and reads the clip (bone channels on every frame, the feet on the floor); here:
- Retarget: the clip's bones map onto the sitter's rig by name (the part after 'mixamorig:'). The rotations are carried
  over relative to the rest pose in armature space, so another armature axis or unit also works: G and k fit the
  clip's rest joints onto the rig's (rotation and scale, least squares). A bone's armature-space turn from rest is
  D = (G Rs) q (G Rs)^-1 (Rs: the clip bone's rest), its local rotation Rt^-1 D Rt (Rt: the rig bone's rest); the
  hips' translation k G Rs t. `mirror` (v2.5b, a partner looking the other way) takes each bone's turn from the
  other side's bone, reflected in the rig's middle plane (normal: from the right hip joint to the left one at rest).
- Loop: the clip's last BLEND frames cross-fade into its first ones (a loop of count - BLEND frames), played a whole
  number of times over the 43.2 s period (retimed by under about 10 % for clips up to 9 s), shifted by the phase.
- Fit on the bench: the root turns so the clip's frame-0 facing (from the hip joints) is the sitter's heading (the
  bench's front plus its yaw); the hips joint goes over the seat spot, then forward until both knees are at least
  contact.KNEE_CLEAR in front of the seat's front edge at every sampled frame (v2.5b: the shins hang in front of the seat).
  Per frame the root's height keeps the lowest buttock or thigh underside at EMBED on the seat (measured every SEAT_STEP frames on
  the skinned mesh, interpolated). Per frame each planted foot's shin pitches about the knee towards the vertical, then
  the thigh about the hip for what is left (the foot back by as much, each at most MAX_LEG_FIX), so the ankle is the
  sole's height above the ground; a foot off the floor in the clip (Sitting Talking's crossed leg) takes the planted
  thigh's pitch (one axis through both hips: the crossed leg stays on the knee). garden_people_sit_motion moves the
  sitter further forward when a sloping thigh would cut the seat's front edge. Seat and feet repeat
  PASSES times, the feet last. A lean back into the bench moves the root forward (one shift for the loop).
  garden_people_sit_motion.py then checks every contact and the penetration into the bench over the loop.
"""
import math
import numpy as np
from mathutils import Matrix, Quaternion, Vector

import garden_people_contact as contact
import garden_people_layout as people_layout
import garden_people_mixamo_clip as mixamo_clip
import garden_people_seat as seat

BLEND, SEAT_STEP, PASSES = 15, 5, 5
MAX_LEG_FIX = math.radians(35)
HIPS = ('LeftUpLeg', 'RightUpLeg')


def _fit_rest(clip, rig):
    """(G, k, rms m): the rotation and scale taking the clip's rest joints onto the rig's (armature spaces)."""
    keys = [b.name.split(':')[-1] for b in rig.pose.bones if b.name.split(':')[-1] in clip]
    src = np.array([clip[k][1] for k in keys])
    dst = np.array([np.array(b.bone.matrix_local.translation) for b in rig.pose.bones if b.name.split(':')[-1] in clip])
    sc, dc = src - src.mean(0), dst - dst.mean(0)
    k = math.sqrt((dc ** 2).sum() / (sc ** 2).sum())
    u, _, vt = np.linalg.svd(sc.T @ dc)
    d = np.sign(np.linalg.det(vt.T @ u.T))
    g = vt.T @ np.diag([1, 1, d]) @ u.T
    rms = math.sqrt((((sc * k) @ g.T - dc) ** 2).sum(1).mean())
    return Matrix(g.tolist()).to_quaternion(), k, rms


def _looped(values, quaternion, cycles, length, phase):
    """values (count + 1 rows) played as a loop of `length` = count - BLEND frames (the tail cross-fading into the head)
    `cycles` times over the period, from `phase` frames: rows for frames 0..PERIOD_FRAMES."""
    def at(x):
        i = np.minimum(np.floor(x).astype(int), len(values) - 2)
        f = (x - i)[:, None]
        a, b = values[i], values[i + 1]
        return a * (1 - f) + b * f
    k = np.arange(people_layout.PERIOD_FRAMES + 1)
    p = ((k + phase) * cycles * length / people_layout.PERIOD_FRAMES) % length
    head, tail = at(p), at(np.minimum(p + length, len(values) - 1))
    w = np.clip(p / BLEND, 0, 1)[:, None]
    if quaternion:
        tail = np.where(np.sum(head * tail, axis=1, keepdims=True) < 0, -tail, tail)
    out = np.where(p[:, None] < BLEND, tail * (1 - w) + head * w, head)
    if quaternion:
        out /= np.linalg.norm(out, axis=1, keepdims=True)
        for i in range(1, len(out)):
            if np.dot(out[i], out[i - 1]) < 0:
                out[i] = -out[i]
    return out


def _other_side(key):
    return key.replace('Left', '#').replace('Right', 'Left').replace('#', 'Right')


def _reflect(q, normal):
    """The rotation q mirrored in the plane with this normal (a reflection turns the axis to -M a)."""
    v = Vector(q[1:])
    v = -v + 2 * v.dot(normal) * normal
    return Quaternion((q[0], *v))


def retarget(rig, clip, planted, phase, mirror=False):
    """The rig's bone rows from the clip, looped over the period (`mirror`: left and right swapped). Returns (rows,
    summary, planted: frames x 2 bools, the feet on the floor, looped the same way)."""
    g, k, rms = _fit_rest(clip, rig)
    count = len(next(iter(clip.values()))[2]) - 1
    length = count - BLEND
    if length < 2 * BLEND:
        raise RuntimeError(f"the clip is {count} frames long, too short to loop")
    cycles = max(1, round(people_layout.PERIOD_FRAMES / length))
    bones = {b.name.split(':')[-1]: b for b in rig.pose.bones}
    normal = (bones['LeftUpLeg'].bone.head_local - bones['RightUpLeg'].bone.head_local).normalized()
    rows, mapped = [], 0
    n = people_layout.PERIOD_FRAMES + 1
    for key, bone in bones.items():
        source = _other_side(key) if mirror else key
        loc, rot = np.zeros((n, 3)), np.tile([1.0, 0, 0, 0], (n, 1))
        if source in clip:
            rs, _, loc_s, rot_s = clip[source]
            turn, rest = g @ rs, bone.bone.matrix_local.to_quaternion()
            deltas = [turn @ Quaternion(q) @ turn.inverted() for q in rot_s]
            if mirror:
                deltas = [_reflect(d, normal) for d in deltas]
            rot = _looped(np.array([tuple(rest.inverted() @ d @ rest) for d in deltas]), True, cycles, length, phase)
            if key == 'Hips':
                moved = (loc_s @ np.array(turn.to_matrix()).T) * k
                if mirror:
                    moved -= 2 * np.outer(moved @ np.array(normal), np.array(normal))
                loc = _looped(moved @ np.array(rest.to_matrix()), False, cycles, length, phase)
            mapped += 1
        rows += [(bone.path_from_id('location'), loc), (bone.path_from_id('rotation_quaternion'), rot),
                 (bone.path_from_id('scale'), np.ones((n, 3)))]
    summary = dict(kind='mixamo', clip_frames=count, loop_frames=length, cycles=cycles, mirrored=mirror,
                   retime=round(people_layout.PERIOD_FRAMES / cycles / length, 4), bones_mapped=mapped,
                   rest_fit=dict(scale=round(k, 4), rms_m=round(rms, 4)))
    feet = planted[:, ::-1] if mirror else planted
    return rows, summary, _looped(feet.astype(float), False, cycles, length, phase) > 0.5


def _set_root(rows, location, rotation):
    rows = [r for r in rows if r[0] not in ('location', 'rotation_quaternion')]
    return rows + [('location', location), ('rotation_quaternion', rotation)]


def _place(rig, rows, person, base, bench, body, bench_obj):
    """Root rotation (the clip's frame-0 facing turned to the bench's) and frame-0 placement: the hips joint over the
    seat spot, the pelvis on the seat, the back off the bench's back. Returns the rows with constant root rows."""
    n = people_layout.PERIOD_FRAMES + 1
    rig.animation_data_clear()
    rig.rotation_mode = 'QUATERNION'
    spot = bench_obj.matrix_world @ Vector((person['local_x'], seat.HIPS_Y, 0.0))

    def put(location, rotation):
        placed = _set_root(rows, np.tile(np.array(location), (n, 1)), np.tile(np.array(rotation), (n, 1)))
        contact.apply_rows(rig, placed, 0)
        return placed

    put(spot, base)
    turn = Quaternion((0, 0, 1), person['heading'] - contact.facing(rig, HIPS)) @ base
    location = Vector(spot)
    for _ in range(3):
        put(location, turn)
        joint = Vector(contact.hips(rig))
        location += Vector((spot.x - joint.x, spot.y - joint.y, 0.0))
        put(location, turn)
        location.z += contact.EMBED - bench.seat_gap(body.vertices(), body)
        put(location, turn)
        location += bench.back * min(max(bench.back_gap(body.vertices(), body) - contact.BACK_GAP, -0.1), 0.15)
    return put(location, turn)


def _forward(rig, rows, bench, extra):
    """Moves the root forward (constant) so both knee joints stay KNEE_CLEAR in front of the seat's front edge at every
    30th frame, and `extra` m further. Returns (rows, shift m)."""
    knees = [b for b in rig.pose.bones if b.name.endswith(('LeftLeg', 'RightLeg'))]
    ahead = []
    for f in range(0, people_layout.PERIOD_FRAMES + 1, 30):
        contact.apply_rows(rig, rows, f)
        ahead.append(bench.local(np.array([list(rig.matrix_world @ b.head) for b in knees]))[:, 1].max())
    shift = max(0.0, max(ahead) - (contact.FRONT - contact.KNEE_CLEAR)) + extra
    return _set_root(rows, rows[-2][1] - np.array(bench.back) * shift, rows[-1][1]), shift


def _seat_heights(rig, rows, bench, body):
    """The root's z per frame keeping the lowest buttock or thigh underside EMBED on the seat (every SEAT_STEP frames,
    linear between), and one forward shift for the whole loop when the body leans into the
    bench's back (BACK_GAP left; one shift, so the pelvis does not slide). Returns (rows, (least, largest) height fix,
    back shift)."""
    n = people_layout.PERIOD_FRAMES + 1
    samples = np.arange(0, n, SEAT_STEP)
    if samples[-1] != n - 1:
        samples = np.append(samples, n - 1)
    fix, backs = [], []
    for f in samples:
        contact.apply_rows(rig, rows, f)
        points = body.vertices()
        fix.append(contact.EMBED - bench.seat_gap(points, body))
        backs.append(bench.back_gap(points, body))
    shift = min(max(min(backs) - contact.BACK_GAP, -0.1), 0.0)
    location = rows[-2][1].copy()
    location[:, 2] += np.interp(np.arange(n), samples, fix)
    location += np.array(bench.back) * shift
    return _set_root(rows, location, rows[-1][1]), (float(min(fix)), float(max(fix))), shift


def _pitch(pivot, ankle, left, want):
    """The pitch (radians, about `left` through the pivot) that brings the ankle to height `want` above the pivot (the
    nearest it gets when out of reach)."""
    v = ankle - pivot
    a, b = v.z, left.cross(v).z
    phi, spread = math.atan2(b, a), math.acos(max(-1.0, min(1.0, want / math.hypot(a, b))))
    delta = min((math.remainder(phi + spread, 2 * math.pi), math.remainder(phi - spread, 2 * math.pi)), key=abs)
    return max(-MAX_LEG_FIX, min(MAX_LEG_FIX, delta))


def _turn(rig, rows, paths, f, bone, axis, angle):
    """Turns the bone's frame-f rotation row by `angle` about the armature-space axis (the pose evaluated at f)."""
    row = rows[paths[bone.path_from_id('rotation_quaternion')]][1]
    q = Quaternion(row[f])
    c = bone.matrix.to_quaternion() @ q.inverted()
    row[f] = tuple(c.inverted() @ Quaternion(axis, angle) @ c @ q)


def _plant_feet(rig, rows, person, body, at, planted):
    """Per frame, a planted foot's shin pitches about the knee, then its thigh about the hip for the rest (the foot back
    by as much), so the ankle is the sole's height above the ground; with one foot off the floor both thighs take the
    planted one's pitch. Returns (rows, the thigh pitch per frame and side in radians)."""
    bones = {b.name.split(':')[-1]: b for b in rig.pose.bones}
    paths = {r[0]: i for i, r in enumerate(rows)}
    contact.apply_rows(rig, rows, 0)
    world, points = rig.matrix_world, body.vertices()
    sole = {}  # the ankle's height over the sole's lowest point (the foot's 40 lowest vertices, as feet_gap)
    for side in ('Left', 'Right'):
        foot_z = np.sort(points[body.feet[side], 2])[:40].min()
        sole[side] = (world @ bones[f'{side}Foot'].head).z - foot_z
    heading = person['heading']
    left = Vector((0, 0, 1)).cross(Vector((math.cos(heading), math.sin(heading), 0)))
    armatures = [m for obj in body.meshes for m in obj.modifiers if m.type == 'ARMATURE' and m.show_viewport]
    for modifier in armatures:
        modifier.show_viewport = False  # bones only: the skinning is not needed here
    deltas = np.zeros((people_layout.PERIOD_FRAMES + 1, 2))
    sides = ('Left', 'Right')
    try:
        for f in range(people_layout.PERIOD_FRAMES + 1):
            for pivot_name in ('Leg', 'UpLeg'):
                contact.apply_rows(rig, rows, f)
                world = rig.matrix_world
                axis = (world.to_quaternion().inverted() @ left).normalized()
                pitch = np.zeros(2)
                for k, side in enumerate(sides):
                    if planted[f, k]:
                        pivot, ankle = world @ bones[side + pivot_name].head, world @ bones[f'{side}Foot'].head
                        pitch[k] = _pitch(pivot, ankle, left, at(ankle.x, ankle.y) + sole[side] + contact.EMBED - pivot.z)
                if pivot_name == 'UpLeg' and planted[f].sum() == 1:
                    pitch[:] = pitch[int(np.argmax(planted[f]))]
                for k, side in enumerate(sides):
                    if pitch[k] == 0.0 or (pivot_name == 'Leg' and not planted[f, k]):
                        continue
                    _turn(rig, rows, paths, f, bones[side + pivot_name], axis, pitch[k])
                    if planted[f, k]:
                        _turn(rig, rows, paths, f, bones[f'{side}Foot'], axis, -pitch[k])
                if pivot_name == 'UpLeg':
                    deltas[f] = pitch
    finally:
        for modifier in armatures:
            modifier.show_viewport = True
    for path in (bones[f'{s}{n}'].path_from_id('rotation_quaternion') for s in sides for n in ('UpLeg', 'Leg', 'Foot')):
        row = rows[paths[path]][1]
        for i in range(1, len(row)):
            if np.dot(row[i], row[i - 1]) < 0:
                row[i] = -row[i]
    return rows, deltas


def animate(rig, person, base, bench, body, bench_obj, at, path, forward=0.0):
    """The sitter's rows from the Mixamo clip at `path`, fitted on the bench. Returns (rows, summary, planted: the
    feet on the floor per frame, frames x 2). `forward` (m): further forward than the knees need."""
    phase = person['idle'][1] * people_layout.FPS
    mirror = person['idle'][2].endswith('mirrored')
    rows, summary, planted = retarget(rig, *mixamo_clip.load(path), phase, mirror)
    rows = _place(rig, rows, person, base, bench, body, bench_obj)
    rows, forward = _forward(rig, rows, bench, forward)
    pitch, shifts = 0.0, []
    for _ in range(PASSES):  # the shins' pitch moves the feet, the seat contact moves the knees: seat, feet, ...
        rows, seat_fix, shift = _seat_heights(rig, rows, bench, body)
        rows, deltas = _plant_feet(rig, rows, person, body, at, planted)
        pitch, shifts = pitch + deltas, shifts + [round(shift, 4)]
    leg_fix = math.degrees(float(np.abs(pitch).max()))
    summary.update(clip=path.name, feet_on_floor=planted.mean(axis=0).round(3).tolist(),
                   forward_m=round(float(forward), 4), seat_fix_m=[round(v, 4) for v in seat_fix], back_shift_m=shifts,
                   leg_fix_degrees=round(leg_fix, 1))
    if leg_fix >= math.degrees(MAX_LEG_FIX) - 0.01:
        raise RuntimeError(f"{person['name']}: {path.name} needs more than {math.degrees(MAX_LEG_FIX):.0f} degrees of thigh "
                           "pitch to keep the feet on the ground (a clip with the feet off the floor?)")
    print(f"[people] {person['name']}: Mixamo {path.name}, {summary}", flush=True)
    return rows, summary, planted
