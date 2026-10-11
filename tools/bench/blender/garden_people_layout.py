"""Where the garden's people are (build_lab_garden.py, garden v2.4), their walking tracks, and the checks that keep them
off the props, the plants, the walls, the combo camera's eye and each other.

Blender axes, metres, seconds. The people are copies of the lab's characters (lab_characters.blend, see
garden_people.py): each PEOPLE entry is (name, role, source rig, where, timing); names are people_<character>_<role>_<k>
(character: olivia, chad or xbot; glb_share_skins.py reads it):
- 'walk': ('path', side, s0, legs) walks back and forth along the path, `side` metres beside the route (> 0 left of
  the camera's travel), from s0 metres along that line; ('line', x, y, heading, legs) on a straight line from (x, y).
  A walker makes `legs` round trips per animation period at its source's speed (the lab's Olivia/Chad cadence,
  character_motion.bake), so its line is speed * (PERIOD / (2 legs) - TURN) long, turning on the spot for TURN seconds
  at each end like the lab's walkers. timing = (route offset s, pose offset frames): different start points and
  steps, so nobody walks in step.
- 'talk': ('pair', x, y, axis degrees, partner's name) stands 0.5 m from the pair's centre along the axis, facing the
  partner; 'dance': ('at', x, y, heading degrees); timing = pose offset frames (the source loop shifted).
- 'sit': ('bench', prop name, local x[, yaw degrees][, partner's name]) sits on that bench (garden_props_layout name),
  at the bench's local x, turned `yaw` from the bench's front (> 0: to the sitter's left), the pelvis on the seat, the
  knees in front of it and the feet on the ground; timing = (loops per period, phase s, style 'idle_a' / 'idle_b' /
  'talk' / 'talk_mirrored'): the seated loop (garden_people_sit_motion.py; a Mixamo clip, when present, keeps only the
  phase and the talk/idle choice, 'talk_mirrored' plays the talking clip mirrored: the partner who looks the other way).
  v2.5b: Chad and Olivia talk to each other on one double bench (Sitting Talking looks about 65 degrees to the
  talker's left, so Chad sits on the right and turns left, Olivia's mirrored clip looks right).
Checks (check, raised by the build): a walker's line (body RADIUS) keeps EYE_CLEAR from the combo eye's path (frames
0-1399) and from hero_girl's walking line in the hall; nothing within the clearances below of prop footprints, trunks,
ferns, buildings, crates, poles, walls and the fence; walkers keep PASS apart from each other at every frame and every
person's footprint keeps clear of the others' (a talking pair's partners excepted).
"""
import math

import numpy as np

from garden_layout import _box_distance
from garden_path import distance_to_route


FPS, PERIOD_FRAMES, TURN = 30, 1295, 1.5          # lab_characters.blend: character_period_frames at 30 fps
PERIOD = PERIOD_FRAMES / FPS
SPEED = {'olivia': 19.1 / (PERIOD / 2 - TURN), 'chad': 20.0 / (PERIOD / 2 - TURN)}  # character_motion.bake's walkers
RADIUS, EYE_CLEAR, PASS = 0.3, 0.7, 0.8           # a walker's body, its clearance from the eye, walkers' centres
TALK_RADIUS, SIT_FOOT = 0.35, 0.15                # a talker's footprint; a sitter's foot circle
SIT_FEET_Y = -0.55                                # a sitter's ankles (bench local y; the front edge is at -0.335)
JOINTS = ('LeftLeg', 'RightLeg', 'LeftFoot', 'RightFoot', 'LeftToe_End', 'RightToe_End', 'LeftHand', 'RightHand')  # sitters' (sit_motion)
SIT_WALK_CLEAR, SIT_PAIR_CLEAR = 0.3, 0.18        # walkers' bodies to sitters' knees/feet; partners' joints apart
PROP_CLEAR, TRUNK, FERN = 0.15, 0.6, 0.75         # beyond the body: props and walls; trunk and fern radii
HERO_LINE = np.array([(0.35, -6.1), (0.35, 13.0)])  # character_motion.route: hero_girl's line in the hall

PEOPLE = (
    # Walkers on the path, both sides, on segments that do not overlap on a side.
    # v2.5b: walk_0 and walk_2 make 2 round trips on shorter lines between the benches (were 1 trip past them), so
    # the sitters' knees and feet keep SIT_WALK_CLEAR from the walkers.
    ('people_olivia_walk_0', 'walk', 'olivia', ('path', -1.05, 11.0, 2), (0.0, 0)),
    ('people_chad_walk_1', 'walk', 'chad', ('path', -1.05, 24.0, 3), (5.3, 11)),
    ('people_chad_walk_2', 'walk', 'chad', ('path', 1.05, 21.0, 2), (27.0, 23)),
    ('people_olivia_walk_3', 'walk', 'olivia', ('path', 1.05, 1.6, 2), (14.0, 31)),
    # A walker on the yard's concrete north of the garden.
    ('people_chad_walk_4', 'walk', 'chad', ('line', 12.8, 19.6, 0.0, 2), (9.0, 7)),
    # Talking pairs: behind the single bench, by the picnic table, by the hall's wall north of the opening.
    ('people_olivia_talk_0', 'talk', 'olivia_talking', ('pair', 14.0, -12.5, 90.0, 'people_chad_talk_1'), 0),
    ('people_chad_talk_1', 'talk', 'chad_talking', ('pair', 14.0, -12.5, 90.0, 'people_olivia_talk_0'), 400),
    ('people_chad_talk_2', 'talk', 'chad_talking', ('pair', 21.9, 2.9, 30.0, 'people_olivia_talk_3'), 170),
    ('people_olivia_talk_3', 'talk', 'olivia_talking', ('pair', 21.9, 2.9, 30.0, 'people_chad_talk_2'), 820),
    ('people_olivia_talk_4', 'talk', 'olivia_talking', ('pair', 10.9, 16.6, 90.0, 'people_chad_talk_5'), 610),
    ('people_chad_talk_5', 'talk', 'chad_talking', ('pair', 10.9, 16.6, 90.0, 'people_olivia_talk_4'), 1050),
    # Dancers on the open ground east of the picnic table.
    ('people_xbot_dance_0', 'dance', 'hiphop', ('at', 25.6, 4.4, 200.0), 300),
    ('people_xbot_dance_1', 'dance', 'salsa', ('at', 27.6, 5.6, 230.0), 700),
    # Sitters on benches beside the path.
    ('people_olivia_sit_0', 'sit', 'olivia_talking', ('bench', 'prop_bench_double_1', -0.95), (4, 0.0, 'idle_a')),
    ('people_xbot_sit_2', 'sit', 'salsa', ('bench', 'prop_bench_0', 0.45), (5, 6.1, 'idle_b')),
    # v2.5b: a talking pair on one double bench, turned 25 degrees to each other, 1.4 m apart (closer, their crossed
    # feet meet: 1.0 m and 30 degrees put the toes past each other).
    ('people_chad_sit_1', 'sit', 'chad_talking', ('bench', 'prop_bench_double_0', -1.85, 25.0, 'people_olivia_sit_3'),
     (4, 3.7, 'talk')),
    ('people_olivia_sit_3', 'sit', 'olivia_talking', ('bench', 'prop_bench_double_0', -0.45, -25.0, 'people_chad_sit_1'),
     (4, 11.0, 'talk_mirrored')),
)
DANCE_RADIUS = {'hiphop': 0.95, 'salsa': 0.95}    # the clips move the feet and hands about the spot (garden_people.steps)


def _arc(line):
    return np.concatenate([[0.0], np.cumsum(np.hypot(*np.diff(line, axis=0).T))])


def offset_line(polyline, side, s0, length, step=0.05):
    """Points every `step` m along the line `side` m beside the route (left of travel > 0), from s0 for `length` m."""
    tangent = np.gradient(polyline, axis=0)
    tangent /= np.linalg.norm(tangent, axis=1, keepdims=True)
    shifted = polyline + side * np.column_stack([-tangent[:, 1], tangent[:, 0]])
    dense_s = np.arange(0.0, _arc(shifted)[-1], 0.01)
    dense = np.column_stack([np.interp(dense_s, _arc(shifted), shifted[:, k]) for k in (0, 1)])
    s = np.arange(s0, s0 + length + 1e-9, step)
    if s[-1] > dense_s[-1]:
        raise RuntimeError(f"a walker's line runs past the path's end ({s[-1]:.2f} > {dense_s[-1]:.2f} m)")
    return np.column_stack([np.interp(s, dense_s, dense[:, k]) for k in (0, 1)])


def track(line, legs, offset, frames):
    """(xy (n x 2), heading (n)) at the frames: back and forth along `line` `legs` times a period, turning on the spot
    (anticlockwise, TURN s) at both ends; `offset` seconds into the walk at frame 0. heading: radians, 0 = +x."""
    arc = _arc(line)
    tangent = np.diff(line, axis=0)
    angles = np.unwrap(np.arctan2(tangent[:, 1], tangent[:, 0]))
    angle = lambda s: np.interp(s, arc[:-1] + np.diff(arc) / 2, angles)  # noqa: E731
    lap, travel = PERIOD / legs, PERIOD / (2 * legs) - TURN
    u = (np.asarray(frames) / FPS + offset) % lap
    s = np.where(u < travel, arc[-1] * u / travel,
                 np.where(u < travel + TURN, arc[-1], np.where(u < 2 * travel + TURN,
                          arc[-1] * (1 - (u - travel - TURN) / travel), 0.0)))
    heading = np.where(u < travel, angle(s), np.where(u < travel + TURN, angle(arc[-1]) + math.pi * (u - travel) / TURN,
                       np.where(u < 2 * travel + TURN, angle(s) + math.pi,
                                angle(0.0) + math.pi + math.pi * (u - 2 * travel - TURN) / TURN)))
    xy = np.column_stack([np.interp(s, arc, line[:, k]) for k in (0, 1)])
    return xy, heading


def _bench_point(prop, lx, ly):
    c, s = math.cos(prop['turn']), math.sin(prop['turn'])
    lx, ly = lx * prop['scale'], ly * prop['scale']
    return prop['x'] + c * lx - s * ly, prop['y'] + s * lx + c * ly


def resolve(polyline, placed, sitters=True):
    """[dict(name, role, source, ...)] of PEOPLE: walkers get `line` (points) and `legs`, stationary people `x, y,
    heading` and `radius`; sitters (when `sitters`, after the scatter: their benches must exist) the bench's name, turn
    and the sitting spot; `feet` are the footprint centres every person clears in the grass."""
    people, by_name = [], {p.get('name'): p for p in placed}
    for name, role, source, where, timing in PEOPLE:
        person = dict(name=name, role=role, source=source)
        if role == 'walk':
            kind, legs = where[0], where[-1]
            length = SPEED[source.split('_')[0]] * (PERIOD / (2 * legs) - TURN)
            if kind == 'path':
                line = offset_line(polyline, where[1], where[2], length)
            else:
                a = math.radians(where[3])
                t = np.arange(0.0, length + 1e-9, 0.05)
                line = np.column_stack([where[1] + t * math.cos(a), where[2] + t * math.sin(a)])
            person.update(line=line, legs=legs, route_offset=timing[0], pose_offset=timing[1], length=length)
        elif role in ('talk', 'dance'):
            if role == 'talk':
                _, cx, cy, axis, partner = where
                order = [p[0] for p in PEOPLE]
                sign = -0.5 if order.index(name) < order.index(partner) else 0.5
                a = math.radians(axis)
                x, y = cx + sign * math.cos(a), cy + sign * math.sin(a)
                heading = a if sign < 0 else a + math.pi
                person.update(partner=partner, radius=TALK_RADIUS)
            else:
                _, x, y, degrees = where
                heading = math.radians(degrees)
                person.update(radius=DANCE_RADIUS[source])
            person.update(x=float(x), y=float(y), heading=float(heading) % (2 * math.pi), pose_offset=timing)
        else:
            if not sitters:
                continue
            bench = by_name[where[1]]
            yaw = math.radians(where[3]) if len(where) > 3 else 0.0
            x, y = _bench_point(bench, where[2], 0.1)
            c, s_ = math.cos(yaw), math.sin(yaw)
            feet = [_bench_point(bench, where[2] + c * d - s_ * (SIT_FEET_Y - 0.1), 0.1 + s_ * d + c * (SIT_FEET_Y - 0.1))
                    for d in (-0.12, 0.12)]
            person.update(bench=where[1], local_x=where[2], x=x, y=y, yaw=where[3] if len(where) > 3 else 0.0,
                          heading=(bench['turn'] - math.pi / 2 + yaw) % (2 * math.pi), feet=feet, radius=SIT_FOOT, idle=timing)
            if len(where) > 4:
                person['partner'] = where[4]
        if role in ('talk', 'dance'):
            person['feet'] = [(person['x'], person['y'])]
        people.append(person)
    return people


def avoid(people):
    """[(points (n x 2), radius)] a prop's footprint must keep clear of (garden_props_layout.Site): the walkers' lines and
    the standing people (the sitters sit on benches the scatter places)."""
    zones = []
    for p in people:
        if p['role'] == 'walk':
            zones.append((p['line'], RADIUS + PROP_CLEAR))
        elif p['role'] in ('talk', 'dance'):
            zones.append((np.array([[p['x'], p['y']]]), p['radius'] + PROP_CLEAR))
    return zones


def grass_points(people):
    """Points (n x 2) whose surroundings lose their grass: the standing and sitting people's feet, and a ring for the
    dancers' steps."""
    points = []
    for p in people:
        if p['role'] == 'dance':
            a = np.linspace(0, 2 * math.pi, 12, endpoint=False)
            points += [(p['x'], p['y'])] + list(zip(p['x'] + 0.45 * np.cos(a), p['y'] + 0.45 * np.sin(a)))
        elif p['role'] in ('talk', 'sit'):
            points += list(p['feet'])
    return np.array(points)


def _spots(p):
    """(points, radius) of a person's footprint for the checks."""
    if p['role'] == 'walk':
        return p['line'], RADIUS
    if p['role'] == 'sit':
        return np.array(p['feet']), SIT_FOOT
    return np.array([[p['x'], p['y']]]), p['radius']


def _to_props(points, radius, placed, site, skip=()):
    """Least distance between the footprint (points grown by radius) and any prop's footprint rectangle."""
    best, name = np.inf, None
    for prop in placed:
        if prop['name'] in skip:
            continue
        half = site.halves[prop['template']]
        c, s = math.cos(prop['turn']), math.sin(prop['turn'])
        dx, dy = points[:, 0] - prop['x'], points[:, 1] - prop['y']
        u, v = np.abs(c * dx + s * dy), np.abs(-s * dx + c * dy)
        d = float(np.hypot(np.maximum(u - half[0] * prop['scale'], 0), np.maximum(v - half[1] * prop['scale'], 0)).min())
        if d - radius < best:
            best, name = d - radius, prop['name']
    return best, name


def check(people, placed, site, eyes):
    """Raises on any problem (see the module's description). Returns a report per person (clearances in metres)."""
    frames = np.arange(PERIOD_FRAMES + 1)
    tracks = {p['name']: track(p['line'], p['legs'], p['route_offset'], frames)[0] for p in people if p['role'] == 'walk'}
    eye_line, problems, report = np.asarray(eyes), [], []
    x0, y0, x1, y1 = site.bounds
    for i, p in enumerate(people):
        points, radius = _spots(p)
        skip = (p['bench'],) if p['role'] == 'sit' else ()
        to_prop, prop = _to_props(points, radius, placed, site, skip)
        to_eye = float(distance_to_route(points, eye_line).min()) - radius
        to_hero = float(distance_to_route(points, HERO_LINE).min()) - radius
        to_box = float(_box_distance(points, site.obstacles).min()) - radius
        to_trunk = float(np.hypot(points[:, None, 0] - site.trunks[None, :, 0], points[:, None, 1] - site.trunks[None, :, 1]).min()) - radius
        to_fern = float(np.hypot(points[:, None, 0] - site.ferns[None, :, 0], points[:, None, 1] - site.ferns[None, :, 1]).min()) - radius
        others = []
        for q in people[:i]:
            if q['name'] == p.get('partner') or p['name'] == q.get('partner'):
                continue
            if p['role'] == 'walk' and q['role'] == 'walk':
                d = float(np.hypot(*(tracks[p['name']] - tracks[q['name']]).T).min())
                if d < PASS:
                    problems.append(f"{p['name']} passes {d:.2f} m from {q['name']} (at least {PASS})")
            else:
                qp, qr = _spots(q)
                d = float(np.hypot(points[:, None, 0] - qp[None, :, 0], points[:, None, 1] - qp[None, :, 1]).min()) - radius - qr
                if d < 0.1:
                    problems.append(f"{p['name']}: {d:.2f} m from {q['name']}")
            others.append(round(d, 2))
        if p['role'] == 'walk' and min(to_eye, to_hero) < EYE_CLEAR:
            problems.append(f"{p['name']}: {to_eye:.2f} m from the combo eye, {to_hero:.2f} m from hero_girl's line")
        if to_prop < PROP_CLEAR:
            problems.append(f"{p['name']}: {to_prop:.2f} m from {prop}")
        if to_box < PROP_CLEAR or to_trunk < TRUNK or to_fern < FERN:
            problems.append(f"{p['name']}: {to_box:.2f} m from a wall/building/crate/pole, {to_trunk:.2f} m from a trunk, "
                            f"{to_fern:.2f} m from a fern (bodies)")
        if points[:, 0].min() - radius < x0 + 0.2 or points[:, 0].max() + radius > x1 - 0.2 \
                or points[:, 1].min() - radius < y0 + 0.2 or points[:, 1].max() + radius > y1 - 0.2:
            problems.append(f"{p['name']}: outside the fence")
        report.append(dict(name=p['name'], role=p['role'], source=p['source'], to_prop=round(to_prop, 2), nearest_prop=prop,
                           to_eye=round(to_eye, 2), to_hero_line=round(to_hero, 2), to_obstacle=round(to_box, 2),
                           to_trunk=round(to_trunk, 2), to_fern=round(to_fern, 2),
                           to_people=min(others) if others else None))
        print(f"[people] {p['name']:15s} {p['role']:5s} {p['source']:15s} prop {to_prop:.2f} ({prop}) eye {to_eye:.2f} "
              f"wall {to_box:.2f} trunk {to_trunk:.2f} fern {to_fern:.2f} people {min(others) if others else '-'}", flush=True)
    if problems:
        raise RuntimeError("garden people:\n  " + "\n  ".join(problems))
    return report


def sitter_clearance(people, joints):
    """The sitters' knees, ankles and toes (joints: {name: {frame: joint positions}}, garden_people_sit_motion's) against
    every walker's line (its body: RADIUS) and the partners' and bench neighbours' joints at the same frames. Raises
    below SIT_WALK_CLEAR / SIT_PAIR_CLEAR. Returns {name: {to_walkers, nearest_walker, to_neighbour}} (m)."""
    walkers = [p for p in people if p['role'] == 'walk']
    sitters = [p for p in people if p['role'] == 'sit']
    report, problems = {}, []
    for p in sitters:
        legs = np.concatenate([j[:6, :2] for j in joints[p['name']].values()])  # knees, ankles, toes
        near = min(((float(distance_to_route(legs, w['line']).min()) - RADIUS, w['name']) for w in walkers), default=(math.inf, None))
        neighbour, closest = math.inf, None
        for q in sitters:
            if q is p or q['bench'] != p['bench']:
                continue
            for f, a in joints[p['name']].items():
                b = joints[q['name']].get(f)
                if b is not None:
                    d = np.linalg.norm(a[:, None, :] - b[None, :, :], axis=2)
                    if d.min() < neighbour:
                        i, j = np.unravel_index(int(d.argmin()), d.shape)
                        neighbour, closest = float(d.min()), (f, JOINTS[i], q['name'], JOINTS[j])
        report[p['name']] = dict(to_walkers=round(near[0], 3), nearest_walker=near[1],
                                 to_neighbour=round(neighbour, 3) if neighbour < math.inf else None)
        print(f"[people] {p['name']}: knees/feet {near[0]:.2f} m from {near[1]}'s body; nearest joint of a bench "
              f"neighbour {neighbour:.2f} m {closest or ''}", flush=True)
        if near[0] < SIT_WALK_CLEAR:
            problems.append(f"{p['name']}: knees/feet {near[0]:.2f} m from {near[1]} (at least {SIT_WALK_CLEAR})")
        if neighbour < SIT_PAIR_CLEAR:
            problems.append(f"{p['name']}: {neighbour:.2f} m from a bench neighbour's joints (at least {SIT_PAIR_CLEAR})")
    if problems:
        raise RuntimeError("garden sitters:\n  " + "\n  ".join(problems))
    return report
