"""The garden's card-grass clumps (build_lab_garden.py), textured by garden_grass_atlas.py's atlas.

Clump: 4-5 quads at varied angles (k * 180 / n degrees, each jittered by up to 40 % of the step), 0.3-0.6 m tall and
0.5-0.9 m wide (about 1.5 x the height, the atlas cells' aspect). Each quad stands 2-9 cm off the clump's centre on one
side and leans 6-18 degrees outward to that side (its top edge shifted, the quad stays planar). Its bottom vertices lie
at the clump's base and its top ones above them (garden v2.4: the base is the lowest surface under the clump's centre and
its quads' bottom corners, raycast onto the yard, soil and path, less SINK, so no card's bottom edge shows a gap on
slopes or the soil bed's raised edge; build_nodes reports the largest gap, which must not be above 0): bench12's wind finds every quad's bottom and top as a connected
component. Each quad shows a random cell of the atlas, randomly mirrored; a clump gets one of four tints (materials).
Near the path a clump is narrowed so that it reaches at most OVERHANG over the path's edge. Clumps are joined into
`veg_grass_NN` meshes of at most PER_NODE clumps, grouped by 8 m cells (the wind's node names need "grass").
Materials alphaMode MASK (Math Round on the atlas alpha), cutoff 0.5, double-sided.
"""
import math

import bpy
import numpy as np

from garden_grass_atlas import CELLS_X, CELLS_Y, HEIGHT, WIDTH

TINTS = ((1.0, 1.0, 1.0), (0.86, 0.97, 0.80), (1.0, 0.95, 0.78), (0.90, 0.93, 0.88))
PER_NODE = 500
CLUMP_HEIGHT = (0.3, 0.6)
CLUMP_WIDTH = (0.5, 0.9)
ASPECT = (1.35, 1.75)       # width / height before clamping to CLUMP_WIDTH
QUADS = (4, 5)
OFFSET = (0.02, 0.09)       # metres off the centre
TILT = (6.0, 18.0)          # degrees outward
OVERHANG = 0.15             # metres a clump may reach over the path's edge
SINK = 0.04                 # metres the base goes below the lowest surface under the clump (v2.4)


def grass_materials(atlas_path):
    """Four materials: atlas x tint (Mix MULTIPLY, the exporter's baseColorFactor), alpha Round (MASK, cutoff 0.5)."""
    image = bpy.data.images.load(str(atlas_path), check_existing=True)
    image.pack()
    materials = []
    for k, tint in enumerate(TINTS):
        material = bpy.data.materials.new(f"veg_grass_{k}")
        material.use_nodes = True
        material.use_backface_culling = False
        nodes, links = material.node_tree.nodes, material.node_tree.links
        bsdf = nodes["Principled BSDF"]
        bsdf.inputs["Roughness"].default_value = 0.8
        texture, mix, cut = nodes.new("ShaderNodeTexImage"), nodes.new("ShaderNodeMix"), nodes.new("ShaderNodeMath")
        texture.image = image
        mix.data_type, mix.blend_type = "RGBA", "MULTIPLY"
        mix.inputs["Factor"].default_value = 1.0
        mix.inputs["B"].default_value = (*tint, 1.0)
        cut.operation = 'ROUND'
        links.new(texture.outputs["Color"], mix.inputs["A"])
        links.new(mix.outputs["Result"], bsdf.inputs["Base Color"])
        links.new(texture.outputs["Alpha"], cut.inputs[0])
        links.new(cut.outputs["Value"], bsdf.inputs["Alpha"])
        materials.append(material)
    return materials


def _cell_uvs(cell, flip):
    """The four corners' UVs (bottom left, bottom right, top right, top left) of an atlas cell, one texel inset."""
    u0, u1 = (cell % CELLS_X) / CELLS_X + 1 / WIDTH, (cell % CELLS_X + 1) / CELLS_X - 1 / WIDTH
    v0, v1 = 1 - (cell // CELLS_X + 1) / CELLS_Y + 1 / HEIGHT, 1 - (cell // CELLS_X) / CELLS_Y - 1 / HEIGHT
    ua, ub = (u1, u0) if flip else (u0, u1)
    return [(ua, v0), (ub, v0), (ub, v1), (ua, v1)]


def _clump(rng, x, y, edge):
    """The quads of one clump at (x, y), `edge` metres from the path's edge: (verts, uvs, material index, narrowed)."""
    height = rng.uniform(*CLUMP_HEIGHT)
    width = float(np.clip(height * rng.uniform(*ASPECT), *CLUMP_WIDTH))
    count = int(rng.integers(QUADS[0], QUADS[1] + 1))
    start = rng.uniform(0.0, math.pi)
    local, uvs = [], []
    for k in range(count):
        a = start + (k + rng.uniform(-0.4, 0.4)) * math.pi / count
        along, normal = np.array((math.cos(a), math.sin(a))), np.array((-math.sin(a), math.cos(a)))
        side = 1.0 if rng.uniform() < 0.5 else -1.0
        half, h = 0.5 * width * rng.uniform(0.85, 1.0), height * rng.uniform(0.85, 1.05)
        centre = side * rng.uniform(*OFFSET) * normal + rng.uniform(-0.05, 0.05) * along
        lean = side * h * math.tan(math.radians(rng.uniform(*TILT))) * normal
        bottom = (centre - half * along, centre + half * along)
        local += [(*bottom[0], 0.0), (*bottom[1], 0.0), (*(bottom[1] + lean), h), (*(bottom[0] + lean), h)]
        uvs += _cell_uvs(int(rng.integers(0, CELLS_X * CELLS_Y)), rng.uniform() < 0.5)
    local = np.array(local)
    reach = float(np.hypot(local[:, 0], local[:, 1]).max())
    scale = min(1.0, (edge + OVERHANG) / reach)
    local[:, :2] *= scale
    verts = [(x + vx, y + vy, vz) for vx, vy, vz in local]
    return verts, uvs, int(rng.integers(0, len(TINTS))), scale < 1.0


def _ground(verts, at):
    """Moves the clump (z = 0 at its base) onto the lowest surface under its centre and bottom corners, less SINK.
    Returns (verts, the largest bottom-corner gap above the surface: <= -SINK)."""
    bottom = [(x, y) for x, y, z in verts if z == 0.0]
    centre = np.mean([v[:2] for v in verts], axis=0)
    floor = np.array([at(x, y) for x, y in bottom + [tuple(centre)]], float)  # NaN: off the yard (by the hall's wall)
    base = float(np.nanmin(floor)) - SINK
    return [(x, y, z + base) for x, y, z in verts], float(np.nanmax(base - floor[:-1]))


def build_nodes(points, edges, materials, collection, at, seed=11):
    """The clumps at points (n x 2), `edges` their distances from the path's edge, joined into veg_grass_NN meshes by
    8 m cells (<= PER_NODE clumps each); `at(x, y)` is the surface height (garden_props_ground.surface, NaN off the yard). Returns
    (objects, quads, ground: the bases and the largest gap of a card's bottom edge above the surface). Raises if a
    bottom edge is above the surface."""
    rng = np.random.default_rng(seed)
    order = np.lexsort((points[:, 1], np.floor(points[:, 1] / 8), np.floor(points[:, 0] / 8)))
    cells = np.floor(points[order] / 8).astype(int)
    groups, current = [], [order[0]]
    for i in range(1, len(order)):
        if len(current) >= PER_NODE or tuple(cells[i]) != tuple(cells[i - 1]) and len(current) >= PER_NODE // 2:
            groups.append(current)
            current = []
        current.append(order[i])
    groups.append(current)
    objects, quads, narrowed, gaps, bases = [], 0, 0, [], []
    for n, group in enumerate(groups):
        verts, uvs, slots = [], [], []
        for i in group:
            v, uv, slot, narrow = _clump(rng, *points[i], edges[i])
            v, gap = _ground(v, at)
            gaps.append(gap)
            bases.append(min(z for _, _, z in v))
            verts += v
            uvs += uv
            slots += [slot] * (len(v) // 4)
            narrowed += narrow
        mesh = bpy.data.meshes.new(f"veg_grass_{n:02d}")
        mesh.from_pydata(verts, [], [(4 * q, 4 * q + 1, 4 * q + 2, 4 * q + 3) for q in range(len(verts) // 4)])
        for material in materials:
            mesh.materials.append(material)
        mesh.polygons.foreach_set("material_index", slots)
        layer = mesh.uv_layers.new(name="UVMap")
        layer.data.foreach_set("uv", np.array(uvs, np.float32).ravel())
        mesh.validate()
        obj = bpy.data.objects.new(f"veg_grass_{n:02d}", mesh)
        collection.objects.link(obj)
        objects.append(obj)
        quads += len(verts) // 4
    print(f"[garden] grass: {len(points)} clumps in {len(objects)} nodes "
          f"({min(len(g) for g in groups)}-{max(len(g) for g in groups)} each), {quads} quads, {2 * quads} triangles; "
          f"{narrowed} clumps narrowed beside the path", flush=True)
    ground = dict(sink=SINK, base_z=[round(min(bases), 4), round(max(bases), 4)], max_bottom_gap=round(max(gaps), 4))
    print(f"[garden] grass on the ground: bases z {ground['base_z'][0]:+.3f}..{ground['base_z'][1]:+.3f} m, largest gap of a "
          f"card's bottom edge above the surface {ground['max_bottom_gap'] * 100:+.1f} cm (must be <= 0)", flush=True)
    if ground['max_bottom_gap'] > 0:
        raise RuntimeError("a grass card's bottom edge floats above the surface")
    return objects, quads, ground
