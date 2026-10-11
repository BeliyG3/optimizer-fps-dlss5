"""Checks of the baked lab export (bake_lab_lightmaps.py runs them; they also run without Blender):

    python tools\\bench\\blender\\lab_lightmap_check.py lab_scene_baked.glb [lab_scene.glb] [--raster 8192]

- structure: TEXCOORD_1 on every primitive of a static node and on none of a dynamic one (the bench's rule: a skin, a
  morph-weight channel on the node, or a TRS channel on the node or a parent) or a foliage or prop one (`veg_*`,
  `prop_*`), mesh extras.lightmapAtlas on the static meshes; with the reference export, the same nodes in the same order with the same primitives and index counts;
- UVs, rasterized at texel centres (at the atlas size, --raster): the atlas fraction covered, the fraction of covered
  texels that two nodes share, the smallest distance between texels of different nodes, and the static nodes (and
  their area) that cover no texel centre at all, so the bake leaves them black.
"""
import json
import struct
import sys

import numpy as np

COMPONENTS = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3, 'VEC4': 4}
TYPES = {5121: np.uint8, 5123: np.uint16, 5125: np.uint32, 5126: np.float32}
UNBAKED_PREFIXES = ("veg_", "prop_")  # the garden's foliage and props: no TEXCOORD_1 (bake_lab_lightmaps.py skips them)


def read_glb(path):
    with open(path, 'rb') as f:
        raw = f.read()
    magic, version, _ = struct.unpack_from('<4sII', raw)
    if magic != b'glTF' or version != 2:
        raise ValueError(f"{path}: not a glTF 2 binary")
    size, _ = struct.unpack_from('<II', raw, 12)
    doc = json.loads(raw[20:20 + size])
    binary_size, _ = struct.unpack_from('<II', raw, 20 + size)
    return doc, raw[28 + size:28 + size + binary_size]


def accessor(doc, binary, index):
    acc = doc['accessors'][index]
    view = doc['bufferViews'][acc['bufferView']]
    dtype, width = np.dtype(TYPES[acc['componentType']]), COMPONENTS[acc['type']]
    stride = view.get('byteStride', dtype.itemsize * width)
    offset = view.get('byteOffset', 0) + acc.get('byteOffset', 0)
    return np.ndarray((acc['count'], width), dtype, binary, offset, (stride, dtype.itemsize)).copy()


def dynamic_nodes(doc):
    nodes = doc['nodes']
    parent = {child: i for i, n in enumerate(nodes) for child in n.get('children', [])}
    channels = [c['target'] for a in doc.get('animations', []) for c in a['channels']]
    moved = {t['node'] for t in channels if t['path'] in ('translation', 'rotation', 'scale')}
    morphed = {t['node'] for t in channels if t['path'] == 'weights'}

    def dynamic(i):
        if 'skin' in nodes[i] or i in morphed:
            return True
        while i is not None:
            if i in moved:
                return True
            i = parent.get(i)
        return False
    return [dynamic(i) for i in range(len(nodes))]


def _local_matrix(node):
    if 'matrix' in node:
        return np.array(node['matrix'], np.float64).reshape(4, 4).T  # glTF stores column-major
    x, y, z, w = node.get('rotation', (0.0, 0.0, 0.0, 1.0))
    rotation = np.array([[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
                         [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
                         [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]])
    matrix = np.eye(4)
    matrix[:3, :3] = rotation * np.array(node.get('scale', (1.0, 1.0, 1.0)))
    matrix[:3, 3] = node.get('translation', (0.0, 0.0, 0.0))
    return matrix


def world_matrices(doc):
    nodes = doc['nodes']
    parent = {child: i for i, n in enumerate(nodes) for child in n.get('children', [])}
    worlds = {}

    def world(i):
        if i not in worlds:
            local = _local_matrix(nodes[i])
            worlds[i] = world(parent[i]) @ local if i in parent else local
        return worlds[i]
    return [world(i) for i in range(len(nodes))]


def _counts(doc):
    return dict(nodes=len(doc['nodes']), meshes=len(doc['meshes']), primitives=sum(len(m['primitives']) for m in doc['meshes']),
                skins=len(doc.get('skins', [])), animations=len(doc.get('animations', [])))


def _indices(doc, binary, primitive):
    if 'indices' in primitive:
        return accessor(doc, binary, primitive['indices'])[:, 0].astype(np.int64)
    return np.arange(doc['accessors'][primitive['attributes']['POSITION']]['count'])


def _corners(doc, binary, primitive):
    """Triangle corner positions in index order, and the primitive's vertex count."""
    position = accessor(doc, binary, primitive['attributes']['POSITION'])
    return position[_indices(doc, binary, primitive)], len(position)


def _material(doc, primitive):
    return doc['materials'][primitive['material']].get('name') if 'material' in primitive else None


def compare_geometry(doc, binary, reference, reference_binary):
    """Node by node against the reference export: the same triangles (corner positions in order), materials and
    attributes apart from TEXCOORD_1 (and an added empty TEXCOORD_0). Vertex counts grow at the lightmap's UV seams."""
    a, b = doc['nodes'], reference['nodes']
    report = dict(_counts(reference), same_node_names_in_order=len(a) == len(b) and all(x.get('name') == y.get('name') for x, y in zip(a, b)),
                  triangles=0, reference_triangles=0, vertices=0, reference_vertices=0, max_corner_distance=0.0,
                  primitives_with_other_triangles=0, material_changes=0, texcoord0_added=0, other_attribute_changes=0)
    for x, y in zip(a, b) if report['same_node_names_in_order'] else ():
        px = doc['meshes'][x['mesh']]['primitives'] if 'mesh' in x else []
        py = reference['meshes'][y['mesh']]['primitives'] if 'mesh' in y else []
        report['primitives_with_other_triangles'] += abs(len(px) - len(py))
        for p, q in zip(px, py):
            (cp, vp), (cq, vq) = _corners(doc, binary, p), _corners(reference, reference_binary, q)
            report['triangles'] += len(cp) // 3
            report['reference_triangles'] += len(cq) // 3
            report['vertices'] += vp
            report['reference_vertices'] += vq
            if cp.shape != cq.shape:
                report['primitives_with_other_triangles'] += 1
            else:
                report['max_corner_distance'] = max(report['max_corner_distance'], float(np.abs(cp - cq).max(initial=0.0)))
            report['material_changes'] += _material(doc, p) != _material(reference, q)
            added = set(p['attributes']) - set(q['attributes']) - {'TEXCOORD_1'}
            report['texcoord0_added'] += added == {'TEXCOORD_0'}
            report['other_attribute_changes'] += bool(set(q['attributes']) - set(p['attributes'])) or bool(added - {'TEXCOORD_0'})
    print("[check] against the reference:", json.dumps(report), flush=True)
    return report


def check_structure(doc):
    dynamic = dynamic_nodes(doc)
    report = dict(_counts(doc), static_nodes=0, dynamic_nodes=0, static_primitives=0, dynamic_primitives=0,
                  static_without_texcoord1=0, dynamic_with_texcoord1=0, static_meshes_without_atlas=0, unbaked_nodes=0,
                  unbaked_with_texcoord1=0)
    uses = {}
    for i, node in enumerate(doc['nodes']):
        if 'mesh' not in node:
            continue
        mesh = doc['meshes'][node['mesh']]
        if node.get('name', '').startswith(UNBAKED_PREFIXES):  # never baked (lab_lightmap_uv.is_unbaked)
            report['unbaked_nodes'] += 1
            report['unbaked_with_texcoord1'] += any('TEXCOORD_1' in p['attributes'] for p in mesh['primitives'])
            continue
        kind = 'dynamic' if dynamic[i] else 'static'
        report[kind + '_nodes'] += 1
        uses.setdefault(node['mesh'], []).append(kind)
        for primitive in mesh['primitives']:
            has = 'TEXCOORD_1' in primitive['attributes']
            report[kind + '_primitives'] += 1
            if dynamic[i]:
                report['dynamic_with_texcoord1'] += has
            else:
                report['static_without_texcoord1'] += not has
        if not dynamic[i] and not isinstance(mesh.get('extras', {}).get('lightmapAtlas'), int):
            report['static_meshes_without_atlas'] += 1
    report['static_meshes_on_two_nodes'] = sum(1 for kinds in uses.values() if kinds.count('static') > 1)
    print("[check] structure:", json.dumps(report), flush=True)
    return report


def static_uv_triangles(doc, binary):
    """(triangles x 3 x 2 TEXCOORD_1, node index per triangle, world-space area per triangle) of the static nodes."""
    dynamic, worlds = dynamic_nodes(doc), world_matrices(doc)
    tris, owners, areas = [], [], []
    for i, node in enumerate(doc['nodes']):
        if 'mesh' not in node or dynamic[i]:
            continue
        for p in doc['meshes'][node['mesh']]['primitives']:
            if 'TEXCOORD_1' not in p['attributes']:
                continue
            uv = accessor(doc, binary, p['attributes']['TEXCOORD_1']).astype(np.float64)
            corners = _indices(doc, binary, p).reshape(-1, 3)
            position = accessor(doc, binary, p['attributes']['POSITION']).astype(np.float64) @ worlds[i][:3, :3].T
            tris.append(uv[corners])
            owners.append(np.full(len(corners), i, np.int64))
            co = position[corners]
            areas.append(0.5 * np.linalg.norm(np.cross(co[:, 1] - co[:, 0], co[:, 2] - co[:, 0]), axis=1))
    return np.concatenate(tris), np.concatenate(owners), np.concatenate(areas)


def _inside(p, x, y, sign):
    """Texel centres (x, y) inside triangles p (m x 3 x 2), edges included; sign = orientation per triangle."""
    inside = np.ones(x.shape, bool)
    for k in range(3):
        a, b = p[:, k], p[:, (k + 1) % 3]
        edge = (b[:, 0, None] - a[:, 0, None]) * (y - a[:, 1, None]) - (b[:, 1, None] - a[:, 1, None]) * (x - a[:, 0, None])
        inside &= edge * sign[:, None] >= 0
    return inside


def rasterize(tris, owners, res, budget=1 << 23):
    """(texel index, owner) pairs of every texel centre inside a triangle, without duplicates."""
    p = tris * res - 0.5  # texel centres at integer coordinates
    lo = np.maximum(np.ceil(p.min(axis=1)), 0).astype(np.int64)
    hi = np.minimum(np.floor(p.max(axis=1)), res - 1).astype(np.int64)
    size = hi - lo + 1
    e1, e2 = p[:, 1] - p[:, 0], p[:, 2] - p[:, 0]
    sign = np.sign(e1[:, 0] * e2[:, 1] - e1[:, 1] * e2[:, 0])
    candidates = np.nonzero((size[:, 0] > 0) & (size[:, 1] > 0) & (sign != 0))[0]
    keys = size[candidates, 0] * (res + 1) + size[candidates, 1]
    texels, who = [], []
    for key in np.unique(keys):
        group = candidates[keys == key]
        w, h = int(key // (res + 1)), int(key % (res + 1))
        rows = max(1, budget // (w * h))
        for start in range(0, len(group), rows):
            t = group[start:start + rows]
            for y0 in range(0, h, max(1, budget // w)):  # a single huge triangle is cut into row bands
                gy, gx = np.mgrid[y0:min(h, y0 + max(1, budget // w)), 0:w]
                x, y = lo[t, 0, None] + gx.ravel(), lo[t, 1, None] + gy.ravel()
                inside = _inside(p[t], x, y, sign[t])
                texels.append((y * res + x)[inside])
                who.append(np.broadcast_to(owners[t, None], x.shape)[inside])
    pairs = np.unique(np.concatenate(texels) * (owners.max() + 1) + np.concatenate(who))
    return pairs // (owners.max() + 1), pairs % (owners.max() + 1)


def min_gap(texel, owner, res, limit):
    """Smallest Chebyshev distance (in texels) between texel centres of two different nodes, up to limit."""
    image = np.full(res * res, -1, np.int32)
    image[texel] = owner
    image = image.reshape(res, res)
    for d in range(1, limit + 1):
        ring = [(dx, dy) for dy in range(0, d + 1) for dx in range(-d, d + 1) if max(abs(dx), abs(dy)) == d and (dy > 0 or dx > 0)]
        for dx, dy in ring:
            a = image[dy:, max(0, -dx):res - max(0, dx)]
            b = image[:res - dy, max(0, dx):res - max(0, -dx)]
            if np.any((a >= 0) & (b >= 0) & (a != b)):
                return d
    return None


def check_uvs(doc, binary, res, gap_limit=0):
    tris, owners, areas = static_uv_triangles(doc, binary)
    texel, owner = rasterize(tris, owners, res)
    covered, shared = np.unique(texel, return_counts=True)
    blank = np.setdiff1d(np.unique(owners), owner)
    blank_area = areas[np.isin(owners, blank)]
    report = dict(resolution=res, uv_min=tris.min(axis=(0, 1)).tolist(), uv_max=tris.max(axis=(0, 1)).tolist(),
                  covered_fraction=len(covered) / res ** 2, shared_texel_fraction=float(np.count_nonzero(shared > 1)) / max(1, len(covered)),
                  static_nodes=len(np.unique(owners)), static_area_m2=float(areas.sum()), nodes_without_texel=len(blank),
                  area_without_texel_m2=float(blank_area.sum()), nodes_without_texel_examples=[doc['nodes'][i].get('name') for i in blank[:8]])
    if gap_limit:
        report['min_texel_distance_between_nodes'] = min_gap(texel, owner, res, gap_limit)
    print("[check] lightmap UVs:", json.dumps(report), flush=True)
    return report


def main(argv):
    raster = int(argv[argv.index('--raster') + 1]) if '--raster' in argv else 8192
    paths = [a for a in argv if a.endswith('.glb')]
    doc, binary = read_glb(paths[0])
    check_structure(doc)
    if len(paths) > 1:
        compare_geometry(doc, binary, *read_glb(paths[1]))
    check_uvs(doc, binary, raster, gap_limit=12)


if __name__ == '__main__':
    main(sys.argv[1:])
