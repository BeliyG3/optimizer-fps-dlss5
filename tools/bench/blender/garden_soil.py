"""The garden's soil bed (build_lab_garden.py): `garden_soil`, a flat static mesh 1 cm above the yard ground, under
the grass, with a ragged outline onto the concrete.

The outline is the 0.5 level of a field on a CELL grid over the yard inside the fence, traced by marching squares (so
it is smooth, not stepped):
- the strip along the route: out to SOIL_HALF_WIDTH from it, its edge wandering by noise;
- the grass's own zones (garden_layout.grass_zone: the band, the fence, wall and tree patches and their stragglers),
  widened by MARGIN and blurred over BLEND, so no clump stands on concrete;
- fine noise across the BLEND-wide transition (only there: no specks on the open concrete, no holes inside) breaks
  the edge into a ragged strip of soil tongues, bays and small islands (islands under MIN_ISLAND m² are dropped)
  instead of a straight line.
UVs: world xy / 5 m (garden_ground's soil tile). Blender axes, metres.
"""
import bpy
import numpy as np

import garden_layout as layout
from garden_ground import SOIL_PERIOD
from garden_path import distance_to_route

SOIL_HALF_WIDTH = 8.5   # the strip along the route (its mean edge)
EDGE_WANDER = 0.8       # metres the strip's edge wanders (coarse noise)
MARGIN = 0.4            # soil reaches this far past the grass's zones
BLEND = 1.2             # metres over which the field falls from 1 to 0 at the edge (the ragged strip's width)
CELL = 0.25
MIN_ISLAND = 0.08       # m²
Z = 0.01


def _dilate(mask, cells):
    """mask widened by a disc of radius cells (no wrap at the edges)."""
    n, m = mask.shape
    padded = np.pad(mask, cells)
    out = np.zeros_like(mask)
    for i in range(-cells, cells + 1):
        for j in range(-cells, cells + 1):
            if i * i + j * j <= cells * cells:
                out |= padded[cells + i:cells + i + n, cells + j:cells + j + m]
    return out


def _blur(values, cells):
    """Box blur of radius cells along both axes (edges clamped)."""
    for axis in (0, 1):
        padded = np.pad(values, [(cells, cells) if a == axis else (0, 0) for a in (0, 1)], mode='edge')
        total = np.cumsum(padded, axis=axis)
        total = np.insert(total, 0, 0, axis=axis)
        n = values.shape[axis]
        hi = np.take(total, np.arange(2 * cells + 1, n + 2 * cells + 1), axis=axis)
        lo = np.take(total, np.arange(0, n), axis=axis)
        values = (hi - lo) / (2 * cells + 1)
    return values


def field(polyline, bounds, boxes, walls, tree_spots, seed=11):
    """(xs, ys, F): the grid's coordinates and the field (soil where F > 0.5), F[i, j] at (xs[i], ys[j])."""
    x0, y0, x1, y1 = bounds
    xs = np.arange(x0, x1 + CELL / 2, CELL)
    ys = np.arange(y0, y1 + CELL / 2, CELL)
    gx, gy = np.meshgrid(xs, ys, indexing='ij')
    points = np.column_stack([gx.ravel(), gy.ravel()])
    rng = np.random.default_rng(seed)
    zone = layout.grass_zone(points, polyline, bounds, boxes, walls, tree_spots).reshape(gx.shape)
    zone = _blur(_dilate(zone, round(MARGIN / CELL)).astype(float), round(BLEND / 2 / CELL))
    wander = EDGE_WANDER * layout.noise(points, rng, 0.5)
    strip = np.clip((SOIL_HALF_WIDTH + wander - distance_to_route(points, polyline)) / BLEND + 0.5, 0, 1)
    rough = (0.3 * layout.noise(points, rng, 2.5) + 0.15 * layout.noise(points, rng, 6.0)).reshape(gx.shape)
    base = np.maximum(zone, strip.reshape(gx.shape))
    return xs, ys, base + rough * np.clip(4 * np.minimum(base, 1 - base), 0, 1)  # the noise only in the transition


def contour_mesh(xs, ys, values, level=0.5):
    """(vertices, faces) of the region values > level by marching squares: per cell its corners inside and the
    crossings on its edges, counter-clockwise (normals +z); shared corners and crossings are one vertex."""
    inside = values > level
    index, verts, faces = {}, [], []

    def corner(i, j):
        key = (i, j)
        if key not in index:
            index[key] = len(verts); verts.append((xs[i], ys[j]))
        return index[key]

    def crossing(a, b):
        key = (min(a, b), max(a, b))
        if key not in index:
            (ai, aj), (bi, bj) = key
            t = (level - values[ai, aj]) / (values[bi, bj] - values[ai, aj])
            index[key] = len(verts)
            verts.append((xs[ai] + t * (xs[bi] - xs[ai]), ys[aj] + t * (ys[bj] - ys[aj])))
        return index[key]

    cells = np.argwhere(inside[:-1, :-1] | inside[1:, :-1] | inside[1:, 1:] | inside[:-1, 1:])
    for i, j in cells:
        ring = ((i, j), (i + 1, j), (i + 1, j + 1), (i, j + 1))
        face = []
        for k in range(4):
            a, b = ring[k], ring[(k + 1) % 4]
            if inside[a]:
                face.append(corner(*a))
            if inside[a] != inside[b]:
                face.append(crossing(a, b))
        faces.append(face)
    return np.array(verts), faces


def _drop_islands(verts, faces):
    """Faces of the connected components (sharing a vertex) of at least MIN_ISLAND m²."""
    parent = list(range(len(verts)))

    def find(v):
        while parent[v] != v:
            parent[v] = parent[parent[v]]; v = parent[v]
        return v

    for face in faces:
        for v in face[1:]:
            ra, rb = find(face[0]), find(v)
            if ra != rb:
                parent[ra] = rb
    area = {}
    for face in faces:
        p = verts[face]
        a = 0.5 * abs(np.dot(p[:, 0], np.roll(p[:, 1], -1)) - np.dot(p[:, 1], np.roll(p[:, 0], -1)))
        area[find(face[0])] = area.get(find(face[0]), 0.0) + a
    kept = [f for f in faces if area[find(f[0])] >= MIN_ISLAND]
    islands = sum(1 for a in area.values() if a >= MIN_ISLAND)
    return kept, islands, sum(a for a in area.values() if a >= MIN_ISLAND)


def build(polyline, bounds, boxes, walls, tree_spots, material, collection):
    """garden_soil with the given material."""
    xs, ys, values = field(polyline, bounds, boxes, walls, tree_spots)
    verts, faces = contour_mesh(xs, ys, values)
    faces, islands, area = _drop_islands(verts, faces)
    used = np.unique(np.concatenate(faces))
    remap = np.full(len(verts), -1); remap[used] = np.arange(len(used))
    mesh = bpy.data.meshes.new("garden_soil")
    mesh.from_pydata([(x, y, Z) for x, y in verts[used]], [], [remap[f].tolist() for f in faces])
    mesh.materials.append(material)
    uv = mesh.uv_layers.new(name="UVMap")
    loops = np.array([loop.vertex_index for loop in mesh.loops])
    uv.data.foreach_set("uv", (verts[used][loops] / SOIL_PERIOD).astype(np.float32).ravel())
    mesh.validate()
    obj = bpy.data.objects.new("garden_soil", mesh)
    collection.objects.link(obj)
    print(f"[garden] soil: {len(faces)} faces, {sum(len(f) - 2 for f in faces)} triangles, {islands} pieces, "
          f"{area:.0f} m²", flush=True)
    return obj
