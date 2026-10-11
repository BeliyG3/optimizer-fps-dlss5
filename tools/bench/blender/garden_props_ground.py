"""The garden's props on the ground (build_lab_garden.py, garden v2.3): each prop's height from what is under its feet.

A template's supports are its vertices within SUPPORT_BAND of its lowest point (a bench's feet, a barrel's rim, the bed
of a rock), at most MAX_SUPPORTS of them spread over the footprint. Under each support a ray goes down onto the surfaces
(the yard's concrete, the soil bed, the path; for a prop standing on another, that prop as well) and the highest hit is
the ground there. Then, with h the height each support needs to touch its ground:
- a rigid prop (lowest point on its base, garden_props `min`) rests on the support that needs the most height,
  EMBED into the surface, so no hairline gap shows; the others sink by the ground's unevenness under the footprint;
- a buried one (rocks, the boulder, the trunk and branches: modelled or sunk below their base on purpose) keeps that
  depth below the lowest ground under it.
gap = the prop's height minus the height a support needs: > 0 is that support above its ground, < 0 sunk into it
(beyond the designed depth). The build fails if a prop on the ground has a support FLOAT_LIMIT above it, a stacked one
rests FLOAT_LIMIT above its parent, or any sinks more than SINK_LIMIT.
"""
import math

import numpy as np
from mathutils import Vector
from mathutils.bvhtree import BVHTree

SURFACES = ('yard_ground', 'garden_soil', 'garden_path')
SUPPORT_BAND = 0.01
MAX_SUPPORTS = 96
EMBED = 0.005
BURIED = 0.005       # metres below its base a template's lowest point lies when it is buried by design
FLOAT_LIMIT, SINK_LIMIT = 0.02, 0.08


def local_vertices(mesh):
    co = np.empty(len(mesh.vertices) * 3)
    mesh.vertices.foreach_get('co', co)
    return co.reshape(-1, 3)


def _supports(co):
    """(supports n x 3, the template's lowest z): vertices within SUPPORT_BAND of it, one per 2 cm cell."""
    zmin = co[:, 2].min()
    points = co[co[:, 2] <= zmin + SUPPORT_BAND]
    points = points[np.unique(np.round(points[:, :2] / 0.02), axis=0, return_index=True)[1]]
    if len(points) > MAX_SUPPORTS:
        points = points[np.linspace(0, len(points) - 1, MAX_SUPPORTS).astype(int)]
    return points, float(zmin)


def _tree(obj):
    """The object's triangles in world space (its basis matrix: the props hang under an empty at the origin)."""
    matrix = obj.matrix_basis
    vertices = [matrix @ v.co for v in obj.data.vertices]
    return BVHTree.FromPolygons(vertices, [p.vertices[:] for p in obj.data.polygons])


def _ground(trees, x, y):
    """The highest hit straight down at (x, y) and its surface's name, or (None, None)."""
    best = (None, None)
    for name, tree in trees:
        hit = tree.ray_cast(Vector((x, y, 10.0)), Vector((0.0, 0.0, -1.0)), 20.0)[0]
        if hit is not None and (best[0] is None or hit.z > best[0]):
            best = (hit.z, name)
    return best


def surface(objects, missing=None):
    """A function (x, y) -> the highest z straight down onto the objects (garden v2.4: the people's and the grass's
    ground; the objects hang under an empty at the origin). Where nothing is under (x, y) it returns `missing`, or
    raises if that is None."""
    trees = [_tree(o) for o in objects]

    def at(x, y):
        z = _ground([(None, t) for t in trees], x, y)[0]
        if z is None:
            if missing is None:
                raise RuntimeError(f"no ground under ({x:.2f}, {y:.2f})")
            return missing
        return z
    return at


def settle(objects, placed, surfaces):
    """Sets every prop's z (placed[i]['z'] and its object's location) from what is under its supports. Returns
    [dict(name, z, gap_min, gap_max, supports, on)] in placement order (gaps in metres)."""
    ground = [(o.name, _tree(o)) for o in surfaces]
    cache, report = {}, []
    for obj, prop in zip(objects, placed):
        if obj.data.name not in cache:
            cache[obj.data.name] = _supports(local_vertices(obj.data))
        points, zmin = cache[obj.data.name]
        trees = ground if prop['on'] is None else ground + [(objects[prop['on']].name, _tree(objects[prop['on']]))]
        s, c, n = prop['scale'], math.cos(prop['turn']), math.sin(prop['turn'])
        need, names = [], set()
        for px, py, pz in points:
            z, name = _ground(trees, prop['x'] + s * (c * px - n * py), prop['y'] + s * (n * px + c * py))
            if z is None:
                raise RuntimeError(f"{prop['name']}: no ground under a support at local ({px:.2f}, {py:.2f})")
            need.append(z - s * (pz - zmin))
            names.add(name)
        need = np.array(need)
        buried = -zmin > BURIED
        prop['z'] = float(need.min() if buried else need.max() - EMBED)
        obj.location.z = prop['z']
        gaps = prop['z'] - need
        report.append(dict(name=prop['name'], z=round(prop['z'], 4), gap_min=round(float(gaps.min()), 4),
                           gap_max=round(float(gaps.max()), 4), supports=len(points), buried=buried,
                           on=sorted(names)))
    return report


def check(report, placed):
    """Raises if a prop floats or sinks (see the module's description); prints every prop's gaps."""
    problems = []
    for entry, prop in zip(report, placed):
        print(f"[garden] ground {entry['name']:24s} z {entry['z']:+.3f} gap {entry['gap_min'] * 100:+5.1f}.."
              f"{entry['gap_max'] * 100:+5.1f} cm, {entry['supports']} supports on {', '.join(entry['on'])}", flush=True)
        floats = entry['gap_min'] if prop['on'] is not None else entry['gap_max']
        if floats > FLOAT_LIMIT:
            problems.append(f"{entry['name']}: floats {floats * 100:.1f} cm above {', '.join(entry['on'])}")
        if entry['gap_min'] < -SINK_LIMIT:
            problems.append(f"{entry['name']}: sinks {-entry['gap_min'] * 100:.1f} cm into {', '.join(entry['on'])}")
    if problems:
        raise RuntimeError("garden props off the ground:\n  " + "\n  ".join(problems))
    worst = max((e for e, p in zip(report, placed) if p['on'] is None), key=lambda e: e['gap_max'])
    print(f"[garden] ground: {len(report)} props settled, largest support gap on the ground {worst['gap_max'] * 100:.1f} cm "
          f"({worst['name']}), deepest {min(e['gap_min'] for e in report) * 100:.1f} cm", flush=True)
