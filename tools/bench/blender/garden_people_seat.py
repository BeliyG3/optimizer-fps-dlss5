"""The static seated pose for a garden person on a bench (garden_people.py, garden v2.4); the contact measures are in
garden_people_contact.py (v2.5: the sitters' loops are checked with them too, garden_people_sit_motion.py).

From the rig's rest pose (the Mixamo T-pose, upright): the thighs point forward (6 degrees apart), the shins down, the
feet as at rest (flat), the upper arms down and a little forward, the forearms onto the thighs (hands half way to the
knees, along the thighs). One parameter `reach` places the feet: below 0 it tips the thighs down (up to THIGH_DOWN), above
0 it swings the shins forward (up to SHIN_FORWARD, the knee opening past 90 degrees). Contact is measured on the
evaluated (skinned) meshes with raycasts (garden_people_contact.py, its tolerances):
- seat: the buttocks' lowest underside (the sitting bones' region) against the seat's top: the root's height makes it
  EMBED (the tipped thighs then rest on the seat's edge; garden_people_sit_motion moves the sitter forward if they cut it);
- feet: each sole against the ground under it: `reach` by bisection makes the lower one EMBED;
- knees (v2.5b): the root moves forward until both knee joints are KNEE_CLEAR in front of the seat's front edge, so
  the shins hang in front of the seat (and by `forward` more when the sloping thighs would cut the seat's edge);
- back: the body (the band of heights above the seat) against the bench's back, horizontal rays: never into it
  (BACK_GAP left).
Repeated until it settles; the build fails if a contact is out of tolerance. The pose is keyed over the whole period
(every channel constant), as a single-frame animation would be.
"""
import math

import bpy
import numpy as np
from mathutils import Quaternion, Vector

from garden_people_contact import BACK_GAP, EMBED, FRONT, KNEE_CLEAR, Bench, Body, contact_problems, feet_gap, hips

THIGH_DOWN, SHIN_FORWARD, SPLAY = math.radians(35), math.radians(55), math.radians(6)
HIPS_Y = 0.06                  # where the hips joint starts on the seat (local y), before the knees move it forward
ITERATIONS = 10               # seat, knees and feet settle together


def _bone(rig, suffix):
    return next(b for b in rig.pose.bones if b.name.endswith(suffix))


def _refresh(rig):
    """Re-evaluates the pose (setting PoseBone.matrix does not tag the armature, so its children would read stale
    matrices)."""
    rig.update_tag()
    bpy.context.view_layer.update()


def _aim(rig, bone, direction):
    """Turns the pose bone (the least rotation) so its axis points along the world direction."""
    target = (rig.matrix_world.to_3x3().inverted() @ direction).normalized()
    m = bone.matrix.copy()
    turn = m.to_3x3().col[1].normalized().rotation_difference(target).to_matrix()
    new = (turn @ m.to_3x3()).to_4x4()
    new.translation = m.translation
    bone.matrix = new
    _refresh(rig)


def _rest_orientation(rig, bone):
    m = bone.bone.matrix_local.to_3x3().to_4x4()
    m.translation = bone.matrix.translation
    bone.matrix = m
    _refresh(rig)


def pose(rig, heading, reach):
    """Sets the seated pose for a person facing `heading` (radians) with the feet placement `reach` in [-1, 1]."""
    for bone in rig.pose.bones:
        bone.location, bone.rotation_quaternion, bone.scale = (0, 0, 0), (1, 0, 0, 0), (1, 1, 1)
    bpy.context.view_layer.update()
    up, fwd = Vector((0, 0, 1)), Vector((math.cos(heading), math.sin(heading), 0))
    left = up.cross(fwd)
    down_angle, forward_angle = THIGH_DOWN * max(0.0, -reach), SHIN_FORWARD * max(0.0, reach)
    for side, sign in (('Left', 1.0), ('Right', -1.0)):
        flat = (fwd * math.cos(SPLAY) + left * sign * math.sin(SPLAY)).normalized()
        _aim(rig, _bone(rig, f'{side}UpLeg'), flat * math.cos(down_angle) - up * math.sin(down_angle))
        _aim(rig, _bone(rig, f'{side}Leg'), -up * math.cos(forward_angle) + flat * math.sin(forward_angle))
        _rest_orientation(rig, _bone(rig, f'{side}Foot'))
        world = rig.matrix_world
        hip, knee = world @ _bone(rig, f'{side}UpLeg').head, world @ _bone(rig, f'{side}Leg').head
        thigh = hip + (knee - hip) * 0.5 + up * 0.075 - left * sign * 0.01
        _arm_onto(rig, side, sign, thigh, up, fwd, left)
        _aim(rig, _bone(rig, f'{side}Hand'), (knee - hip).normalized() - up * 0.25)


def _arm_onto(rig, side, sign, target, up, fwd, left):
    """Swings the upper arm (down, a little out, pitched forward or back) so the forearm reaches `target` (the wrist on
    the thigh), then aims the forearm at it."""
    world = rig.matrix_world
    arm, fore, hand = (_bone(rig, f'{side}{n}') for n in ('Arm', 'ForeArm', 'Hand'))
    shoulder = world @ arm.head
    upper, lower = ((world @ fore.head) - shoulder).length, ((world @ hand.head) - (world @ fore.head)).length
    out = left * sign * 0.15
    best = min(range(-40, 71), key=lambda d: abs(
        (shoulder + upper * (-up * math.cos(math.radians(d)) + fwd * math.sin(math.radians(d)) + out).normalized()
         - target).length - lower))
    a = math.radians(best)
    _aim(rig, arm, -up * math.cos(a) + fwd * math.sin(a) + out)
    _aim(rig, fore, target - world @ fore.head)


def sit(rig, meshes, person, base, bench_obj, at, count, forward=0.0):
    """Poses the sitter on the bench. Returns (rows: the pose keyed `count` times, as garden_people.write takes them;
    the contact report in metres). Raises if a contact is out of tolerance. `forward` (m): further forward than the
    knees need (garden_people_sit_motion: a thigh into the seat's front edge)."""
    heading = person['heading']
    rig.animation_data_clear()
    rig.rotation_mode = 'QUATERNION'
    rig.rotation_quaternion = Quaternion((0, 0, 1), heading) @ base
    bench, body = Bench(bench_obj, person['local_x']), Body(meshes)
    m = bench_obj.matrix_world
    rig.location = m @ Vector((person['local_x'], HIPS_Y, 0.0))
    reach = 0.0
    knees = [b for b in rig.pose.bones if b.name.endswith(('LeftLeg', 'RightLeg'))]
    for _ in range(ITERATIONS):
        pose(rig, heading, reach)
        joint = rig.matrix_world @ _bone(rig, 'Hips').head
        target = m @ Vector((person['local_x'], HIPS_Y, 0.0))
        if _ == 0:
            rig.location += Vector((target.x - joint.x, target.y - joint.y, 0.0))
            bpy.context.view_layer.update()
        ahead = bench.local(np.array([list(rig.matrix_world @ b.head) for b in knees]))[:, 1].max()
        rig.location -= bench.back * (max(0.0, ahead - (FRONT - KNEE_CLEAR)) + (forward if _ == 0 else 0.0))
        bpy.context.view_layer.update()
        rig.location.z += EMBED - bench.seat_gap(body.vertices(), body, hips(rig))
        bpy.context.view_layer.update()
        rig.location += bench.back * min(max(bench.back_gap(body.vertices(), body) - BACK_GAP, -0.1), 0.0)
        bpy.context.view_layer.update()
        low, high = -1.0, 1.0
        for _step in range(18):
            reach = (low + high) / 2
            pose(rig, heading, reach)
            if min(feet_gap(body, body.vertices(), at).values()) > EMBED:
                high = reach
            else:
                low = reach
    rig.location.z += EMBED - bench.seat_gap(body.vertices(), body, hips(rig))  # the feet's last thigh tip moved the seat a little
    bpy.context.view_layer.update()
    points = body.vertices()
    seat_gap, back_gap, feet = bench.seat_gap(points, body, hips(rig)), bench.back_gap(points, body), feet_gap(body, points, at)
    report = dict(bench=person['bench'], seat_z=round(bench.seat, 4), seat_gap=round(seat_gap, 4),
                  back_gap=round(back_gap, 4), feet_gap={k: round(v, 4) for k, v in feet.items()}, reach=round(reach, 3),
                  hips_z=round((rig.matrix_world @ _bone(rig, 'Hips').head).z, 3))
    print(f"[people] {person['name']}: seated on {person['bench']}: seat {seat_gap * 100:+.1f} cm, back "
          f"{back_gap * 100:+.1f} cm, feet {', '.join(f'{k} {v * 100:+.1f} cm' for k, v in feet.items())}, reach {reach:+.2f}",
          flush=True)
    problems = contact_problems(seat_gap, back_gap, feet)
    if problems:
        raise RuntimeError(f"{person['name']} does not sit on {person['bench']}: {problems}")
    rows = []
    for bone in rig.pose.bones:
        for prop in ('location', 'rotation_quaternion', 'scale'):
            rows.append((bone.path_from_id(prop), np.tile(np.array(getattr(bone, prop)), (count, 1))))
    rows += [('location', np.tile(np.array(rig.location), (count, 1))),
             ('rotation_quaternion', np.tile(np.array(rig.rotation_quaternion), (count, 1)))]
    return rows, report
