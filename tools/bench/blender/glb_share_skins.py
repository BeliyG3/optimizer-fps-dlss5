"""Makes the garden's people (garden v2.4, nodes people_<character>_..., garden_people.py) share their character's one
glTF mesh in an exported .glb, in place (scene_export.export_glb calls it; a file without such nodes is left untouched).

Blender's exporter writes a skinned mesh per object, its vertices in world space at the export frame, and the skin's
inverse bind matrices (IBM) from the armature's world matrix. So copies of one character get byte-different meshes: for
a copy c of the lab character o (CANONICAL: the lab's own node), P_c = M P_o and IBM_c[j] = IBM_o[j] M^-1 with one rigid
M = IBM_c[j]^-1 IBM_o[j] for every joint j. Then J_c[j] IBM_o[j] P_o = J_c[j] IBM_c[j] P_c: the copy can use o's mesh
and o's IBM accessor with its own skin (its own joints) and lose nothing. (The copies export the lab character's own
mesh data, garden_people.use_lab_meshes; the exporter's vertex merging still differs with the world positions, so the
vertex lists differ while the triangles do not.) Checked per copy before it is changed: the same primitives, materials,
attributes and joint names (in order), M the same for every joint (MATRIX_TOLERANCE), and at every triangle corner (in
the index buffers' order) the same JOINTS_0, WEIGHTS_0 and TEXCOORD_0, the position within POSITION_TOLERANCE m and the
normal within NORMAL_TOLERANCE of M applied to o's; if any copy fails, nothing is changed and it raises. Unused meshes,
accessors and buffer views are then dropped and the binary chunk is rewritten.
"""
import json
import struct
from pathlib import Path

import numpy as np

PREFIX = 'people_'
CANONICAL = {'olivia': 'olivia_mesh_{}', 'chad': 'chad_mesh_{}', 'xbot': 'hiphop_mesh_{}'}
MATRIX_TOLERANCE, POSITION_TOLERANCE, NORMAL_TOLERANCE = 1e-3, 2e-4, 2e-3
SIZES = {5126: 4, 5125: 4, 5123: 2, 5122: 2, 5121: 1, 5120: 1}
COMPONENTS = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3, 'VEC4': 4, 'MAT4': 16}
TYPES = {5126: np.float32, 5125: np.uint32, 5123: np.uint16, 5122: np.int16, 5121: np.uint8, 5120: np.int8}


def read(path):
    raw = Path(path).read_bytes()
    size, _ = struct.unpack_from('<II', raw, 12)
    doc = json.loads(raw[20:20 + size])
    bsize, _ = struct.unpack_from('<II', raw, 20 + size)
    return doc, raw[28 + size:28 + size + bsize]


def write(path, doc, binary):
    text = json.dumps(doc, separators=(',', ':')).encode()
    text += b' ' * (-len(text) % 4)
    binary += b'\0' * (-len(binary) % 4)
    total = 12 + 8 + len(text) + 8 + len(binary)
    Path(path).write_bytes(struct.pack('<4sII', b'glTF', 2, total) + struct.pack('<II', len(text), 0x4E4F534A) + text
                           + struct.pack('<II', len(binary), 0x004E4942) + binary)


def accessor_bytes(doc, binary, index):
    a = doc['accessors'][index]
    view = doc['bufferViews'][a['bufferView']]
    width = SIZES[a['componentType']] * COMPONENTS[a['type']]
    stride = view.get('byteStride', width)
    start = view.get('byteOffset', 0) + a.get('byteOffset', 0)
    rows = np.lib.stride_tricks.as_strided(np.frombuffer(binary, np.uint8, count=stride * (a['count'] - 1) + width,
                                                         offset=start), (a['count'], width), (stride, 1))
    return rows.tobytes()


def values(doc, binary, index):
    a = doc['accessors'][index]
    return np.frombuffer(accessor_bytes(doc, binary, index), TYPES[a['componentType']]).reshape(
        a['count'], COMPONENTS[a['type']]).astype(np.float64)


def _ibm(doc, binary, skin):
    return values(doc, binary, doc['skins'][skin]['inverseBindMatrices']).reshape(-1, 4, 4).transpose(0, 2, 1)


def _joint_names(doc, skin):
    return [doc['nodes'][j].get('name') for j in doc['skins'][skin]['joints']]


def equivalent(doc, binary, copy, canon):
    """None if node `copy` can use node `canon`'s mesh and IBM (see the module's description), else the reason."""
    nodes = doc['nodes']
    c, o = nodes[copy], nodes[canon]
    if _joint_names(doc, c['skin']) != _joint_names(doc, o['skin']):
        return "joint names differ"
    m = np.linalg.inv(_ibm(doc, binary, c['skin'])) @ _ibm(doc, binary, o['skin'])
    spread = float(np.abs(m - m[0]).max())
    if spread > MATRIX_TOLERANCE:
        return f"no common transform (spread {spread:.2e})"
    m = m[0]
    pc, po = doc['meshes'][c['mesh']]['primitives'], doc['meshes'][o['mesh']]['primitives']
    if len(pc) != len(po):
        return "primitive counts differ"
    for a, b in zip(pc, po):
        if a.get('material') != b.get('material') or a.get('mode', 4) != b.get('mode', 4) \
                or set(a['attributes']) != set(b['attributes']) or 'targets' in a or 'targets' in b:
            return "primitive layout differs"
        ia, ib = (values(doc, binary, p['indices'])[:, 0].astype(np.int64) for p in (a, b))
        if len(ia) != len(ib):
            return "triangle counts differ"
        for key in a['attributes']:
            ca, cb = values(doc, binary, a['attributes'][key])[ia], values(doc, binary, b['attributes'][key])[ib]
            if key == 'POSITION':
                cb, limit = cb @ m[:3, :3].T + m[:3, 3], POSITION_TOLERANCE
            elif key == 'NORMAL':
                cb = cb @ m[:3, :3].T
                cb, limit = cb / np.linalg.norm(cb, axis=1, keepdims=True), NORMAL_TOLERANCE
            elif key == 'TANGENT':
                return "tangents are not checked"
            else:
                limit = 1e-5
            error = float(np.abs(ca - cb).max())
            if error > limit:
                return f"{key} differs by {error:.2e} at a triangle corner"
    return None


def _compact(doc, binary):
    """Drops meshes, accessors and buffer views nothing uses; returns the new binary chunk."""
    used_meshes = sorted({n['mesh'] for n in doc['nodes'] if 'mesh' in n})
    mesh_map = {old: new for new, old in enumerate(used_meshes)}
    doc['meshes'] = [doc['meshes'][i] for i in used_meshes]
    for node in doc['nodes']:
        if 'mesh' in node:
            node['mesh'] = mesh_map[node['mesh']]
    refs = []  # (container, key) holding an accessor index
    for mesh in doc['meshes']:
        for p in mesh['primitives']:
            refs += [(p['attributes'], k) for k in p['attributes']] + ([(p, 'indices')] if 'indices' in p else [])
    refs += [(s, 'inverseBindMatrices') for s in doc.get('skins', []) if 'inverseBindMatrices' in s]
    for animation in doc.get('animations', []):
        refs += [(s, k) for s in animation['samplers'] for k in ('input', 'output')]
    used = sorted({c[k] for c, k in refs})
    accessor_map = {old: new for new, old in enumerate(used)}
    doc['accessors'] = [doc['accessors'][i] for i in used]
    for container, key in refs:
        container[key] = accessor_map[container[key]]
    view_users = [a for a in doc['accessors'] if 'bufferView' in a] + [i for i in doc.get('images', []) if 'bufferView' in i]
    views = sorted({u['bufferView'] for u in view_users})
    view_map, chunks, offset = {}, [], 0
    for new, old in enumerate(views):
        view = doc['bufferViews'][old]
        start = view.get('byteOffset', 0)
        data = binary[start:start + view['byteLength']]
        pad = -offset % 4
        chunks.append(b'\0' * pad + data)
        offset += pad
        view['byteOffset'] = offset
        offset += len(data)
        view_map[old] = new
    doc['bufferViews'] = [doc['bufferViews'][i] for i in views]
    for user in view_users:
        user['bufferView'] = view_map[user['bufferView']]
    new_binary = b''.join(chunks)
    doc['buffers'][0]['byteLength'] = len(new_binary)
    return new_binary


def share_copies(path):
    """Shares the people's meshes in the .glb at path (in place). Returns a summary dict, or None without people."""
    doc, binary = read(path)
    names = {n.get('name'): i for i, n in enumerate(doc['nodes'])}
    copies = [i for i, n in enumerate(doc['nodes']) if n.get('name', '').startswith(PREFIX) and 'mesh' in n and 'skin' in n]
    if not copies:
        return None
    before = (len(doc['meshes']), len(binary))
    pairs, kept = [], []
    for i in copies:
        name = doc['nodes'][i]['name']
        character, k = name.split('_')[1], name.rsplit('_', 1)[1]
        canon = names.get(CANONICAL[character].format(k))
        reason = "no canonical node" if canon is None else equivalent(doc, binary, i, canon)
        if reason:
            kept.append(f"{name}: {reason}")
        pairs.append((i, canon))
    if kept:
        raise RuntimeError(f"people meshes that cannot share their character's mesh: {kept}")
    for i, canon in pairs:
        node, source = doc['nodes'][i], doc['nodes'][canon]
        node['mesh'] = source['mesh']
        doc['skins'][node['skin']]['inverseBindMatrices'] = doc['skins'][source['skin']]['inverseBindMatrices']
    binary = _compact(doc, binary)
    write(path, doc, binary)
    summary = dict(copy_mesh_nodes=len(copies), meshes=[before[0], len(doc['meshes'])],
                   binary_bytes=[before[1], len(binary)])
    print(f"[share] {Path(path).name}: all {len(copies)} people mesh nodes share their character's mesh; meshes "
          f"{before[0]} -> {len(doc['meshes'])}, binary {before[1] / 1e6:.1f} -> {len(binary) / 1e6:.1f} MB", flush=True)
    return summary
