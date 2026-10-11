"""Where the garden's hand-placed props stand (build_lab_garden.py, garden v2.2/v2.3), and the checks that keep every
prop (these and garden_props_scatter.py's) off the path.

Blender axes, metres. Each entry of PROPS is (template, where, heading, scale):
- where: ('view', frame, d) is the combo eye at that frame plus d along its view (d > 0: the outer side of the path,
  which the camera looks at; d < 0: the inner side); ('at', x, y); ('on', k, lx, ly) stands on prop k (its local
  coordinates, e.g. gloves on a crate's lid; the height comes from garden_props_ground.settle, as for every prop);
- heading: ('face', extra) turns the template's front (local -y) towards the nearest route point, plus extra degrees;
  ('turn', degrees) is a world turn about z (0: the template as authored);
- scale: a factor on the template (1 for furniture; rocks and branches vary by up to 10 %).
The templates (garden_props.py) are centred on their footprint, front -y. Their sizes come from the built templates,
so the checks run on the real footprints: nothing within PATH_CLEAR of the route (the 2.5 m path and its blended
margin; the combo eye is on the route), off the yard's buildings, crates and poles and the hall's wall, clear of the
tree trunks and the ferns, inside the fence, no two props overlapping (except one standing on another), and the
"among the grass" ones on the soil bed (Site.problems; the scatter rejects its candidates with the same test).
"""
import math

import numpy as np

from garden_layout import _box_distance, view_point
from garden_path import distance_to_route

PATH_CLEAR = 1.5 + 0.25  # the path mesh's half width (path texture blended into the ground) plus a margin
TRUNK_CLEAR, FERN_CLEAR, PROP_GAP = 0.6, 0.75, 0.03
ON_SOIL = ('rock', 'boulder', 'trunk', 'branches')  # template prefixes that must stand on the soil bed (in the grass)

PROPS = (
    # Beside the path: a two-section bench on the outer side, a one-section bench on the inner, the picnic table under
    # tree 0 and the table with two chairs on the inner side.
    ('bench_double', ('view', 1015, 2.25), ('face', 0), 1.0),
    ('bench', ('view', 1052, -2.35), ('face', 0), 1.0),
    ('picnic_table', ('view', 958, 3.35), ('face', 90), 1.0),
    ('table_set', ('view', 986, -3.0), ('face', 90), 1.0),
    # Rocks, the boulder, the dead trunk and dry branches in the grass beside the path and under the trees.
    ('rock_s2_09', ('view', 1064, 3.5), ('turn', 20), 1.0),
    ('rock_s2_08', ('view', 1074, 4.3), ('turn', 75), 0.95),
    ('rock_s1_03', ('view', 1084, 3.7), ('turn', 140), 0.66),
    ('rock_s2_12', ('view', 1056, 6.4), ('turn', 250), 0.92),
    ('boulder', ('at', 25.9, -17.0), ('turn', 30), 1.0),
    ('trunk', ('at', 24.9, -10.9), ('turn', 28), 1.0),
    ('branches_a', ('at', 21.7, -16.1), ('turn', 60), 1.05),
    ('rock_s1_06', ('at', 27.5, -4.3), ('turn', 15), 0.68),
    ('rock_s2_10', ('at', 32.1, -8.6), ('turn', 200), 0.9),
    ('branches_b', ('at', 29.0, -8.9), ('turn', 120), 1.0),
    ('rock_s2_11', ('at', 24.4, 1.4), ('turn', 300), 0.85),
    ('branches_c', ('at', 21.0, -3.6), ('turn', 15), 0.95),
    ('rock_s1_04', ('at', 24.4, 7.0), ('turn', 95), 0.7),
    # By the hall's east wall, south of the opening: pots, barrels, a jerrycan and a bucket.
    ('pot', ('at', 10.46, 1.6), ('turn', 0), 1.0),
    ('barrel_red', ('at', 10.27, 0.3), ('turn', 180), 1.0),
    ('barrel_blue', ('at', 10.24, -0.33), ('turn', 270), 1.0),
    ('barrel_blue', ('at', 10.85, 0.02), ('turn', 90), 0.97),
    ('jerrycan', ('at', 10.08, -0.98), ('turn', 90), 1.0),
    ('bucket', ('at', 10.3, -1.55), ('turn', 160), 1.0),
    ('pot', ('at', 10.46, -5.7), ('turn', 180), 0.95),
    # By building 3's west face (x = 34): the hand truck against the wall, compost and cement bags, the crates with
    # gloves on a lid.
    ('hand_truck', ('at', 33.62, -14.55), ('turn', 270), 1.0),
    ('compost_bags', ('at', 33.48, -16.45), ('turn', 90), 1.0),
    ('cement_bag', ('at', 32.62, -15.97), ('turn', 4), 1.0),
    ('cement_bag', ('at', 32.62, -16.84), ('turn', -6), 1.0),
    ('crates', ('at', 33.47, -11.2), ('turn', 90), 1.0),
    ('cement_bag', ('on', 26, -0.03, -0.43), ('turn', 87), 1.0),  # across the two bags below
    ('gloves', ('on', 28, -0.5, 0.02), ('turn', 25), 1.0),  # on crate a's lid (crate a at local x -0.5)
)


def _nearest_route_point(x, y, polyline):
    a, b = polyline[:-1], polyline[1:]
    ab = b - a
    t = np.clip((((x, y) - a) * ab).sum(1) / (ab * ab).sum(1), 0, 1)
    points = a + t[:, None] * ab
    return points[np.argmin(np.hypot(points[:, 0] - x, points[:, 1] - y))]


def face(x, y, polyline, extra_degrees):
    """The turn that points a template's front (local -y) at the nearest route point, plus extra degrees."""
    px, py = _nearest_route_point(x, y, polyline)
    return (math.atan2(py - y, px - x) + math.pi / 2 + math.radians(extra_degrees)) % (2 * math.pi)


def resolve(polyline):
    """[dict(template, x, y, z, turn, scale, on)] of PROPS in order (on: the index of the prop it stands on; z is set
    later by garden_props_ground.settle). name_all names them once the scattered props are added."""
    placed = []
    for template, where, heading, scale in PROPS:
        if where[0] == 'view':
            x, y = view_point(where[1], where[2])
        elif where[0] == 'at':
            x, y = where[1:]
        else:
            parent = placed[where[1]]
            c, s = math.cos(parent['turn']), math.sin(parent['turn'])
            lx, ly = where[2] * parent['scale'], where[3] * parent['scale']
            x, y = parent['x'] + c * lx - s * ly, parent['y'] + s * lx + c * ly
        turn = face(x, y, polyline, heading[1]) if heading[0] == 'face' else math.radians(heading[1]) % (2 * math.pi)
        placed.append(dict(template=template, x=float(x), y=float(y), z=0.0, turn=float(turn), scale=float(scale),
                           on=where[1] if where[0] == 'on' else None))
    return placed


def name_all(placed):
    """Names every placement prop_<template>_<k> (k counts per template, in order)."""
    counts = {}
    for prop in placed:
        k = counts.get(prop['template'], 0)
        counts[prop['template']] = k + 1
        prop['name'] = f"prop_{prop['template']}_{k}"
    return placed


def footprint(prop, half, step=0.1):
    """Points (n x 2) filling the prop's rotated footprint rectangle (half extents `half` in its local x, y)."""
    hx, hy = half[0] * prop['scale'], half[1] * prop['scale']
    u = np.linspace(-hx, hx, max(2, int(2 * hx / step) + 1))
    v = np.linspace(-hy, hy, max(2, int(2 * hy / step) + 1))
    gu, gv = np.meshgrid(u, v)
    c, s = math.cos(prop['turn']), math.sin(prop['turn'])
    return np.column_stack([prop['x'] + c * gu.ravel() - s * gv.ravel(), prop['y'] + s * gu.ravel() + c * gv.ravel()])


def _inside(points, prop, half, gap):
    """Points inside prop's footprint rectangle grown by gap."""
    c, s = math.cos(prop['turn']), math.sin(prop['turn'])
    dx, dy = points[:, 0] - prop['x'], points[:, 1] - prop['y']
    u, v = c * dx + s * dy, -s * dx + c * dy
    return (np.abs(u) <= half[0] * prop['scale'] + gap) & (np.abs(v) <= half[1] * prop['scale'] + gap)


class Site:
    """What a prop keeps clear of: the route and the combo eye, the yard's boxes and walls, the trunks and ferns, the
    fence (the yard's bounds), other props; ON_SOIL templates also stay on the soil bed."""

    def __init__(self, halves, polyline, bounds, boxes, walls, trees, ferns, on_soil, route_eyes, people=()):
        self.halves, self.polyline, self.bounds, self.on_soil, self.eyes = halves, polyline, bounds, on_soil, route_eyes
        # garden v2.4: (points, radius) of the people's walking lines and spots (garden_people_layout.avoid)
        self.people = [(points, radius, points.min(axis=0) - radius, points.max(axis=0) + radius)
                       for points, radius in people]
        self.obstacles = np.concatenate([boxes, walls])
        self.trunks = np.array([(x, y) for x, y, _, _ in trees])
        self.ferns = np.array([(x, y) for x, y, _, _, _ in ferns])

    def _nearest(self, points, spots):
        return float(np.hypot(points[:, None, 0] - spots[None, :, 0], points[:, None, 1] - spots[None, :, 1]).min())

    def problems(self, prop, others, gap=PROP_GAP):
        """(problems, clearances) of prop among others (the props before it). gap: the least distance between two
        footprints (a prop standing on another is checked by hand). Also off the people's lines and spots (v2.4)."""
        half = self.halves[prop['template']]
        points = footprint(prop, half)
        to_path = float(distance_to_route(points, self.polyline).min())
        to_box = float(_box_distance(points, self.obstacles).min())
        to_trunk, to_fern = self._nearest(points, self.trunks), self._nearest(points, self.ferns)
        x0, y0, x1, y1 = self.bounds
        name, found = prop.get('name', prop['template']), []
        if to_path < PATH_CLEAR:
            found.append(f"{name}: {to_path:.2f} m from the route (at least {PATH_CLEAR})")
        if _inside(self.eyes, prop, half, 0.5).any():
            found.append(f"{name}: the combo eye passes within 0.5 m of it")
        if to_box < 0.02:
            found.append(f"{name}: in a building, crate, pole or the hall's wall")
        if to_trunk < TRUNK_CLEAR or to_fern < FERN_CLEAR:
            found.append(f"{name}: {to_trunk:.2f} m from a trunk, {to_fern:.2f} m from a fern")
        if points[:, 0].min() < x0 or points[:, 0].max() > x1 - 0.2 or points[:, 1].min() < y0 + 0.2 \
                or points[:, 1].max() > y1 - 0.2:
            found.append(f"{name}: outside the fence")
        for other in others:
            stacked = prop['on'] is not None or other['on'] is not None
            if not stacked and _inside(points, other, self.halves[other['template']], gap).any():
                found.append(f"{name} overlaps {other.get('name', other['template'])}")
        low, high = points.min(axis=0), points.max(axis=0)
        for spots, radius, z0, z1 in self.people:
            if (low <= z1).all() and (high >= z0).all() and (
                    (points[:, None, :] - spots[None, :, :]) ** 2).sum(-1).min() < radius ** 2:
                found.append(f"{name}: in the people's way")
                break
        if prop['template'].startswith(ON_SOIL) and not self.on_soil(np.array([[prop['x'], prop['y']]]))[0]:
            found.append(f"{name}: not on the soil bed")
        return found, dict(to_path=round(to_path, 2), to_obstacle=round(to_box, 2), to_trunk=round(to_trunk, 2),
                           to_fern=round(to_fern, 2))


def check(placed, site):
    """Raises on any problem Site.problems finds. Returns per-prop clearances for the layout JSON."""
    problems, report = [], []
    for i, prop in enumerate(placed):
        found, clearances = site.problems(prop, placed[:i])
        problems += found
        report.append(dict(name=prop['name'], **clearances))
    if problems:
        raise RuntimeError("garden props:\n  " + "\n  ".join(problems))
    closest = min(report, key=lambda r: r['to_path'])
    print(f"[garden] props: {len(placed)} placed, closest to the route {closest['name']} at {closest['to_path']} m",
          flush=True)
    return report


def clear_grass(points, edge, low, radius=0.35):
    """The grass clumps (points, edge distances) without those within radius of the props' low points (n x 2)."""
    keep = np.ones(len(points), bool)
    for start in range(0, len(points), 2048):
        p = points[start:start + 2048]
        d2 = ((p[:, None, :] - low[None, :, :]) ** 2).sum(-1).min(axis=1)
        keep[start:start + 2048] = d2 >= radius ** 2
    print(f"[garden] grass: {int((~keep).sum())} clumps removed within {radius} m of the props", flush=True)
    return points[keep], edge[keep]
