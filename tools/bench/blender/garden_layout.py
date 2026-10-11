"""Where the garden's plants stand (build_lab_garden.py): the trees and ferns, and the grass clumps' points.

Blender axes, metres. Inputs are the route (garden_path.route), the yard's bounds inside the fence and the footprints
of the yard's objects (vertices below 0.5 m of the buildings, crates and poles). All positions are seeded.

- Trees: two beside the path where the combo camera looks (it looks outward from its anchor, so "beside" is the
  outer side: the route point at a frame plus the view direction times a distance), two further out in its view.
- Ferns: two by each tree, the rest along building walls the route faces.
- Grass: a Poisson process thinned by density() with a minimum spacing (dart throwing on a hash grid): dense beside
  the path (thinning towards its edge), patches along the fence, the walls and under the trees, all with ragged edges
  and sparse stragglers past them.
"""
import math

import numpy as np

from build_lab_outdoor import combo_eye
from garden_path import PATH_WIDTH, distance_to_route

CLEAR = 0.3            # no clump centre within this distance of the path's edge
RAMP = (0.3, 1.6)      # grass density ramps up between these distances from the path's edge (thinning near it)
BAND = (3.5, 5.5)      # full density out to the first distance from the edge, none past the second, both ragged
RAGGED = (1.3, 0.35)   # metres the band's and the patches' edges wander: coarse and fine noise
NEAR_WANDER = 0.9      # metres the start of that ramp wanders outward (a ragged line of thinning grass)
HALO = 2.0             # stragglers out to this distance past the band's and the patches' edges
SOIL_EDGE = 8.2        # no band or stragglers past this distance from the route (the soil bed's strip, garden_soil.py)
DENSE, PATCH, STRAGGLER = 26.0, 14.0, 0.8  # clumps per m²: beside the path, in the patches, stragglers (v2.1: 2x v2)
SPACING = 0.14         # minimum distance between clump centres (v2: 0.2, which capped the density near 17/m²)
# (frame of the route point, distance outward along the camera's view): two trees beside the path, two further out.
TREES = ((975, 4.6), (1045, 5.0), (1003, 11.5), (925, 10.0))
TREE_TURN = (0.0, 2.1, 4.0, 5.3)  # radians, then +-15 % scale on top of the base scale
TREE_SCALE = (1.0, 1.12, 0.88, 1.06)


def view_point(frame, distance):
    """The combo eye at frame plus distance along its (horizontal) view direction."""
    x, y, yaw = combo_eye(frame)
    a = math.radians(yaw)
    return x + distance * math.sin(a), y + distance * math.cos(a)


def footprints(objects):
    """(x0, y0, x1, y1) per object of its vertices below 0.5 m (world space)."""
    boxes = []
    for obj in objects:
        co = np.array([obj.matrix_world @ v.co for v in obj.data.vertices])
        low = co[co[:, 2] < 0.5]
        if len(low):
            boxes.append((*low[:, :2].min(axis=0), *low[:, :2].max(axis=0)))
    return np.array(boxes)


def _box_distance(points, boxes):
    """Distance from points (n x 2) to the nearest box (0 inside)."""
    dx = np.maximum(np.maximum(boxes[None, :, 0] - points[:, None, 0], points[:, None, 0] - boxes[None, :, 2]), 0)
    dy = np.maximum(np.maximum(boxes[None, :, 1] - points[:, None, 1], points[:, None, 1] - boxes[None, :, 3]), 0)
    return np.hypot(dx, dy).min(axis=1)


def trees(polyline, boxes):
    """[(x, y, turn, scale)] of the four trees; raises if one stands on the path or near an obstacle."""
    result = []
    for (frame, distance), turn, scale in zip(TREES, TREE_TURN, TREE_SCALE):
        x, y = view_point(frame, distance)
        to_path = distance_to_route(np.array([[x, y]]), polyline)[0]
        to_box = _box_distance(np.array([[x, y]]), boxes)[0]
        print(f"[garden] tree at ({x:.2f}, {y:.2f}): {to_path:.2f} m from the route, {to_box:.2f} m from obstacles",
              flush=True)
        if to_path < PATH_WIDTH / 2 + 2.5 or to_box < 3.0:
            raise RuntimeError("a tree is too close to the path or an obstacle")
        result.append((x, y, turn, scale))
    return result


def ferns(tree_spots, rng):
    """[(x, y, turn, scale, variant)]: two by each tree, then along the walls of building_1 and building_3 (west faces)
    and the hall's east wall south of the opening."""
    spots = []
    for x, y, _, _ in tree_spots:
        for k in range(2):
            a = rng.uniform(0, 2 * math.pi)
            r = rng.uniform(1.3, 2.0)
            spots.append((x + r * math.cos(a), y + r * math.sin(a)))
    spots += [(37.4, 4.0), (37.4, 8.5), (37.4, 12.0), (33.4, -18.5), (33.4, -13.0), (10.6, -3.0), (10.6, -8.5)]
    return [(x, y, rng.uniform(0, 2 * math.pi), rng.uniform(1.1, 1.4), int(rng.integers(0, 4))) for x, y in spots]


def noise(points, rng, scale=0.45, waves=6):
    """Smooth noise in about [-1, 1] from a few random plane waves (scale: radians per metre)."""
    total = np.zeros(len(points))
    for _ in range(waves):
        a, phase = rng.uniform(0, 2 * math.pi), rng.uniform(0, 2 * math.pi)
        k = scale * rng.uniform(0.6, 1.6)
        total += np.sin(k * (points[:, 0] * math.cos(a) + points[:, 1] * math.sin(a)) + phase)
    return total / waves * 2.2


def density(points, polyline, bounds, boxes, walls, tree_spots, rng):
    """Clumps per m² at points (n x 2), and the points' distances from the path's edge.

    The band beside the path and the patches (fence, walls, trees) have ragged edges: their distances are shifted by
    coarse and fine noise. Past those edges, out to HALO, stragglers stand in small groups (a fine noise mask)."""
    edge = distance_to_route(points, polyline) - PATH_WIDTH / 2
    wander = RAGGED[0] * noise(points, rng, 0.35) + RAGGED[1] * noise(points, rng, 2.2)
    band_edge = edge + wander
    near = edge - NEAR_WANDER * np.abs(noise(points, rng, 1.1))  # the ramp's start wanders away from the path
    beside = np.clip((near - RAMP[0]) / (RAMP[1] - RAMP[0]), 0, 1) ** 1.5 * np.clip((BAND[1] - band_edge) / (BAND[1] - BAND[0]), 0, 1)
    x0, y0, x1, y1 = bounds
    to_fence = np.minimum.reduce([x1 - points[:, 0], points[:, 1] - y0, y1 - points[:, 1]])
    to_wall = _box_distance(points, walls)
    to_tree = np.min([np.hypot(points[:, 0] - x, points[:, 1] - y) for x, y, _, _ in tree_spots], axis=0)
    patchy = np.clip((noise(points, rng) - 0.15) * 2.5, 0, 1)
    rim = np.minimum(to_fence - 1.1, to_wall - 1.2) + 0.5 * wander  # < 0 inside the strips along fence and walls
    edges = np.clip(0.5 - rim / 0.6, 0, 1) * patchy
    under = np.clip((3.0 - to_tree + 0.6 * wander) / 1.0, 0, 1)
    core = np.maximum(beside, np.maximum(edges, under))
    d = DENSE * beside + PATCH * np.maximum(edges, under)
    halo = np.maximum.reduce([band_edge < BAND[1] + HALO, (rim < HALO) & (patchy > 0), to_tree + 0.6 * wander < 3.0 + HALO])
    groups = np.clip(noise(points, rng, 3.0) * 1.5, 0, 1)
    d += STRAGGLER * 2.5 * groups * ((halo & (core < 0.05)) | (beside < 0.05) & (edge < RAMP[1]))
    d[(edge > SOIL_EDGE - PATH_WIDTH / 2) & (core < 0.05) & ~((rim < HALO) | (to_tree < 3.0 + HALO))] = 0
    d[edge < CLEAR] = 0
    d[_box_distance(points, boxes) < 0.35] = 0
    return d, edge


def grass_zone(points, polyline, bounds, boxes, walls, tree_spots, seed=5):
    """True where grass_points (same seed) may put a clump: its density map, with the same noise, above zero."""
    return density(points, polyline, bounds, boxes, walls, tree_spots, np.random.default_rng(seed + 1))[0] > 0


def grass_points(polyline, bounds, boxes, walls, tree_spots, seed=5):
    """Clump centres (n x 2) and their distances from the path's edge: candidates of a Poisson process at the peak
    density, kept with probability density / peak, then dart throwing at SPACING in random order."""
    rng = np.random.default_rng(seed)
    x0, y0, x1, y1 = bounds
    peak = DENSE + PATCH
    count = rng.poisson(peak * (x1 - x0) * (y1 - y0))
    candidates = np.column_stack([rng.uniform(x0 + 0.1, x1 - 0.1, count), rng.uniform(y0 + 0.1, y1 - 0.1, count)])
    d, edge = density(candidates, polyline, bounds, boxes, walls, tree_spots, np.random.default_rng(seed + 1))
    keep = rng.uniform(0, peak, count) < d
    candidates, edge = candidates[keep], edge[keep]
    grid, chosen, size = {}, [], SPACING / math.sqrt(2)  # at most one point per cell
    for index, p in enumerate(candidates):
        cell = (int(p[0] // size), int(p[1] // size))
        near = (grid.get((cell[0] + i, cell[1] + j)) for i in range(-2, 3) for j in range(-2, 3))
        if all(q is None or (q[0] - p[0]) ** 2 + (q[1] - p[1]) ** 2 >= SPACING ** 2 for q in near):
            grid[cell] = p
            chosen.append(index)
    points, edge = candidates[chosen], edge[chosen]
    print(f"[garden] grass: {count} candidates, {keep.sum()} kept by density, {len(points)} after spacing; "
          f"closest to the path's edge {edge.min():.3f} m", flush=True)
    return points, edge
