"""The garden sitters' animation (garden v2.5, v2.5b): a looping seated clip on the bench, checked over the loop.

garden_people.build calls `animate` for each sitter: the Mixamo clip
`<character>__sitting_idle.fbx` / `__sitting_talking.fbx` when it is in the clips folder (garden_people_mixamo.py) or
the procedural idle (garden_people_idle.py) on top of the static seated pose (garden_people_seat.sit: pelvis on the
seat, knees in front of it, feet on the ground, by raycasts). The result is checked on the skinned mesh at
every CHECK_STEP-th frame of one loop and every PERIOD_STEP-th frame of the whole period (garden_people_contact.py):
- v2.5c: the lowest buttock or thigh underside over the seat, the buttocks alone and the planted soles touch (on the
  mesh, within contact.TOUCH: -1..+0.5 cm), the back not into the bench, a lifted foot (a crossed leg) not into the ground;
- v2.5b: no body part (hips, thighs, shins, feet, arms) deeper than contact.PENETRATION inside the bench; while a
  sloping thigh cuts the seat's front edge deeper, the sitter is fitted again further forward (up to FORWARD_TRIES);
- the hips joint and the planted ankles may not slide (their travel from their first sampled place is reported; for
  the procedural idle it must stay under SLIDE_LIMIT: the pelvis and legs are not animated).
The build raises on any failure. The report keeps the knees', ankles', toes' and hands' world positions at the
shared frames (every PERIOD_STEP-th) under 'joints' for garden_people's clearance checks (dropped from the JSON).
"""
import math

import numpy as np

import garden_people_contact as contact
import garden_people_idle as idle
import garden_people_layout as people_layout
import garden_people_mixamo as mixamo
import garden_people_mixamo_clip as mixamo_clip
import garden_people_seat as seat

CHECK_STEP, PERIOD_STEP = 3, 30
SLIDE_LIMIT = 0.02   # m: the procedural idle's ankles and hips joint must not move (Mixamo: reported only)
FORWARD_TRIES, FORWARD_STEP_MAX = 6, 0.2   # the penetration loop: tries, the most one try moves forward (m)


def _frames(loop_frames):
    loop = range(0, min(loop_frames, people_layout.PERIOD_FRAMES) + 1, CHECK_STEP)
    return sorted(set(loop) | set(range(0, people_layout.PERIOD_FRAMES + 1, PERIOD_STEP)))


def check(rig, rows, person, bench, body, at, loop_frames, planted):
    """Contacts and penetration at the sampled frames; raises if any is out of tolerance. Only a foot on the floor at
    that frame (`planted`, frames x 2: Mixamo Sitting Talking crosses the legs) is checked against the ground and for
    sliding; a lifted foot's least height is reported. Returns the ranges (m) and the joints."""
    seat_gaps, butt_gaps, back_gaps, slide, failures = [], [], [], [], []
    feet, lifted = {'Left': [], 'Right': []}, {'Left': [], 'Right': []}
    deepest = {part: 0.0 for part in body.parts}
    bones = {b.name.split(':')[-1]: b for b in rig.pose.bones}
    start, joints = {}, {}
    for f in _frames(loop_frames):
        contact.apply_rows(rig, rows, f)
        points = body.vertices()
        gap, back = bench.seat_gap(points, body), bench.back_gap(points, body)
        butt = bench.seat_gap(points, body, contact.hips(rig))
        butt_gaps.append(butt)
        foot = contact.feet_gap(body, points, at)
        depth, _ = bench.penetration(points, body.parts)
        deepest = {part: max(deepest[part], depth[part]) for part in deepest}
        seat_gaps.append(gap)
        back_gaps.append(back)
        on_floor = {side: value for k, (side, value) in enumerate(foot.items()) if planted[f, k]}
        for k, (side, value) in enumerate(foot.items()):
            (feet if planted[f, k] else lifted)[side].append(value)
        places = {'hips': contact.hips(rig)}
        places.update({side: np.array(rig.matrix_world @ bones[f'{side}Foot'].head) for side in on_floor})
        for name, p in places.items():
            start.setdefault(name, p)
            slide.append(float(np.linalg.norm((p - start[name])[:2])))
        if f % PERIOD_STEP == 0:
            joints[f] = np.array([list(rig.matrix_world @ bones[n].head) for n in people_layout.JOINTS])
        problems = contact.contact_problems(gap, back, on_floor, butt)
        problems += [f"lifted {side} {v * 100:+.1f} cm into the ground" for side, v in foot.items()
                     if side not in on_floor and v < -contact.SINK_LIMIT]
        problems += [f"{part} {d * 100:.1f} cm inside the bench" for part, d in depth.items() if d > contact.PENETRATION]
        if problems:
            failures.append((f, problems))
    contact.apply_rows(rig, rows, 0)
    span = lambda v: [round(min(v), 4), round(max(v), 4)] if v else None  # noqa: E731
    report = dict(frames_checked=len(seat_gaps), seat_gap=span(seat_gaps), buttocks_gap=span(butt_gaps),
                  back_gap_min=round(min(back_gaps), 4),
                  feet_gap={k: span(v) for k, v in feet.items()}, lifted_foot_gap={k: span(v) for k, v in lifted.items() if v},
                  penetration_max=deepest, slide_max=round(max(slide), 4), joints=joints)
    print(f"[people] {person['name']}: loop contacts over {len(seat_gaps)} frames: seat "
          f"{min(seat_gaps) * 100:+.1f}..{max(seat_gaps) * 100:+.1f} cm (buttocks {min(butt_gaps) * 100:+.1f}.."
          f"{max(butt_gaps) * 100:+.1f}), back >= {min(back_gaps) * 100:+.1f} cm, feet "
          + ', '.join(f"{k} {min(v) * 100:+.1f}..{max(v) * 100:+.1f} cm" for k, v in feet.items() if v)
          + ''.join(f", {k} lifted {min(v) * 100:+.1f}..{max(v) * 100:+.1f} cm" for k, v in lifted.items() if v)
          + f", hips/planted ankles slide <= {max(slide) * 100:.2f} cm; inside the bench (cm) "
          + ', '.join(f"{k} {v * 100:.1f}" for k, v in deepest.items()), flush=True)
    if failures:
        raise RuntimeError(f"{person['name']}: contacts out of tolerance at {len(failures)} frames, first {failures[:3]}")
    return report


def _forward_need(rig, rows, bench, body):
    """How much further forward (m) the sitter should sit so no leg cuts the seat's front edge deeper than
    contact.PENETRATION, from every 60th frame: a shin (the calf behind the knee) by how far its deep points are behind
    the edge; a thigh by its depth over the sine of its slope (moving forward by d lowers the cut by d sin(slope))."""
    bones = {b.name.split(':')[-1]: b for b in rig.pose.bones}
    legs = {part: body.parts[part] for part in ('thighs', 'shins')}
    need = 0.0
    for f in range(0, people_layout.PERIOD_FRAMES + 1, 60):
        contact.apply_rows(rig, rows, f)
        depth, deep = bench.penetration(body.vertices(), legs)
        if len(deep['shins']):
            need = max(need, float(deep['shins'][:, 1].max()) - contact.FRONT + 0.005)
        if depth['thighs'] > contact.PENETRATION:
            world = rig.matrix_world
            slope = max(((world @ bones[f'{s}UpLeg'].head).z - (world @ bones[f'{s}Leg'].head).z)
                        / ((world @ bones[f'{s}UpLeg'].head) - (world @ bones[f'{s}Leg'].head)).length for s in ('Left', 'Right'))
            need = max(need, (depth['thighs'] - 0.8 * contact.PENETRATION) / max(slope, 0.08) + 0.005)
    return min(need, FORWARD_STEP_MAX)


def animate(rig, meshes, person, base, bench_obj, at, clips):
    """The sitter's rows (garden_people.write) and report. `clips`: the Mixamo folder, or None for the procedural idle."""
    count = people_layout.PERIOD_FRAMES + 1
    bench, body = contact.Bench(bench_obj, person['local_x']), contact.Body(meshes)
    path = mixamo_clip.find(clips, person) if clips else None
    planted = np.ones((count, 2), bool)
    forward, tries = 0.0, []
    for _ in range(FORWARD_TRIES):  # v2.5b: further forward while a sloping thigh cuts the seat's front edge
        if path:
            report = dict(bench=person['bench'], seat_z=round(bench.seat, 4))
            rows, motion, planted = mixamo.animate(rig, person, base, bench, body, bench_obj, at, path, forward)
            loop_frames = math.ceil(motion['loop_frames'] * motion['retime'])
        else:
            rows, report = seat.sit(rig, meshes, person, base, bench_obj, at, count, forward)
            rows, motion = idle.loop_rows(rig, rows, person, count)
            loop_frames = math.ceil(motion['loop_s'] * people_layout.FPS)
        need = _forward_need(rig, rows, bench, body)
        tries.append(round(forward, 3))
        if need <= 0.0:
            break
        forward += need
    motion['forward_tries_m'] = tries
    checked = check(rig, rows, person, bench, body, at, loop_frames, planted)
    if motion['kind'] == 'procedural' and checked['slide_max'] > SLIDE_LIMIT:
        raise RuntimeError(f"{person['name']}: the hips or feet move {checked['slide_max'] * 100:.1f} cm in the idle")
    report.update(loop=dict(**motion, contact=checked))
    return rows, report
