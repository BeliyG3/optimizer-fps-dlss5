"""CuteGirl head measurements: morph tables per mesh, lip / cheek / chin geometry under a shape-key weight set."""

import bpy
import mathutils

from cutegirl_asset import NAME_TABLES


def morph_meshes():
    """{object: {cc4_name: ShapeKey}} for every imported mesh with named-able morphs."""
    out = {}
    for obj in bpy.data.objects:
        if obj.type != "MESH" or not obj.data.shape_keys:
            continue
        kbs = obj.data.shape_keys.key_blocks
        table = NAME_TABLES.get(len(kbs) - 1)
        if not table:
            continue
        out[obj] = {name: kbs["target_%d" % i] for i, name in enumerate(table)}
    return out


def world_basis(obj):
    """Neutral vertex positions of ``obj`` in world space (the shape keys are local)."""
    m = obj.matrix_world
    return [m @ v.co for v in obj.data.shape_keys.key_blocks["Basis"].data], m


def inner_lip_pair(obj, table, opener="Mouth_Drop_Lower"):
    """(upper, lower) vertex indices of the sealed inner-lip contact at the midline.

    The lower one is the head vertex on the face midline that ``opener`` drags furthest
    down; the upper one is its sealed partner -- the nearest vertex above it that the
    same key leaves in place.  In the neutral pose the two sit 0.087 units apart (the
    closed lip line), so the vertical distance between them IS the mouth opening.
    """
    basis, m = world_basis(obj)
    xs = [c.x for c in basis]
    xc, width = 0.5 * (min(xs) + max(xs)), max(xs) - min(xs)
    slab = [i for i in range(len(basis)) if abs(basis[i].x - xc) < 0.035 * width]
    kb = table[opener]
    dz = {i: (m @ kb.data[i].co).z - basis[i].z for i in slab}
    lo = min(slab, key=lambda i: dz[i])
    ref = abs(dz[lo])
    above = [i for i in slab if basis[i].z > basis[lo].z + 1e-4 and dz[i] > -0.05 * ref]
    up = min(above, key=lambda i: (basis[i] - basis[lo]).length)
    return up, lo


def lip_gap(obj, table, pair, weights):
    """Vertical gap (world units) between the two inner-lip vertices for a weight set."""
    basis, m = world_basis(obj)
    up, lo = pair
    a, b = basis[up].copy(), basis[lo].copy()
    for name, val in weights.items():
        kb = table.get(name)
        if kb is None or val == 0.0:
            continue
        a += ((m @ kb.data[up].co) - basis[up]) * val
        b += ((m @ kb.data[lo].co) - basis[lo]) * val
    return a.z - b.z


def cheek_width(obj, table, pair, weights):
    """Face width across the two cheek vertices the weight set pushes furthest out.

    Only vertices off the midline and around mouth height are considered: a plain bbox
    width is useless (the widest points of the head are the ears, which Cheek_Puff does
    not touch) and so are the mouth corners, which Cheek_Puff pulls *inwards*.
    """
    basis, m = world_basis(obj)
    pos = [c.copy() for c in basis]
    for name, val in weights.items():
        kb = table.get(name)
        if kb is None or val == 0.0:
            continue
        for i in range(len(basis)):
            pos[i] += ((m @ kb.data[i].co) - basis[i]) * val
    xs = [c.x for c in basis]
    zs = [c.z for c in basis]
    xc, width, height = 0.5 * (min(xs) + max(xs)), max(xs) - min(xs), max(zs) - min(zs)
    zm = 0.5 * (basis[pair[0]].z + basis[pair[1]].z)
    cheek = [i for i in range(len(basis))
             if abs(basis[i].x - xc) > 0.2 * width and abs(basis[i].z - zm) < 0.12 * height]
    side = {s: [i for i in cheek if (basis[i].x - xc) * s > 0.0] for s in (-1.0, 1.0)}
    ends = [max(v, key=lambda i: (pos[i].x - basis[i].x) * s) for s, v in side.items()]
    return (abs(basis[ends[0]].x - basis[ends[1]].x),
            abs(pos[ends[0]].x - pos[ends[1]].x))


def face_forward(obj):
    """Unit "out of the face" direction (world XY): head centre -> eye centre.

    Same trick ``check_render`` uses to aim the camera -- the eye meshes sit toward the
    face, so the vector from the head's centroid to theirs points forward.
    """
    basis, _ = world_basis(obj)
    hc = sum(basis, mathutils.Vector()) / len(basis)
    ec, n = mathutils.Vector(), 0
    for name in ("7", "8", "9", "10"):
        eye = bpy.data.objects.get(name)
        if eye is None:
            continue
        m = eye.matrix_world
        cos = [m @ v.co for v in eye.data.vertices]
        ec += sum(cos, mathutils.Vector()) / len(cos)
        n += 1
    if not n:
        return mathutils.Vector((0.0, -1.0, 0.0))
    fwd = mathutils.Vector((ec.x / n - hc.x, ec.y / n - hc.y, 0.0))
    fwd.normalize()
    return fwd


def mouth_corners(obj, table):
    """(left, right) mouth-corner vertex indices = what Mouth_Stretch_L/_R move most."""
    basis, m = world_basis(obj)
    out = []
    for key in ("Mouth_Stretch_L", "Mouth_Stretch_R"):
        kb = table[key]
        out.append(max(range(len(basis)),
                       key=lambda i: ((m @ kb.data[i].co) - basis[i]).length))
    return tuple(out)


def lip_center_verts(obj, table, pair, count=8):
    """The ``count`` frontmost midline vertices just above and just below the lip line.

    These are the lip-centre vertices whose forward displacement IS the protrusion of a
    pucker; upper and lower are kept apart because several CC4 keys move only one of them.
    """
    basis, _ = world_basis(obj)
    fwd = face_forward(obj)
    hc = sum(basis, mathutils.Vector()) / len(basis)
    xs = [c.x for c in basis]
    zs = [c.z for c in basis]
    xc, width, height = 0.5 * (min(xs) + max(xs)), max(xs) - min(xs), max(zs) - min(zs)
    zm = 0.5 * (basis[pair[0]].z + basis[pair[1]].z)
    mid = [i for i in range(len(basis)) if abs(basis[i].x - xc) < 0.03 * width]
    out = []
    for lo, hi in ((zm, zm + 0.03 * height), (zm - 0.03 * height, zm)):
        band = [i for i in mid if lo < basis[i].z < hi]
        band.sort(key=lambda i: -(basis[i] - hc).dot(fwd))
        out.append(band[:count])
    return tuple(out)


def posed(obj, table, weights, idx):
    """World-space positions of the vertices ``idx`` under a shape-key weight set."""
    basis, m = world_basis(obj)
    pos = {i: basis[i].copy() for i in idx}
    for name, val in weights.items():
        kb = table.get(name)
        if kb is None or val == 0.0:
            continue
        for i in idx:
            pos[i] += ((m @ kb.data[i].co) - basis[i]) * val
    return basis, pos


def lip_protrusion(obj, table, lips, weights):
    """(upper, lower) mean forward displacement of the lip-centre vertices, world units."""
    fwd = face_forward(obj)
    up, lo = lips
    basis, pos = posed(obj, table, weights, list(up) + list(lo))
    return tuple(sum((pos[i] - basis[i]).dot(fwd) for i in band) / len(band)
                 for band in (up, lo))


def mouth_width(obj, table, corners, weights):
    """(neutral, posed) distance between the two mouth-corner vertices."""
    basis, pos = posed(obj, table, weights, corners)
    a, b = corners
    return (basis[a] - basis[b]).length, (pos[a] - pos[b]).length


def chin_motion(obj, table, pair, weights):
    """Largest vertex displacement in the chin band below the mouth (distortion check)."""
    basis, _ = world_basis(obj)
    zs = [c.z for c in basis]
    height = max(zs) - min(zs)
    zm = 0.5 * (basis[pair[0]].z + basis[pair[1]].z)
    chin = [i for i in range(len(basis))
            if zm - 0.20 * height < basis[i].z < zm - 0.06 * height]
    basis, pos = posed(obj, table, weights, chin)
    return max((pos[i] - basis[i]).length for i in chin)
