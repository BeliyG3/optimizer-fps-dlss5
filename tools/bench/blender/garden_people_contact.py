"""Contact measures for the garden sitters on their benches (garden v2.4 seat pose, v2.5 loops): the skinned body's
pelvis on the seat, feet on the ground, back off the bench's back, and (v2.5b) how deep any body part is inside the
bench mesh. v2.5c: the seat and the planted soles must touch, TOUCH = (-1 cm, +0.5 cm) (not floating, not more than
1 cm in); a body part may be at most PENETRATION inside the bench (cloth on the slats); a lifted foot not more than
SINK_LIMIT into the ground.
- seat (v2.5c, mesh): the lowest underside of the buttocks and thighs over the seat: skinned vertices following the
  hips, thighs or spine most whose normals point down, inside the seat's footprint (local y between the front edge and
  the seat's back end, within SEAT_HALF of the sitter's spot along the bench), each against the seat surface under it
  by a ray down from just above the seat's top (v2.5b started at 0.3 m and hit the arm rests: Chad floated); the
  buttocks alone (SITTING_BONES around the hips joint) must touch too, so a thigh resting on the seat's front edge
  cannot hold a floating pelvis up;
- feet (v2.5c): each foot's sole (vertices weighted to the foot and toe bones whose normals point down) against the
  ground under each vertex;
- back: the body (the band of heights above the seat) against the bench's back, horizontal rays;
- penetration: how far any body vertex is below the seat's top within the seat's slats (SLAT thick: a thigh through
  them is "outside" again below them, so the seat is tested as a slab), and for the rest of the bench (back, arm rests,
  frame) its nearest face and that face's normal: behind the face = inside, by the distance (closed shells).
"""
import math

import bpy
import numpy as np
from mathutils import Vector
from mathutils.bvhtree import BVHTree

from garden_props_ground import FLOAT_LIMIT, SINK_LIMIT

EMBED, BACK_GAP = -0.005, 0.01
TOUCH = (-0.01, 0.005)         # m: a seat or planted-sole contact within this touches
SEAT_HALF = 0.45               # m: the seat contact's footprint along the bench either side of the sitter's spot
SITTING_BONES = 0.15           # m: the buttocks' contact, around the hips joint along the seat's depth
SEAT_Y = (-0.33, 0.22)         # the benches' seat in the template (garden_props: front edge at local y -0.335)
FRONT = -0.335                 # the seat's front edge (local y); the bench's front faces local -y
KNEE_CLEAR = 0.07              # m: the knee joints in front of the seat's front edge (the knee's radius, cloth)
PENETRATION = 0.01             # m: the deepest a body part may be inside the bench
SLAT = 0.035                   # m: the seat slab tested for penetration below the seat's top (the slats' depth)
PARTS = {'hips': ('Hips', 'Spine', 'Spine1', 'Spine2'), 'thighs': ('LeftUpLeg', 'RightUpLeg'),
         'shins': ('LeftLeg', 'RightLeg'), 'feet': ('LeftFoot', 'RightFoot', 'LeftToeBase', 'RightToeBase',
                                                     'LeftToe_End', 'RightToe_End')}


def _bone(rig, suffix):
    return next(b for b in rig.pose.bones if b.name.endswith(suffix))


class Body:
    """The person's skinned vertices (world); masks of the feet's, the seat's (pelvis and thighs) and the body parts'
    (PARTS, arms) vertices, by the bone each vertex follows most."""

    def __init__(self, meshes):
        self.meshes = meshes
        bones = []
        for obj in meshes:
            names = {g.index: g.name for g in obj.vertex_groups}
            bones += [names[max(v.groups, key=lambda g: g.weight).group] if len(v.groups) else '' for v in obj.data.vertices]
        self.feet = {side: np.array([b.endswith((f'{side}Foot', f'{side}ToeBase', f'{side}Toe_End')) for b in bones])
                     for side in ('Left', 'Right')}
        self.seated = np.array([b.endswith(('Hips', 'UpLeg', 'Spine')) for b in bones])
        keys = np.array([b.split(':')[-1] for b in bones])
        self.parts = {part: np.isin(keys, names) for part, names in PARTS.items()}
        self.parts['arms'] = (np.char.find(keys, 'Arm') >= 0) | (np.char.find(keys, 'Hand') >= 0) | (np.char.find(keys, 'Shoulder') >= 0)

    def vertices(self):
        """The skinned vertices (world, n x 3); their normals (world, unit) are kept in `normals`."""
        graph = bpy.context.evaluated_depsgraph_get()
        parts, normals = [], []
        for obj in self.meshes:
            evaluated = obj.evaluated_get(graph)
            mesh = evaluated.to_mesh()
            co, no = np.empty(len(mesh.vertices) * 3), np.empty(len(mesh.vertices) * 3)
            mesh.vertices.foreach_get('co', co)
            mesh.vertices.foreach_get('normal', no)
            m = np.array(evaluated.matrix_world)
            parts.append(co.reshape(-1, 3) @ m[:3, :3].T + m[:3, 3])
            normals.append(no.reshape(-1, 3) @ m[:3, :3].T)
            evaluated.to_mesh_clear()
        normals = np.concatenate(normals)
        self.normals = normals / np.maximum(np.linalg.norm(normals, axis=1, keepdims=True), 1e-9)
        return np.concatenate(parts)


class Bench:
    """The bench prop in world space: its BVH, local frame and seat height."""

    def __init__(self, obj, local_x):
        m = obj.matrix_world
        self.tree = BVHTree.FromPolygons([m @ v.co for v in obj.data.vertices], [p.vertices[:] for p in obj.data.polygons])
        self.inverse, self.local_x = m.inverted(), local_x
        self.back = (m.to_3x3() @ Vector((0, 1, 0))).normalized()
        hit = self.tree.ray_cast(m @ Vector((local_x, -0.1, 2.0)), Vector((0, 0, -1)), 5.0)[0]
        self.seat = hit.z
        local = np.array([v.co for v in obj.data.vertices])
        self.box = (local.min(axis=0) - 0.01, local.max(axis=0) + 0.01)
        self.seat_local = self.local(np.array([[hit.x, hit.y, hit.z]]))[0, 2]

    def local(self, points):
        m = np.array(self.inverse)
        return points @ m[:3, :3].T + m[:3, 3]

    def seat_gap(self, points, body, hips=None):
        """Signed height (m) of the body's lowest buttock or thigh underside over the seat above the seat's surface
        under it (< 0: into it): the module's seat measure, on the skinned mesh. With `hips` (the hips joint, world):
        only the sitting bones' region, within SITTING_BONES of the joint along the seat's depth (the buttocks)."""
        local = self.local(points)
        over = (np.abs(local[:, 0] - self.local_x) < SEAT_HALF) & (local[:, 1] > FRONT) & (local[:, 1] < SEAT_Y[1]) \
            & (points[:, 2] > self.seat - 0.1) & body.seated & (body.normals[:, 2] < -0.3)
        if hips is not None:
            over &= np.abs(local[:, 1] - self.local(np.array([hips]))[0, 1]) < SITTING_BONES
        low = points[over][np.argsort(points[over, 2])[:200]]
        gaps = []
        for p in low:
            hit = self.tree.ray_cast(Vector((p[0], p[1], self.seat + 0.05)), Vector((0, 0, -1)), 0.15)[0]
            if hit is not None and hit.z > self.seat - 0.05:
                gaps.append(p[2] - hit.z)
        if not gaps:
            raise RuntimeError("no buttock or thigh underside over the seat")
        return min(gaps)

    def back_gap(self, points, body):
        """Least distance from the torso (vertices following the hips or spine most, heights 0.08-0.4 m above the seat)
        to the bench's back rest along the back's direction (< 0: into it). Only hits on the back rest count (local y
        past the seat), not the arm rests (v2.5c: an arm rest beside Chad pushed him forward)."""
        local = self.local(points)
        band = (np.abs(local[:, 0] - self.local_x) < 0.3) & (points[:, 2] > self.seat + 0.08) & (points[:, 2] < self.seat + 0.4)             & body.parts['hips']
        best = math.inf
        for p in points[band]:
            start = Vector(p) - self.back * 0.6
            hit = self.tree.ray_cast(start, self.back, 1.2)[0]
            if hit is not None and self.local(np.array([list(hit)]))[0, 1] > SEAT_Y[1] - 0.07:
                best = min(best, (hit - Vector(p)).dot(self.back))
        return best if best < 0.3 else 0.3

    def penetration(self, points, parts):
        """({part: deepest inside the bench, m}, {part: local points inside deeper than PENETRATION})."""
        local = self.local(points)
        near = np.all((local > self.box[0]) & (local < self.box[1]), axis=1)
        under = near & (local[:, 1] > FRONT) & (local[:, 1] < SEAT_Y[1] - 0.07) & (local[:, 2] < self.seat_local) \
            & (local[:, 2] > self.seat_local - SLAT)
        depth = np.where(under, self.seat_local - local[:, 2], 0.0)
        for i in np.flatnonzero(near & ~under):
            hit, normal, _, dist = self.tree.find_nearest(Vector(points[i]))
            if hit is not None and (Vector(points[i]) - hit).dot(normal) < 0:
                depth[i] = dist
        deep = depth > PENETRATION
        return ({part: round(float(depth[mask].max()), 4) if mask.any() else 0.0 for part, mask in parts.items()},
                {part: local[mask & deep] for part, mask in parts.items()})


def facing(rig, pair=('LeftArm', 'RightArm')):
    """The rig's facing at its current pose (radians, 0 = +x) from its shoulders (character_motion.sample_clip), or
    from another left/right bone pair (the hips' for a seated clip, whose chest may turn)."""
    left, right = _bone(rig, pair[0]), _bone(rig, pair[1])
    forward = Vector((0, 0, 1)).cross(rig.matrix_world.to_3x3() @ (right.head - left.head))
    return math.atan2(forward.y, forward.x)


def hips(rig):
    return np.array(rig.matrix_world @ _bone(rig, 'Hips').head)


def apply_rows(rig, rows, frame):
    """Poses the rig as `rows` (garden_people.write's: data path, values per frame) at frame index `frame`, without keys."""
    for path, values in rows:
        if path.startswith('pose.bones["'):
            name, prop = path[len('pose.bones["'):].split('"].')
            setattr(rig.pose.bones[name], prop, values[frame])
        else:
            setattr(rig, path, values[frame])
    rig.update_tag()
    bpy.context.view_layer.update()


def feet_gap(body, points, at):
    """{side: the lowest sole vertex (the foot's vertices whose normals point down; its 60 lowest) above the ground under
    it, m}."""
    gaps = {}
    for side, mask in body.feet.items():
        sole = mask & (body.normals[:, 2] < -0.5)
        feet = points[sole if sole.any() else mask]
        low = feet[np.argsort(feet[:, 2])[:60]]
        gaps[side] = min(p[2] - at(p[0], p[1]) for p in low)
    return gaps


def contact_problems(seat_gap, back_gap, feet, buttocks=None):
    """The contacts out of tolerance: the seat, the buttocks and the planted soles `feet` outside TOUCH; the back into
    the bench past FLOAT_LIMIT."""
    gaps = [('seat', seat_gap), *([('buttocks', buttocks)] if buttocks is not None else []), *feet.items()]
    problems = [f"{k} {v * 100:+.1f} cm (touch {TOUCH[0] * 100:+.0f}..{TOUCH[1] * 100:+.1f})"
                for k, v in gaps if not TOUCH[0] <= v <= TOUCH[1]]
    if back_gap < -FLOAT_LIMIT:
        problems.append(f"back {back_gap * 100:+.1f} cm into the bench")
    return problems
