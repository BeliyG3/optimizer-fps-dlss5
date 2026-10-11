"""The garden's trees and ferns (build_lab_garden.py), from the Poly Haven glTFs (read only).

island_tree_02 (one mesh, three materials): `island_tree_02` is the trunk (bench12's wind gives it weight 0),
`island_tree_02_leaves` 29781 leaf cards (24 triangles each), `island_tree_02_branches` the twigs (KHR_texture_transform
on its textures, baked into the UVs here: the bench reads no texture transform). 1.07 M triangles in all, decimated
(collapse) per material to at most 150 k. fern_02: four variants (784-2384 triangles), alpha cards.

The downloaded textures are JPEGs, so the glTFs' alpha is lost: the leaves' alpha is rebuilt from their diffuse map
(black around each leaf), the fern's from its ARM map (pure yellow, roughness 1, around each frond). Each becomes a
packed RGBA image wired as Color -> Base Color and Alpha -> Math Round -> Alpha (the exporter's alphaMode MASK,
cutoff 0.5). Instances are realised meshes (transforms applied, identity objects): `veg_tree_N`, `veg_fern_NN`.
"""
import json
from pathlib import Path

import bmesh
import bpy
import numpy as np
from mathutils import Matrix

from garden_grass_atlas import fill_transparent

TREE_BUDGET = {'island_tree_02': 13000, 'island_tree_02_branches': 34000, 'island_tree_02_leaves': 100000}
TREE_LIMIT = 150000
TREE_BASE_SCALE = 1.4  # the asset is a 3.4 m tree; 4.8 m reads as a tree beside the 8-15 m buildings
TREE_SINK = 0.05  # the trunk's flat bottom cap would show above the soil bed (1 cm) and the ground


def _import(path):
    before = set(bpy.data.objects)
    bpy.ops.import_scene.gltf(filepath=str(path))
    return [o for o in bpy.data.objects if o not in before]


def _image_array(image):
    w, h = image.size
    return np.array(image.pixels[:], np.float32).reshape(h, w, 4)


def _rgba_image(name, rgb, alpha):
    h, w = alpha.shape
    image = bpy.data.images.new(name, w, h, alpha=True)
    rgba = fill_transparent(np.concatenate([rgb, alpha[..., None]], axis=2), iterations=8)
    image.pixels.foreach_set(rgba.ravel())
    image.pack()
    print(f"[garden] {name}: {w}x{h}, opaque fraction {float((alpha >= 0.5).mean()):.3f}", flush=True)
    return image


def _alpha_chain(tree, bsdf):
    """Nodes between the BSDF's Alpha and the first image texture behind it (the importer's clip or blend chain)."""
    chain, socket = [], bsdf.inputs["Alpha"]
    while socket.is_linked:
        node = socket.links[0].from_node
        if node.type == 'TEX_IMAGE':
            break
        chain.append(node)
        socket = next((s for s in node.inputs if s.is_linked), None)
        if socket is None:
            break
    return chain


def cut_out(material, image):
    """The base colour texture gets `image`; its alpha drives the BSDF's Alpha through Math Round."""
    tree = material.node_tree
    bsdf = next(n for n in tree.nodes if n.type == 'BSDF_PRINCIPLED')
    texture = bsdf.inputs["Base Color"].links[0].from_node
    if texture.type != 'TEX_IMAGE':
        raise RuntimeError(f"{material.name}: base colour is not a plain image texture")
    for node in _alpha_chain(tree, bsdf):
        tree.nodes.remove(node)
    for link in list(bsdf.inputs["Alpha"].links):
        tree.links.remove(link)
    texture.image = image
    cut = tree.nodes.new("ShaderNodeMath")
    cut.operation = 'ROUND'
    tree.links.new(texture.outputs["Alpha"], cut.inputs[0])
    tree.links.new(cut.outputs["Value"], bsdf.inputs["Alpha"])
    material.use_backface_culling = False


def _texture(material, name_part):
    tree = material.node_tree
    return next(n.image for n in tree.nodes if n.type == 'TEX_IMAGE' and n.image and name_part in n.image.name)


def bake_texture_transform(obj, material_name, gltf_path):
    """KHR_texture_transform (offset, scale; no rotation) of the material's base colour applied to the UVs of its
    faces, and the Mapping nodes bypassed (all three textures share the transform in this asset)."""
    doc = json.loads(Path(gltf_path).read_text())
    spec = next(m for m in doc['materials'] if m['name'] == material_name)
    transform = spec['pbrMetallicRoughness']['baseColorTexture']['extensions']['KHR_texture_transform']
    (sx, sy), (ox, oy) = transform.get('scale', (1, 1)), transform.get('offset', (0, 0))
    if transform.get('rotation', 0):
        raise RuntimeError("texture rotation is not handled")
    mesh = obj.data
    index = mesh.materials.find(material_name)
    uv = mesh.uv_layers[0].data
    for polygon in mesh.polygons:
        if polygon.material_index == index:
            for loop in polygon.loop_indices:
                u, v = uv[loop].uv
                uv[loop].uv = (u * sx + ox, 1 - ((1 - v) * sy + oy))  # glTF v runs down
    tree = mesh.materials[index].node_tree
    for node in [n for n in tree.nodes if n.type in ('MAPPING', 'UVMAP')]:
        tree.nodes.remove(node)
    print(f"[garden] {material_name}: texture transform scale ({sx}, {sy}) offset ({ox}, {oy}) baked into the UVs",
          flush=True)


def _split_decimate(obj):
    """One mesh: each material's faces decimated (collapse) to its budget, joined again."""
    depsgraph = bpy.context.evaluated_depsgraph_get()
    joined = bmesh.new()
    for index, material in enumerate(obj.data.materials):
        part = obj.data.copy()
        bm = bmesh.new()
        bm.from_mesh(part)
        bmesh.ops.delete(bm, geom=[f for f in bm.faces if f.material_index != index], context='FACES')
        triangles = sum(len(f.verts) - 2 for f in bm.faces)
        bm.to_mesh(part)
        bm.free()
        holder = bpy.data.objects.new("decimate_" + material.name, part)
        bpy.context.scene.collection.objects.link(holder)
        modifier = holder.modifiers.new("decimate", 'DECIMATE')
        modifier.decimate_type, modifier.use_collapse_triangulate = 'COLLAPSE', True
        modifier.ratio = min(1.0, TREE_BUDGET[material.name] / triangles)
        depsgraph.update()
        reduced = bpy.data.meshes.new_from_object(holder.evaluated_get(depsgraph))
        joined.from_mesh(reduced)
        print(f"[garden] {material.name}: {triangles} -> {len(reduced.polygons)} triangles "
              f"(ratio {modifier.ratio:.3f})", flush=True)
        bpy.data.objects.remove(holder, do_unlink=True)
        for mesh in (part, reduced):
            bpy.data.meshes.remove(mesh)
    mesh = bpy.data.meshes.new("island_tree_02_decimated")
    joined.to_mesh(mesh)
    joined.free()
    for material in obj.data.materials:
        mesh.materials.append(material)
    total = sum(len(p.vertices) - 2 for p in mesh.polygons)
    if total > TREE_LIMIT:
        raise RuntimeError(f"the decimated tree has {total} triangles (limit {TREE_LIMIT})")
    return mesh


def tree_template(gltf_path):
    """(mesh at the tree's local axes with its trunk base at the origin, triangles)."""
    objects = _import(gltf_path)
    obj = next(o for o in objects if o.type == 'MESH')
    materials = {m.name: m for m in obj.data.materials}
    leaves = materials['island_tree_02_leaves']
    diffuse = _image_array(_texture(leaves, 'leaves_diff'))
    alpha = np.clip((diffuse[..., :3].max(axis=2) - 0.03) / 0.05, 0.0, 1.0)
    cut_out(leaves, _rgba_image("island_tree_02_leaves_rgba", diffuse[..., :3], alpha))
    bake_texture_transform(obj, 'island_tree_02_branches', gltf_path)
    mesh = _split_decimate(obj)
    trunk = mesh.materials.find('island_tree_02')
    co = np.array([v.co for v in mesh.vertices])
    used = np.zeros(len(co), bool)
    for p in mesh.polygons:
        if p.material_index == trunk:
            used[list(p.vertices)] = True
    low = co[used & (co[:, 2] < co[used, 2].min() + 0.15)]
    base = low.mean(axis=0)
    mesh.transform(Matrix.Translation((-base[0], -base[1], -co[:, 2].min())))
    _remove(objects)
    triangles = sum(len(p.vertices) - 2 for p in mesh.polygons)
    print(f"[garden] tree template: {triangles} triangles, height {co[:, 2].max() - co[:, 2].min():.2f} m", flush=True)
    return mesh, triangles


def fern_templates(gltf_path):
    """The four variants' meshes (local axes, as authored around the origin)."""
    objects = _import(gltf_path)
    meshes = sorted((o for o in objects if o.type == 'MESH'), key=lambda o: o.name)
    material = meshes[0].data.materials[0]
    diffuse = _image_array(_texture(material, 'diff'))
    arm = _image_array(_texture(material, 'rough'))
    alpha = np.clip((0.95 - arm[..., 1]) / 0.04, 0.0, 1.0)  # roughness (G) is 1 off the fronds
    cut_out(material, _rgba_image("fern_02_rgba", diffuse[..., :3], alpha))
    templates = [o.data for o in meshes]
    for mesh in templates:
        mesh.use_fake_user = False
    _remove(objects, keep_meshes=templates)
    print(f"[garden] fern variants: {[m.name for m in templates]} "
          f"{[len(m.polygons) for m in templates]} faces", flush=True)
    return templates


def _remove(objects, keep_meshes=()):
    for obj in objects:
        data = obj.data
        bpy.data.objects.remove(obj, do_unlink=True)
        if data is not None and data not in keep_meshes and getattr(data, 'users', 1) == 0 and isinstance(data, bpy.types.Mesh):
            bpy.data.meshes.remove(data)


def place(template, name, x, y, turn, scale, collection, z=0.0):
    """A realised copy of template rotated by turn about z, scaled, standing at (x, y, z)."""
    mesh = template.copy()
    mesh.name = name
    mesh.transform(Matrix.Translation((x, y, z)) @ Matrix.Rotation(turn, 4, 'Z') @ Matrix.Scale(scale, 4))
    obj = bpy.data.objects.new(name, mesh)
    collection.objects.link(obj)
    return obj


def build(tree_gltf, fern_gltf, trees, ferns, collection):
    """veg_tree_N and veg_fern_NN. Returns (objects, triangles per tree, triangles of all ferns)."""
    tree_mesh, tree_triangles = tree_template(tree_gltf)
    objects = [place(tree_mesh, f"veg_tree_{k}", x, y, turn, TREE_BASE_SCALE * s, collection, -TREE_SINK)
               for k, (x, y, turn, s) in enumerate(trees)]
    variants = fern_templates(fern_gltf)
    ferns_objects = [place(variants[v], f"veg_fern_{k:02d}", x, y, turn, s, collection)
                     for k, (x, y, turn, s, v) in enumerate(ferns)]
    bpy.data.meshes.remove(tree_mesh)
    for mesh in variants:
        bpy.data.meshes.remove(mesh)
    fern_triangles = sum(len(p.vertices) - 2 for o in ferns_objects for p in o.data.polygons)
    print(f"[garden] {len(objects)} trees of {tree_triangles} triangles, {len(ferns_objects)} ferns of "
          f"{fern_triangles} triangles in all", flush=True)
    return objects + ferns_objects, tree_triangles, fern_triangles
