"""The outdoor yard of lab_scene_outdoor (build_lab_outdoor.py): the opening in the hall's east wall and the yard.

Blender axes, metres (bench x = Blender x, bench z = Blender y, up = Blender z). The hall's east wall is part of
`shell_walls`: its inner face at x = 9.65, its outer face at x = 9.95, 0-9 m high; the hall floor ends at x = 9.8.
The yard lies east of it: 40 m deep (x from 9.8) and 60 m long, centred on the camera's exit. Every yard mesh is a
simple box or quad with Principled materials; the textures are images already in lab_scene.blend.
"""
import bmesh
import bpy
from mathutils import Vector

WALL = "shell_walls"
WALL_INNER, WALL_OUTER = 9.65, 9.95
WALL_Y = (-12.0, 15.0)              # the east wall's extent; the south wall's end cap at y -12.15..-11.85 is left alone
YARD_X0, YARD_DEPTH, YARD_LENGTH = 9.8, 40.0, 60.0
COLLECTION = "Yard"
IMAGES = dict(concrete="smooth_concrete_floor_diff_2k.jpg.001", wall="concrete_wall_008_diff_2k.jpg.001",
              iron="corrugated_iron_02_diff_2k.jpg")


# ---- materials ----------------------------------------------------------------------------------------------------
def _principled(name):
    mat = bpy.data.materials.new(name)
    mat.use_nodes = True
    return mat, mat.node_tree.nodes["Principled BSDF"]


def plain(name, colour, roughness):
    mat, bsdf = _principled(name)
    bsdf.inputs["Base Color"].default_value = (*colour, 1.0)
    bsdf.inputs["Roughness"].default_value = roughness
    return mat


def textured(name, image, tint, roughness):
    """Base colour = image x tint (Mix MULTIPLY, factor 1: the pattern the glTF exporter carries)."""
    mat, bsdf = _principled(name)
    bsdf.inputs["Roughness"].default_value = roughness
    nodes, links = mat.node_tree.nodes, mat.node_tree.links
    texture = nodes.new("ShaderNodeTexImage")
    texture.image = bpy.data.images[image]
    mix = nodes.new("ShaderNodeMix")
    mix.data_type, mix.blend_type = "RGBA", "MULTIPLY"
    mix.inputs["Factor"].default_value = 1.0
    links.new(texture.outputs["Color"], mix.inputs["A"])
    mix.inputs["B"].default_value = (*tint, 1.0)
    links.new(mix.outputs["Result"], bsdf.inputs["Base Color"])
    return mat


def emissive(name, colour, strength):
    """A constant emission (the lightmap's sun bake mutes its strength): one lamp group per material name in bench12."""
    mat, bsdf = _principled(name)
    bsdf.inputs["Base Color"].default_value = (0.0, 0.0, 0.0, 1.0)
    bsdf.inputs["Emission Color"].default_value = (*colour, 1.0)
    bsdf.inputs["Emission Strength"].default_value = strength
    return mat


# ---- meshes -------------------------------------------------------------------------------------------------------
BOX_FACES = [(0, 1, 3, 2), (4, 6, 7, 5), (0, 4, 5, 1), (2, 3, 7, 6), (0, 2, 6, 4), (1, 5, 7, 3)]  # build_lab_scene.box
BOTTOM, TOP = (0, 4, 6, 2), (1, 5, 7, 3)


def _box_parts(lo, hi, bottom):
    corners = [(hi[0] if sx else lo[0], hi[1] if sy else lo[1], hi[2] if sz else lo[2])
               for sx in (0, 1) for sy in (0, 1) for sz in (0, 1)]
    faces = [f for f in BOX_FACES if bottom or set(f) != set(BOTTOM)]
    return corners, faces


def _box_uvs(mesh, tile):
    """Box-projected UVs, one repeat per `tile` metres (build_lab_scene.box)."""
    uv = mesh.uv_layers.new(name="UVMap")
    for polygon in mesh.polygons:
        axis = max(range(3), key=lambda k: abs(polygon.normal[k]))
        u_axis, v_axis = [(1, 2), (0, 2), (0, 1)][axis]
        for loop in polygon.loop_indices:
            position = mesh.vertices[mesh.loops[loop].vertex_index].co
            uv.data[loop].uv = (position[u_axis] / tile, position[v_axis] / tile)


def mesh_object(name, parts, collection, tile=2.5):
    """One object from [(lo, hi, material, bottom[, top material])] boxes (world coordinates, identity transform)."""
    verts, faces, slots, materials = [], [], [], []
    for lo, hi, material, bottom, *top in parts:
        corners, box_faces = _box_parts(lo, hi, bottom)
        for face in box_faces:
            chosen = top[0] if top and face == TOP else material
            if chosen not in materials:
                materials.append(chosen)
            faces.append(tuple(len(verts) + i for i in face))
            slots.append(materials.index(chosen))
        verts += corners
    mesh = bpy.data.meshes.new(name)
    mesh.from_pydata(verts, [], faces)
    for material in materials:
        mesh.materials.append(material)
    mesh.polygons.foreach_set("material_index", slots)
    _box_uvs(mesh, tile)
    mesh.validate()
    obj = bpy.data.objects.new(name, mesh)
    collection.objects.link(obj)
    return obj


def quad(name, lo, hi, material, collection, tile):
    """An upward quad from lo to hi (x, y) at z = 0."""
    mesh = bpy.data.meshes.new(name)
    mesh.from_pydata([(lo[0], lo[1], 0.0), (hi[0], lo[1], 0.0), (hi[0], hi[1], 0.0), (lo[0], hi[1], 0.0)], [], [(0, 1, 2, 3)])
    mesh.materials.append(material)
    _box_uvs(mesh, tile)
    obj = bpy.data.objects.new(name, mesh)
    collection.objects.link(obj)
    return obj


# ---- the opening ----------------------------------------------------------------------------------------------------
def _east_faces(bm):
    return [f for f in bm.faces if all(WALL_INNER - 1e-4 <= v.co.x <= WALL_OUTER + 1e-4 for v in f.verts)
            and min(v.co.y for v in f.verts) >= WALL_Y[0] - 1e-4]


def _vert(bm, co):
    for v in bm.verts:
        if (v.co - co).length < 1e-4:
            return v
    return bm.verts.new(co)


def _uv_scale(bm, uv):
    """Metres per UV unit on the wall (its texture's tile), from its inner face's longest edge."""
    face = max(_east_faces(bm), key=lambda f: f.calc_area())
    a, b = face.loops[0], face.loops[1]
    return (a.vert.co - b.vert.co).length / max((a[uv].uv - b[uv].uv).length, 1e-6)


def cut_opening(y0, y1, z1):
    """Removes the east wall's faces inside y0..y1, 0..z1 (bisecting them there) and closes the wall's thickness with
    two jambs and a lintel. Returns the removed faces as [centre, normal, area]."""
    obj = bpy.data.objects[WALL]
    if any(abs(obj.matrix_world[i][j] - (i == j)) > 1e-6 for i in range(4) for j in range(4)):
        raise RuntimeError(f"{WALL} is expected with an identity transform")
    bm = bmesh.new()
    bm.from_mesh(obj.data)
    uv = bm.loops.layers.uv.active
    tile = _uv_scale(bm, uv)
    for co, normal in (((0, y0, 0), (0, 1, 0)), ((0, y1, 0), (0, 1, 0)), ((0, 0, z1), (0, 0, 1))):
        faces = _east_faces(bm)
        geom = list({e for f in faces for e in f.edges}) + list({v for f in faces for v in f.verts}) + faces
        bmesh.ops.bisect_plane(bm, geom=geom, plane_co=co, plane_no=normal, dist=1e-5)
    inside = [f for f in _east_faces(bm) if y0 < f.calc_center_median().y < y1 and f.calc_center_median().z < z1]
    removed = [[list(f.calc_center_median()), list(f.normal), f.calc_area()] for f in inside]
    material = inside[0].material_index if inside else 0
    bmesh.ops.delete(bm, geom=inside, context='FACES')
    i, o = WALL_INNER, WALL_OUTER
    for corners, outward in ((((i, y0, 0), (o, y0, 0), (o, y0, z1), (i, y0, z1)), (0, 1, 0)),
                             (((i, y1, 0), (i, y1, z1), (o, y1, z1), (o, y1, 0)), (0, -1, 0)),
                             (((i, y0, z1), (o, y0, z1), (o, y1, z1), (i, y1, z1)), (0, 0, -1))):
        face = bm.faces.new([_vert(bm, Vector(c)) for c in corners])
        face.normal_update()
        if face.normal.dot(Vector(outward)) < 0:
            face.normal_flip()
        face.material_index = material
        axis = max(range(3), key=lambda k: abs(face.normal[k]))
        u_axis, v_axis = [(1, 2), (0, 2), (0, 1)][axis]
        for loop in face.loops:
            loop[uv].uv = (loop.vert.co[u_axis] / tile, loop.vert.co[v_axis] / tile)
    bm.to_mesh(obj.data)
    bm.free()
    obj.data.update()
    return removed


# ---- the yard -------------------------------------------------------------------------------------------------------
def build_yard(exit_y):
    """The yard's objects in a collection of their own (exported after the lab's nodes). Returns their names."""
    scene = bpy.context.scene
    collection = bpy.data.collections.new(COLLECTION)
    scene.collection.children.link(collection)
    ground = textured("yard_concrete", IMAGES["concrete"], (0.42, 0.42, 0.42), 0.85)
    wall = textured("yard_wall", IMAGES["wall"], (0.55, 0.50, 0.44), 0.9)
    iron = textured("yard_iron", IMAGES["iron"], (0.40, 0.44, 0.50), 0.6)
    roof = plain("yard_roof", (0.08, 0.08, 0.09), 0.9)
    wood = plain("yard_crate_wood", (0.33, 0.21, 0.10), 0.75)
    blue = plain("yard_crate_blue", (0.08, 0.18, 0.40), 0.6)
    metal = plain("yard_metal", (0.22, 0.23, 0.24), 0.5)
    dark = plain("yard_pole", (0.10, 0.10, 0.11), 0.5)
    lamps = [emissive(f"yard_lamp_{k}", (1.0, 0.86, 0.66), 60.0) for k in "abc"]

    y0, y1 = exit_y - YARD_LENGTH / 2, exit_y + YARD_LENGTH / 2
    x1 = YARD_X0 + YARD_DEPTH
    objects = [quad("yard_ground", (YARD_X0, y0), (x1, y1), ground, collection, tile=4.0)]

    # Buildings (x0, y0, x1, y1, height, wall material): ahead of the exit, to its right, its left, and further south.
    for k, (bx0, by0, bx1, by1, h, skin) in enumerate(((24, 20, 36, 32, 15, wall), (38, 2, 48, 14, 12, iron),
                                                       (12.5, 27, 20.5, 35, 10, wall), (34, -21, 46, -9, 8, iron))):
        objects.append(mesh_object(f"yard_building_{k}", [((bx0, by0, 0.0), (bx1, by1, h), skin, False, roof)], collection, tile=3.0))

    # Crate stacks: 1.2 m crates on the ground and on each other.
    s = 1.2
    stacks = (((19, 12), [(0, 0, 0), (1, 0, 0), (0, 1, 0), (1, 1, 0), (0.5, 0.5, 1)]),
              ((27, -2), [(0, 0, 0), (1, 0, 0), (2, 0, 0), (0.5, 0, 1), (1.5, 0, 1)]),
              ((33, 14), [(0, 0, 0), (0, 0, 1), (1, 0, 0)]))
    for k, ((cx, cy), cells) in enumerate(stacks):
        parts = [((cx + i * s + 0.02, cy + j * s + 0.02, l * s), (cx + (i + 1) * s - 0.02, cy + (j + 1) * s - 0.02, (l + 1) * s),
                  blue if (k + int(i + j + l)) % 3 == 0 else wood, False) for i, j, l in cells]
        objects.append(mesh_object(f"yard_crates_{k}", parts, collection, tile=1.2))

    # Fence along the yard's three outer edges: posts every 2.5 m, two rails.
    inset, height = 0.3, 2.0
    fx, fy0, fy1 = x1 - inset, y0 + inset, y1 - inset
    runs = (((WALL_OUTER + 0.2, fy0), (fx, fy0)), ((fx, fy0), (fx, fy1)), ((WALL_OUTER + 0.2, fy1), (fx, fy1)))
    parts = []
    for (ax, ay), (bx, by) in runs:
        length = max(abs(bx - ax), abs(by - ay))
        count = int(length // 2.5) + 1
        for n in range(count):
            t = n / (count - 1)
            px, py = ax + (bx - ax) * t, ay + (by - ay) * t
            parts.append(((px - 0.05, py - 0.05, 0.0), (px + 0.05, py + 0.05, height), metal, False))
        for z in (0.9, 1.8):
            lo = (min(ax, bx) - 0.04, min(ay, by) - 0.04, z)
            hi = (max(ax, bx) + 0.04, max(ay, by) + 0.04, z + 0.08)
            parts.append((lo, hi, metal, True))
    objects.append(mesh_object("yard_fence", parts, collection, tile=2.5))

    # Light poles: 8 m, an arm towards -x and an emissive head under its end; each head its own lamp material.
    for k, ((px, py), lamp) in enumerate(zip(((16, 16), (28, 10), (36, -4)), lamps)):
        parts = [((px - 0.11, py - 0.11, 0.0), (px + 0.11, py + 0.11, 8.0), dark, False),
                 ((px - 1.6, py - 0.06, 7.85), (px - 0.11, py + 0.06, 7.97), dark, True),
                 ((px - 1.9, py - 0.2, 7.7), (px - 1.3, py + 0.2, 7.85), lamp, True)]
        objects.append(mesh_object(f"yard_pole_{'abc'[k]}", parts, collection, tile=1.0))
    return [o.name for o in objects]
