"""The garden's grass blade atlas (garden_grass.py, build_lab_garden.py).

12 clusters in 4 x 3 cells of a 4096 x 2048 RGBA image (cells 1024 x 683, as wide as 1.5 x their height, the clumps'
aspect). Each cluster: 14-22 tall blades fanning out from a narrow base and 14-24 short ones near the base (denser at
the base), each a ribbon along a cubic Bezier (lean outward, some arching over with a drooping tip), tapering to a point,
three vertices across (a lighter midrib), with varied heights, widths and greens; about a third have yellowed tips and a
few are dry all along; 7 of the 12 clusters carry 1-3 seed stems with a head of spikelets. Rendered once orthographically
by Cycles (emission only, transparent film, Standard view). The colour of transparent texels is filled from their
neighbours, so filtering at the alpha-cut edge does not darken it. Saved as garden_grass_atlas.png.
"""
import math

import bpy
import numpy as np

CELLS_X, CELLS_Y = 4, 3
WIDTH, HEIGHT = 4096, 2048
CELL_W, CELL_H = 1.5, 1.0  # a cell in the atlas scene's units (the image is 6 x 3 units)
HALF_SPAN = 0.70           # blades stay within this distance of the cell's centre line
GREENS = (np.array((0.13, 0.27, 0.035)), np.array((0.09, 0.24, 0.04)), np.array((0.07, 0.20, 0.06)))
BASE = np.array((0.025, 0.06, 0.012))
STRAW = np.array((0.50, 0.42, 0.16))


def _bezier(points, t):
    p0, p1, p2, p3 = points
    s = 1 - t
    position = s ** 3 * p0 + 3 * s * s * t * p1 + 3 * s * t * t * p2 + t ** 3 * p3
    tangent = 3 * s * s * (p1 - p0) + 6 * s * t * (p2 - p1) + 3 * t * t * (p3 - p2)
    return position, tangent / max(np.linalg.norm(tangent), 1e-9)


def _ribbon(points, width, colour_at, depth, steps=12, taper=0.7):
    """A ribbon along the Bezier `points` (x, z): 3 vertices across (edges darker, midrib lighter), tapering to a point.
    Returns (verts, faces, colours) with faces indexing verts from 0."""
    verts, colours = [], []
    for k in range(steps + 1):
        t = k / steps
        (px, pz), (dx, dz) = _bezier(points, t)
        nx, nz = dz, -dx
        half = 0.5 * width * max((1 - t) ** taper, 0.04 if k < steps else 0.0)
        colour = colour_at(t)
        for side, shade in ((-1, 0.85), (0, 1.15), (1, 0.85)):
            verts.append((px + side * nx * half, depth, pz + side * nz * half))
            colours.append(colour * shade)
    faces = [(3 * k + j, 3 * k + j + 1, 3 * k + j + 4, 3 * k + j + 3) for k in range(steps) for j in range(2)]
    return verts, faces, colours


def _blade(rng, cx, base_z, tall):
    """One blade of the cluster centred at cx: its Bezier, width and colour gradient."""
    height = rng.uniform(0.55, 0.97) if tall else rng.uniform(0.16, 0.5)
    x0 = cx + float(np.clip(rng.normal(0.0, 0.11), -0.3, 0.3))
    lean = (x0 - cx) * rng.uniform(1.0, 2.6) + rng.normal(0.0, 0.16 if tall else 0.08)
    droop = rng.uniform(0.1, 0.35) if tall and abs(lean) > 0.25 and rng.uniform() < 0.5 else 0.0
    top = base_z + 0.985 * CELL_H
    points = [np.array((x0, base_z)),
              np.array((x0 + 0.1 * lean, base_z + 0.5 * height)),
              np.array((x0 + 0.65 * lean, base_z + 0.95 * height)),
              np.array((x0 + lean, base_z + height * (1 - droop)))]
    for p in points:
        p[0] = np.clip(p[0], cx - HALF_SPAN, cx + HALF_SPAN)
        p[1] = min(p[1], top)
    green = GREENS[int(rng.integers(0, len(GREENS)))] * rng.uniform(0.8, 1.2)
    dry_blade = rng.uniform() < 0.08
    dry_tip = rng.uniform() < 0.35
    tip = STRAW * rng.uniform(0.9, 1.15) if dry_tip else np.array((0.24, 0.33, 0.07)) * rng.uniform(0.85, 1.15)
    if dry_blade:
        green, tip = np.array((0.32, 0.26, 0.10)) * rng.uniform(0.85, 1.1), np.array((0.55, 0.46, 0.20))
    base = BASE * (2.5 if dry_blade else 1.0)

    def colour_at(t):
        if t < 0.3:
            return base + (green - base) * t / 0.3
        start = 0.55 if dry_tip else 0.7
        return green if t < start else green + (tip - green) * (t - start) / (1 - start)
    return points, rng.uniform(0.018, 0.034), colour_at


def _seed_stem(rng, cx, base_z):
    """A thin stem with a head of 8-12 spikelets (small diamonds) along its top fifth: (verts, faces, colours)."""
    height = rng.uniform(0.85, 0.97)
    x0 = cx + rng.normal(0.0, 0.08)
    lean = rng.uniform(-0.18, 0.18)
    points = [np.array((x0, base_z)), np.array((x0 + 0.1 * lean, base_z + 0.4 * height)),
              np.array((x0 + 0.6 * lean, base_z + 0.8 * height)), np.array((x0 + lean, base_z + height))]
    stem_colour = np.array((0.20, 0.22, 0.07))
    head_colour = np.array((0.48, 0.40, 0.18))
    depth = rng.uniform(-0.3, 0.3)
    verts, faces, colours = _ribbon(points, 0.007, lambda t: stem_colour + (head_colour * 0.8 - stem_colour) * t, depth,
                                    taper=0.2)
    count = int(rng.integers(8, 13))
    for k in range(count):
        t = 0.8 + 0.2 * k / count
        (px, pz), (dx, dz) = _bezier(points, t)
        side = 1 if k % 2 else -1
        angle = math.atan2(dz, dx) + side * math.radians(rng.uniform(22, 35))
        ax, az = math.cos(angle), math.sin(angle)
        length, half = rng.uniform(0.035, 0.05), rng.uniform(0.006, 0.009)
        start = len(verts)
        verts += [(px, depth - 0.001, pz), (px + 0.5 * length * ax - half * az, depth - 0.001, pz + 0.5 * length * az + half * ax),
                  (px + length * ax, depth - 0.001, pz + length * az),
                  (px + 0.5 * length * ax + half * az, depth - 0.001, pz + 0.5 * length * az - half * ax)]
        faces.append((start, start + 1, start + 2, start + 3))
        colours += [head_colour * rng.uniform(0.85, 1.1)] * 4
    return verts, faces, colours


def _cluster_mesh(rng):
    """The 12 clusters in one mesh: cluster k in cell (k % 4, k // 4), row 0 on top. Returns (mesh, blades, stems)."""
    verts, faces, colours = [], [], []
    seeded = set(rng.choice(CELLS_X * CELLS_Y, 7, replace=False).tolist())
    blades = stems = 0

    def add(part):
        v, f, c = part
        faces.extend(tuple(len(verts) + i for i in face) for face in f)
        verts.extend(v)
        colours.extend(c)
    for k in range(CELLS_X * CELLS_Y):
        cx = CELL_W * (k % CELLS_X + 0.5)
        base_z = CELL_H * (CELLS_Y - 1 - k // CELLS_X) + 0.005
        kinds = [True] * int(rng.integers(14, 23)) + [False] * int(rng.integers(14, 25))
        for tall in rng.permutation(kinds):
            points, width, colour_at = _blade(rng, cx, base_z, bool(tall))
            add(_ribbon(points, width, colour_at, rng.uniform(-0.3, 0.3)))
            blades += 1
        if k in seeded:
            for _ in range(int(rng.integers(1, 4))):
                add(_seed_stem(rng, cx, base_z))
                stems += 1
    mesh = bpy.data.meshes.new("grass_atlas_blades")
    mesh.from_pydata(verts, [], faces)
    attribute = mesh.color_attributes.new("Col", 'FLOAT_COLOR', 'POINT')
    attribute.data.foreach_set('color', np.concatenate([np.array(colours), np.ones((len(colours), 1))], axis=1).ravel())
    return mesh, blades, stems


def fill_transparent(rgba, iterations=24):
    """Colour of texels with alpha < 0.5 from their opaque neighbours (repeated 3x3 averaging), then the mean."""
    rgb, known = rgba[..., :3].copy(), rgba[..., 3] >= 0.5
    for _ in range(iterations):
        acc, count = np.zeros_like(rgb), np.zeros(known.shape, np.float32)
        for dy in (-1, 0, 1):
            for dx in (-1, 0, 1):
                acc += np.roll(np.roll(rgb * known[..., None], dy, 0), dx, 1)
                count += np.roll(np.roll(known, dy, 0), dx, 1)
        fill = ~known & (count > 0)
        rgb[fill] = acc[fill] / count[fill][:, None]
        known = known | fill
    rgb[~known] = rgba[..., :3][rgba[..., 3] >= 0.5].mean(axis=0)
    rgba[..., :3] = np.where(rgba[..., 3:] >= 0.5, rgba[..., :3], rgb)
    return rgba


def _render_scene(path, mesh):
    scene = bpy.data.scenes.new("grass_atlas")
    obj = bpy.data.objects.new("grass_atlas_blades", mesh)
    scene.collection.objects.link(obj)
    material = bpy.data.materials.new("grass_atlas_emission")
    material.use_nodes = True
    nodes, links = material.node_tree.nodes, material.node_tree.links
    nodes.remove(nodes["Principled BSDF"])
    attribute, emission = nodes.new("ShaderNodeVertexColor"), nodes.new("ShaderNodeEmission")
    attribute.layer_name = "Col"
    links.new(attribute.outputs["Color"], emission.inputs["Color"])
    links.new(emission.outputs["Emission"], nodes["Material Output"].inputs["Surface"])
    obj.data.materials.append(material)
    camera = bpy.data.objects.new("grass_atlas_camera", bpy.data.cameras.new("grass_atlas_camera"))
    camera.data.type, camera.data.ortho_scale = 'ORTHO', CELLS_X * CELL_W
    camera.location = (CELLS_X * CELL_W / 2, -5.0, CELLS_Y * CELL_H / 2)
    camera.rotation_euler = (math.pi / 2, 0.0, 0.0)
    scene.collection.objects.link(camera)
    scene.camera = camera
    scene.render.engine = 'CYCLES'
    scene.cycles.device, scene.cycles.samples, scene.cycles.use_denoising = 'CPU', 32, False
    scene.render.film_transparent = True
    scene.render.resolution_x, scene.render.resolution_y = WIDTH, HEIGHT
    scene.render.resolution_percentage = 100
    scene.view_settings.view_transform, scene.view_settings.look = 'Standard', 'None'
    scene.render.image_settings.file_format, scene.render.image_settings.color_mode = 'PNG', 'RGBA'
    scene.render.filepath = str(path)
    bpy.ops.render.render(write_still=True, scene=scene.name)
    lens = camera.data
    bpy.data.objects.remove(obj, do_unlink=True)
    bpy.data.objects.remove(camera, do_unlink=True)
    bpy.data.cameras.remove(lens)
    bpy.data.materials.remove(material)
    bpy.data.scenes.remove(scene)


def render_atlas(path, seed=7):
    """Renders the atlas to `path` (PNG RGBA) in a scene of its own, removed afterwards. Returns coverage per cell."""
    mesh, blades, stems = _cluster_mesh(np.random.default_rng(seed))
    _render_scene(path, mesh)
    bpy.data.meshes.remove(mesh)
    image = bpy.data.images.load(str(path), check_existing=False)
    rgba = np.empty(WIDTH * HEIGHT * 4, np.float32)
    image.pixels.foreach_get(rgba)
    rgba = rgba.reshape(HEIGHT, WIDTH, 4)
    image.pixels.foreach_set(fill_transparent(rgba).ravel())
    image.filepath_raw, image.file_format = str(path), 'PNG'
    image.save()
    bpy.data.images.remove(image)
    alpha = rgba[::-1, :, 3] >= 0.5  # top-down rows
    rows = [int(round(HEIGHT * r / CELLS_Y)) for r in range(CELLS_Y + 1)]
    cells = np.array([[alpha[rows[r]:rows[r + 1], c * WIDTH // CELLS_X:(c + 1) * WIDTH // CELLS_X].mean()
                       for c in range(CELLS_X)] for r in range(CELLS_Y)])
    print(f"[garden] grass atlas {path}: {blades} blades, {stems} seed stems; opaque fraction per cell "
          f"{np.round(cells.ravel(), 3).tolist()}", flush=True)
    return cells
