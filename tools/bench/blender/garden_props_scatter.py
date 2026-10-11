"""The garden's scattered props (build_lab_garden.py, garden v2.3/v2.4): ~120 more placements of the 27 templates where
the combo camera and a flying player look, after the 31 hand-placed ones (garden_props_layout.PROPS), and (v2.4, FAR)
about 100 in the distant parts of the garden and the yard, so the view into the distance is not bare.

Each cluster draws `count` props from its templates (weights) at seeded random spots, headings and scales, and keeps a
candidate only if garden_props_layout.Site.problems finds nothing against every prop placed so far with SPACING between
footprints (the same rules the build checks: off the path strip and the combo eye, clear of buildings, walls, trunks
and ferns, inside the fence, rocks and branches on the soil bed). Where:
- ('band', d0, d1, f0, f1): d0..d1 m from the route on either side, along the fraction f0..f1 of its length;
- ('ring', k, r0, r1): r0..r1 m around tree k (garden_layout.TREES order);
- ('box', x0, y0, x1, y1): a rectangle (Blender x, y).
Benches and tables face the path (garden_props_layout.face); everything else turns freely. Scales: SCALE per template
prefix (the first set's rocks are large: scaled down most). Blender axes, metres.
"""
import math

import numpy as np

from garden_props_layout import face

SEED, ATTEMPTS, SPACING = 23, 400, 0.12
ROCKS = {'rock_s2_08': 3, 'rock_s2_09': 3, 'rock_s2_10': 2, 'rock_s2_11': 2, 'rock_s2_12': 3,
         'rock_s1_03': 1, 'rock_s1_04': 1, 'rock_s1_06': 1}
BRANCHES = {'branches_a': 2, 'branches_b': 2, 'branches_c': 2}
TOOLS = {'barrel_red': 3, 'barrel_blue': 3, 'bucket': 3, 'jerrycan': 2, 'cement_bag': 3, 'pot': 1, 'crates': 1,
         'compost_bags': 1, 'hand_truck': 1}
SMALL_TOOLS = {'barrel_red': 2, 'barrel_blue': 2, 'bucket': 2, 'jerrycan': 1, 'cement_bag': 2}
SCALE = {'rock_s1': (0.35, 0.55), 'rock_s2': (0.45, 0.9), 'branches': (0.85, 1.15), 'boulder': (0.55, 0.75),
         'trunk': (0.75, 0.95)}
FACING = {'bench': 0, 'bench_double': 0, 'picnic_table': 90, 'table_set': 90}  # extra degrees, as in PROPS
CLUSTERS = (
    # Two more benches facing the path (first, while there is room): one at its northern part, one at its southern.
    (('band', 2.2, 3.0, 0.05, 0.35), 1, {'bench_double': 1}),
    (('band', 2.2, 3.0, 0.65, 0.95), 1, {'bench': 1}),
    # Both edges of the path, all along it: rocks and dry branches in the grass.
    (('band', 1.9, 3.2, 0.0, 1.0), 36, {**ROCKS, **BRANCHES}),
    # Further out where the camera looks: more rocks and branches, a boulder and a trunk now and then.
    (('band', 3.2, 6.5, 0.0, 1.0), 22, {**ROCKS, **BRANCHES, 'boulder': 1, 'trunk': 1}),
    # Under the four trees: fallen branches and small rocks.
    *((('ring', k, 1.2, 3.2), 4, {**BRANCHES, 'rock_s2_08': 1, 'rock_s2_09': 1, 'rock_s2_12': 1}) for k in range(4)),
    # The garden's entrance by the hall's opening (y 2.4..10.4): yard things on both sides of the doorway.
    (('box', 10.0, -1.9, 13.5, 2.3), 8, TOOLS),
    (('box', 10.0, 10.5, 13.5, 14.8), 8, TOOLS),
    # Along the hall's east wall south of the entrance.
    (('box', 10.0, -11.6, 11.4, -2.0), 8, TOOLS),
    # Building 3's west face and building 1's (x 34 and 38).
    (('box', 32.0, -20.6, 33.9, -8.6), 5, TOOLS),
    (('box', 36.2, 2.1, 37.9, 13.9), 6, TOOLS),
    # By the crate stacks at (19, 12) and (27, -2).
    (('box', 17.6, 10.6, 23.0, 15.6), 5, SMALL_TOOLS),
    (('box', 25.8, -3.6, 31.6, 0.6), 4, SMALL_TOOLS),
)
FAR_TOOLS = {'barrel_red': 3, 'barrel_blue': 3, 'bucket': 2, 'jerrycan': 2, 'cement_bag': 3, 'crates': 2,
             'compost_bags': 2, 'hand_truck': 1, 'pot': 2}
SEATS = {'bench': 2, 'bench_double': 1, 'picnic_table': 2, 'table_set': 2}
FAR = (
    # Garden v2.4, after the v2.3 clusters (whose draws stay as they were): the distant parts of the garden and the yard.
    # The soil bed's far edges and further round the trees: rocks, branches, now and then a boulder or a trunk.
    (('band', 6.5, 9.5, 0.0, 1.0), 16, {**ROCKS, **BRANCHES, 'boulder': 1, 'trunk': 1}),
    *((('ring', k, 3.2, 5.0), 2, {**ROCKS, **BRANCHES}) for k in range(4)),
    # The yard north of the garden (seats and yard things), around building 2 and building 0 (its three free faces).
    (('box', 10.5, 16.5, 23.5, 26.5), 10, {**FAR_TOOLS, **SEATS}),
    (('box', 10.3, 26.5, 12.3, 35.8), 5, FAR_TOOLS),
    (('box', 20.7, 26.5, 23.6, 35.8), 5, FAR_TOOLS),
    (('box', 24.0, 18.3, 36.0, 19.8), 7, FAR_TOOLS),
    (('box', 36.2, 20.0, 38.5, 32.0), 5, FAR_TOOLS),
    (('box', 24.0, 32.2, 36.0, 35.8), 6, FAR_TOOLS),
    # The north-east corner, around building 1 and the strip between it and building 3.
    (('box', 38.5, 15.5, 49.2, 35.8), 14, {**FAR_TOOLS, **SEATS}),
    (('box', 38.0, 0.2, 48.0, 1.8), 6, FAR_TOOLS),
    (('box', 38.0, 14.2, 48.0, 15.5), 5, FAR_TOOLS),
    (('box', 48.2, 2.0, 49.3, 14.0), 3, FAR_TOOLS),
    (('box', 36.5, -8.6, 49.2, 0.0), 10, {**FAR_TOOLS, **SEATS}),
    # Building 3's south and east faces, the south fence.
    (('box', 34.0, -23.1, 46.0, -21.2), 6, FAR_TOOLS),
    (('box', 46.2, -21.0, 49.3, -9.0), 5, FAR_TOOLS),
    (('box', 18.0, -23.1, 33.5, -21.5), 8, {**FAR_TOOLS, **ROCKS}),
)


def _route_frame(polyline):
    """(cumulative length per point, unit tangents per segment) of the route."""
    steps = np.diff(polyline, axis=0)
    lengths = np.hypot(steps[:, 0], steps[:, 1])
    return np.concatenate([[0.0], np.cumsum(lengths)]), steps / lengths[:, None]


def _spot(where, rng, polyline, trees):
    if where[0] == 'band':
        cumulative, tangents = _route_frame(polyline)
        _, d0, d1, f0, f1 = where
        at = rng.uniform(f0, f1) * cumulative[-1]
        k = min(int(np.searchsorted(cumulative, at, side='right')) - 1, len(tangents) - 1)
        tx, ty = tangents[k]
        px, py = polyline[k] + (at - cumulative[k]) * tangents[k]
        side, d = rng.choice((-1.0, 1.0)), rng.uniform(d0, d1)
        return px - side * d * ty, py + side * d * tx
    if where[0] == 'ring':
        x, y = trees[where[1]][:2]
        a, r = rng.uniform(0, 2 * math.pi), rng.uniform(where[2], where[3])
        return x + r * math.cos(a), y + r * math.sin(a)
    return rng.uniform(where[1], where[3]), rng.uniform(where[2], where[4])


def _scale(template, rng):
    for prefix, (low, high) in SCALE.items():
        if template.startswith(prefix):
            return float(rng.uniform(low, high))
    return float(rng.uniform(0.95, 1.05)) if template not in FACING else 1.0


def scatter(site, placed, polyline, trees):
    """Appends the clusters' props to placed (dicts as garden_props_layout.resolve's). Returns how many were added."""
    rng = np.random.default_rng(SEED)
    before = len(placed)
    for where, count, mix in CLUSTERS + FAR:
        names, weights = list(mix), np.array(list(mix.values()), float)
        added = 0
        for _ in range(count):
            for _ in range(ATTEMPTS):
                template = names[rng.choice(len(names), p=weights / weights.sum())]
                x, y = _spot(where, rng, polyline, trees)
                turn = face(x, y, polyline, FACING[template]) if template in FACING else rng.uniform(0, 2 * math.pi)
                prop = dict(template=template, x=float(x), y=float(y), z=0.0, turn=float(turn),
                            scale=_scale(template, rng), on=None)
                if not site.problems(prop, placed, SPACING)[0]:
                    placed.append(prop)
                    added += 1
                    break
        print(f"[garden] scatter {where[0]} {where[1:]}: {added} of {count}", flush=True)
    return len(placed) - before
