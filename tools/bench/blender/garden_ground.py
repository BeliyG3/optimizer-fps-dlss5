"""The garden's ground textures (build_lab_garden.py): the soil bed and the path, from Poly Haven's grass_ground (a
2.5 m tile) and grass_path_2 (a 1 m tile), CC0, read only (their diff, nor_gl and disp maps, 2k JPEG).

bench12 reads one base colour and one normal texture per material, so the layering is baked into new images here
(numpy, written as JPEG next to the scene and packed into it):
  - soil, `garden_ground_{diff,nor}.jpg`, 4096² for a 5 m world-space tile (UV = world xy / 5 m, 819 px/m): three
    copies of grass_ground (as is, turned 90 degrees and offset, turned 270 degrees and offset), each where a periodic
    noise field (plus its own height map, a height blend) is highest, with narrow transitions. The 2.5 m repetition of
    one copy is broken; the whole repeats every 5 m.
  - path, `garden_path_{diff,nor}.jpg`, 2048 x 4096 for a strip 3 m across (u) by 5 m along (v), the path mesh's UVs
    (garden_path.py: across its 2.5 m plus 0.25 m on each side; along the route's arc length): two copies of
    grass_path_2 (the second turned 90 degrees) under the same kind of mask, blending into two copies of grass_ground
    at a ragged edge around 1.1-1.35 m from the centre line (noise plus a height blend), ground only from 1.42 m out.
Turning a normal map turns its vectors too (GL convention: +x along u, +y along v); a check compares each turned
copy's normals with its height map's slopes. Roughness is a constant (the maps' roughness is near 1 everywhere).
"""
import math

import bpy
import numpy as np

GROUND_TILE, PATH_TILE = 2.5, 1.0        # Poly Haven's real-world sizes, metres
SOIL_PERIOD, SOIL_PIXELS = 5.0, 4096
PATH_ACROSS, PATH_ALONG = 3.0, 5.0       # the path texture's extent, metres
PATH_PIXELS = (2048, 4096)               # (across, along)
EDGE = (1.12, 1.42)                      # the path's ragged edge: around the first, ground only past the second


# ---- sources ------------------------------------------------------------------------------------------------------
def _pixels(path, non_color):
    image = bpy.data.images.load(str(path), check_existing=False)
    if non_color:
        image.colorspace_settings.name = 'Non-Color'
    w, h = image.size
    buffer = np.empty(w * h * 4, np.float32)
    image.pixels.foreach_get(buffer)
    bpy.data.images.remove(image)
    return buffer.reshape(h, w, 4)[..., :3]  # rows bottom up (v up), as Blender stores them


def load_source(folder, name, halve=False):
    """(h, w, 7): colour (sRGB-encoded, as stored), normal (decoded, -1..1), height (0..1); halved by a 2x2 box filter."""
    colour = _pixels(folder / f"{name}_diff_2k.jpg", False)
    normal = _pixels(folder / f"{name}_nor_gl_2k.jpg", True) * 2 - 1
    height = _pixels(folder / f"{name}_disp_2k.jpg", True)[..., :1]
    stack = np.concatenate([colour, normal, height], axis=2)
    if halve:
        h, w, c = stack.shape
        stack = stack.reshape(h // 2, 2, w // 2, 2, c).mean(axis=(1, 3))
    return np.ascontiguousarray(stack, np.float32)


# ---- sampling -----------------------------------------------------------------------------------------------------
def _bilinear(src, x, y):
    """src (n, n, c) sampled at pixel coordinates (x, y) (texel centres at i + 0.5), wrapping."""
    n = src.shape[0]
    flat = src.reshape(n * n, -1)
    fx, fy = x - 0.5, y - 0.5
    x0, y0 = np.floor(fx), np.floor(fy)
    tx, ty = (fx - x0)[..., None], (fy - y0)[..., None]
    x0, y0 = x0.astype(np.int64) % n, y0.astype(np.int64) % n
    x1, y1 = (x0 + 1) % n, (y0 + 1) % n
    return ((flat[y0 * n + x0] * (1 - tx) + flat[y0 * n + x1] * tx) * (1 - ty)
            + (flat[y1 * n + x0] * (1 - tx) + flat[y1 * n + x1] * tx) * ty)


def _turn(x, y, quarter):
    """(x, y) turned by quarter * 90 degrees counter-clockwise."""
    return [(x, y), (-y, x), (-x, -y), (y, -x)][quarter % 4]


def layer(src, tile, quarter, offset, xm, ym):
    """A copy of src (one `tile` metres square) turned by quarter * 90 degrees and moved by offset (metres), sampled at
    (xm, ym) metres: (..., 7) with the normals turned with it."""
    sx, sy = _turn(xm, ym, -quarter)
    scale = src.shape[0] / tile
    out = _bilinear(src, (sx + offset[0]) * scale, (sy + offset[1]) * scale)
    out[..., 3], out[..., 4] = _turn(out[..., 3].copy(), out[..., 4].copy(), quarter)
    return out


def noise(xm, ym, rng, waves, cycles, period=(None, None)):
    """Smooth noise in about [-1, 1]: plane waves of `cycles` (low, high) per metre; along an axis with a period the
    frequencies are whole numbers of cycles per period."""
    total = np.zeros(np.broadcast(xm, ym).shape, np.float32)
    for _ in range(waves):
        a, f = rng.uniform(0, 2 * math.pi), rng.uniform(*cycles)
        fx, fy = f * math.cos(a), f * math.sin(a)
        if period[0]:
            fx = round(fx * period[0]) / period[0]
        if period[1]:
            fy = round(fy * period[1]) / period[1]
        total += np.sin(2 * math.pi * (fx * xm + fy * ym) + rng.uniform(0, 2 * math.pi)).astype(np.float32)
    return total * (1.6 / math.sqrt(waves))


def blend(layers, scores, sharpness=0.05):
    """Layers (k, ..., 7) weighted by softmax(score / sharpness); normals renormalised."""
    s = np.stack(scores) / sharpness
    w = np.exp(s - s.max(axis=0))
    w /= w.sum(axis=0)
    out = (np.stack(layers) * w[..., None]).sum(axis=0)
    out[..., 3:6] /= np.linalg.norm(out[..., 3:6], axis=-1, keepdims=True)
    return out


def check_turns(src, tile, name):
    """Each turned copy's normals against its height map's slopes: n.x ~ -dh/dx and n.y ~ -dh/dy (correlations > 0)."""
    step = tile / src.shape[0]
    ym, xm = np.mgrid[0:256, 0:256] * step + 0.37
    for quarter in range(4):
        sample = layer(src, tile, quarter, (0.11, 0.23), xm, ym)
        h = sample[..., 6]
        gx, gy = -np.gradient(h, axis=1), -np.gradient(h, axis=0)
        cx = np.corrcoef(gx.ravel(), sample[..., 3].ravel())[0, 1]
        cy = np.corrcoef(gy.ravel(), sample[..., 4].ravel())[0, 1]
        if cx <= 0.05 or cy <= 0.05:
            raise RuntimeError(f"{name} turned {90 * quarter}: normals do not follow the height map ({cx:.2f}, {cy:.2f})")
    print(f"[garden] {name}: turned normals follow the height map", flush=True)


# ---- the two textures ---------------------------------------------------------------------------------------------
def _rows(height, width, extent, chunk=256):
    """Row chunks of an image covering extent (metres across, metres along): (rows, xm, ym) at texel centres."""
    for r0 in range(0, height, chunk):
        rows = slice(r0, min(r0 + chunk, height))
        ym, xm = np.mgrid[rows, 0:width].astype(np.float32)
        yield rows, (xm + 0.5) / width * extent[0], (ym + 0.5) / height * extent[1]


def soil_maps(ground, seed=21):
    """The soil texture (colour, normal), (n, n, 3) each, bottom-up rows."""
    n, period = SOIL_PIXELS, (SOIL_PERIOD, SOIL_PERIOD)
    colour, normal = np.zeros((n, n, 3), np.float32), np.zeros((n, n, 3), np.float32)
    copies = ((0, (0.0, 0.0)), (1, (0.37, 1.13)), (3, (1.71, 0.52)))
    for rows, xm, ym in _rows(n, n, period):
        layers, scores = [], []
        for k, (quarter, offset) in enumerate(copies):
            sample = layer(ground, GROUND_TILE, quarter, offset, xm, ym)
            mask = noise(xm, ym, np.random.default_rng(seed + k), 6, (0.15, 0.6), period)
            layers.append(sample)
            scores.append(mask + 0.6 * (sample[..., 6] - 0.5))
        out = blend(layers, scores)
        colour[rows], normal[rows] = out[..., :3], out[..., 3:6]
    return colour, normal


def path_maps(ground, gravel, seed=31):
    """The path texture (colour, normal), (along, across, 3) each, bottom-up rows; u across, v along."""
    width, height = PATH_PIXELS
    period = (None, PATH_ALONG)
    colour, normal = np.zeros((height, width, 3), np.float32), np.zeros((height, width, 3), np.float32)
    rng = lambda k: np.random.default_rng(seed + k)  # noqa: E731 (the same field in every chunk)
    for rows, xm, ym in _rows(height, width, (PATH_ACROSS, PATH_ALONG)):
        across = xm - PATH_ACROSS / 2
        g = [layer(gravel, PATH_TILE, 0, (0.0, 0.0), xm, ym), layer(gravel, PATH_TILE, 1, (0.31, 0.57), xm, ym)]
        e = [layer(ground, GROUND_TILE, 0, (0.4, 0.0), xm, ym), layer(ground, GROUND_TILE, 2, (1.1, 0.7), xm, ym)]
        path = blend(g, [noise(xm, ym, rng(k), 5, (0.3, 1.2), period) + 0.6 * (g[k][..., 6] - 0.5) for k in range(2)])
        soil = blend(e, [noise(xm, ym, rng(2 + k), 5, (0.15, 0.6), period) + 0.6 * (e[k][..., 6] - 0.5) for k in range(2)])
        side = np.where(across > 0, 2.3, 0.0)
        edge = (EDGE[0] + 0.10 * noise(0.0 * xm, ym + side, rng(4), 4, (0.2, 0.7), period)
                + 0.04 * noise(xm, ym, rng(5), 5, (1.5, 4.0), period))
        t = (np.abs(across) - edge) / 0.12 + 2.0 * (soil[..., 6] - path[..., 6])
        w = np.clip(0.5 + 0.5 * t, 0, 1)
        w = np.maximum(w * w * (3 - 2 * w), np.clip((np.abs(across) - EDGE[1] + 0.06) / 0.06, 0, 1))[..., None]
        out = path * (1 - w) + soil * w
        out[..., 3:6] /= np.linalg.norm(out[..., 3:6], axis=-1, keepdims=True)
        colour[rows], normal[rows] = out[..., :3], out[..., 3:6]
    return colour, normal


# ---- images and materials -----------------------------------------------------------------------------------------
def save_jpeg(rgb, path, non_color):
    """rgb (h, w, 3) in 0..1, bottom-up rows, written as a JPEG (quality 92), then loaded and packed."""
    h, w, _ = rgb.shape
    image = bpy.data.images.new(path.stem + "_tmp", w, h, alpha=False)
    if non_color:
        image.colorspace_settings.name = 'Non-Color'
    rgba = np.concatenate([np.clip(rgb, 0, 1), np.ones((h, w, 1), np.float32)], axis=2)
    image.pixels.foreach_set(rgba.ravel())
    image.filepath_raw, image.file_format = str(path), 'JPEG'
    try:
        image.save(filepath=str(path), quality=92)
    except TypeError:
        image.save()
    bpy.data.images.remove(image)
    loaded = bpy.data.images.load(str(path), check_existing=False)
    if non_color:
        loaded.colorspace_settings.name = 'Non-Color'
    loaded.pack()
    return loaded


def material(name, colour_image, normal_image, roughness):
    """Principled BSDF: Base Color <- colour image, Normal <- Normal Map (tangent space, the active UV map) <- normal
    image; constant roughness. The glTF exporter writes baseColorTexture and normalTexture."""
    mat = bpy.data.materials.new(name)
    mat.use_nodes = True
    nodes, links = mat.node_tree.nodes, mat.node_tree.links
    bsdf = nodes["Principled BSDF"]
    bsdf.inputs["Roughness"].default_value = roughness
    colour, normal, normal_map = nodes.new("ShaderNodeTexImage"), nodes.new("ShaderNodeTexImage"), nodes.new("ShaderNodeNormalMap")
    colour.image, normal.image = colour_image, normal_image
    links.new(colour.outputs["Color"], bsdf.inputs["Base Color"])
    links.new(normal.outputs["Color"], normal_map.inputs["Color"])
    links.new(normal_map.outputs["Normal"], bsdf.inputs["Normal"])
    return mat


def materials(polyhaven, out):
    """garden_soil and garden_path materials, their images written to `out` and packed. Returns (soil, path)."""
    ground = load_source(polyhaven / "grass_ground", "grass_ground")
    gravel = load_source(polyhaven / "grass_path_2", "grass_path_2", halve=True)
    check_turns(ground, GROUND_TILE, "grass_ground")
    check_turns(gravel, PATH_TILE, "grass_path_2")
    result = []
    for name, stem, (colour, normal), roughness in (("soil", "garden_ground", soil_maps(ground), 0.95),
                                                     ("path", "garden_path", path_maps(ground, gravel), 0.9)):
        images = (save_jpeg(colour, out / f"{stem}_diff.jpg", False),
                  save_jpeg(normal * 0.5 + 0.5, out / f"{stem}_nor.jpg", True))
        result.append(material(f"garden_{name}", *images, roughness))
        print(f"[garden] garden_{name}: {colour.shape[1]}x{colour.shape[0]}, mean colour "
              f"{np.round(colour.reshape(-1, 3).mean(axis=0), 3).tolist()}", flush=True)
    return result
