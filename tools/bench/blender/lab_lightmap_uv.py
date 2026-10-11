"""Which lab meshes are baked, and their second UV map `Lightmap` (TEXCOORD_1 in the glTF), for bake_lab_lightmaps.py.

Static = every MESH object without an armature modifier, an armature parent or animation (on the object, a parent or
its shape keys). Dynamic objects (the characters) are not baked and get no Lightmap layer; nor do the garden's foliage
(`veg_*`) and props (`prop_*`), which bench12 lights and shadows dynamically.

Every static mesh is made single-user first: the lab instances 169 meshes over 495 objects, and each instance needs
texels of its own in the atlas. Text objects become meshes, so they can carry the layer (the exporter already writes
them as meshes). The UVs themselves are packed on one world-space copy of the static meshes (lab_lightmap_proxy.py),
built from the mesh data: unsupported_geometry() refuses objects whose export (modifiers applied) would differ.
"""
import bpy
import numpy as np

LAYER = "Lightmap"
UNBAKED_PREFIXES = ("veg_", "prop_")  # the garden's foliage (bench12's wind nodes, wind.cpp IsWindNode) and props
TEXTURE_LAYER = "UVMap"  # the first UV map's name in the proxy when the sources disagree, so the join keeps one map


def _lineage(obj):
    while obj is not None:
        yield obj
        obj = obj.parent


def _animated(idb):
    data = idb.animation_data
    return data is not None and (data.action is not None or len(data.nla_tracks) > 0 or len(data.drivers) > 0)


def is_dynamic(obj):
    if any(m.type == 'ARMATURE' for m in obj.modifiers):
        return True
    if any(parent.type == 'ARMATURE' for parent in _lineage(obj.parent)):
        return True
    if any(_animated(node) for node in _lineage(obj)):
        return True
    keys = getattr(obj.data, 'shape_keys', None)
    return keys is not None and _animated(keys)


def convert_texts(scene):
    """Static text objects become MESH objects in place (same name, same evaluated geometry). Returns their names."""
    texts = [o for o in scene.objects if o.type == 'FONT' and not is_dynamic(o)]
    if texts:
        bpy.ops.object.select_all(action='DESELECT')
        for obj in texts:
            obj.select_set(True)
        bpy.context.view_layer.objects.active = texts[0]
        bpy.ops.object.convert(target='MESH')
    return [o.name for o in texts]


def is_unbaked(obj):
    """The garden's grass, trees, ferns and props (build_lab_garden.py): never baked, lit and shadowed by bench12 itself."""
    return obj.name.startswith(UNBAKED_PREFIXES)


def classify(scene):
    """(static, dynamic, unbaked) MESH objects, in the scene's object order. Unbaked (veg_, prop_) is neither: no
    Lightmap layer."""
    meshes = [o for o in scene.objects if o.type == 'MESH']
    unbaked = [o for o in meshes if is_unbaked(o)]
    meshes = [o for o in meshes if not is_unbaked(o)]
    return [o for o in meshes if not is_dynamic(o)], [o for o in meshes if is_dynamic(o)], unbaked


def unsupported_geometry(static):
    """Static objects whose evaluated mesh can differ from their mesh data: an enabled modifier, or a non-basis shape
    key with a non-zero value. The proxy is built from mesh data, the export applies modifiers."""
    problems = []
    for obj in static:
        modifiers = [m.name for m in obj.modifiers if m.show_viewport or m.show_render]
        keys = obj.data.shape_keys
        shapes = [k.name for k in keys.key_blocks[1:] if k.value != 0.0 and not k.mute] if keys else []
        if modifiers or shapes:
            problems.append(f"{obj.name}: modifiers {modifiers}, shape keys {shapes}")
    return problems


def make_single_user(static, dynamic):
    """Gives every static object a mesh of its own (the first user keeps the original). Returns the number of copies."""
    seen = {o.data.as_pointer() for o in dynamic}
    copies = 0
    for obj in static:
        key = obj.data.as_pointer()
        if key in seen:
            obj.data = obj.data.copy()
            copies += 1
        else:
            seen.add(key)
    return copies


def add_layers(static):
    """Adds `Lightmap` as the second UV map, so it is TEXCOORD_1 everywhere. A mesh without UVs first gets a constant
    one: Blender (0, 1) is glTF (0, 0) (the exporter flips V), what the bench reads for a missing TEXCOORD_0, and the
    same texel under repeat wrapping. Returns the number of such meshes."""
    placeholders = 0
    for obj in static:
        layers = obj.data.uv_layers
        if LAYER in layers:
            raise RuntimeError(f"{obj.name}: already has a {LAYER} UV map")
        if len(layers) > 1:
            raise RuntimeError(f"{obj.name}: {len(layers)} UV maps, {LAYER} would not be TEXCOORD_1")
        if not layers:
            constant = layers.new(name=TEXTURE_LAYER, do_init=False)
            constant.data.foreach_set('uv', np.tile(np.array([0.0, 1.0], np.float32), len(obj.data.loops)))
            placeholders += 1
        texture = layers[0]
        lightmap = layers.new(name=LAYER, do_init=False)
        texture.active_render = True  # image textures keep sampling the first map, in the bake and in the export
        layers.active = lightmap
    return placeholders


def texture_layer_name(static):
    """The proxy's first UV map: the sources' common name, so material nodes naming it keep working, else UVMap."""
    names = {obj.data.uv_layers[0].name for obj in static}
    return names.pop() if len(names) == 1 else TEXTURE_LAYER


def node_trees(roots):
    """The given node trees and every node group inside them, once each."""
    seen, pending = [], [tree for tree in roots if tree is not None]
    while pending:
        tree = pending.pop()
        if tree not in seen:
            seen.append(tree)
            pending += [n.node_tree for n in tree.nodes if n.type == 'GROUP' and n.node_tree is not None]
    return seen


def _uv_reference(node):
    """The UV map a node names ('' = the active render map)."""
    tangent = node.type == 'NORMAL_MAP' and node.space == 'TANGENT'
    if node.type == 'UVMAP' or tangent or (node.type == 'TANGENT' and node.direction_type == 'UV_MAP'):
        return node.uv_map
    return node.attribute_name if node.type == 'ATTRIBUTE' and node.attribute_type == 'GEOMETRY' else ''


def uv_name_mismatches(static, proxy_name):
    """Material nodes naming a UV map that resolves differently on the proxy (first map renamed to proxy_name) than on a
    source object using the material. Returns (nodes naming a UV map, mismatches as text)."""
    users = {}
    for obj in static:
        for slot in obj.material_slots:
            if slot.material is not None and slot.material.node_tree is not None:
                users.setdefault(slot.material, []).append(obj)
    named, mismatches = 0, []
    for material, objects in users.items():
        for tree in node_trees([material.node_tree]):
            for node in tree.nodes:
                name = _uv_reference(node)
                if not name:
                    continue
                named += 1
                in_proxy = {proxy_name: 0, LAYER: 1}.get(name, -1)
                differ = [o.name for o in objects if o.data.uv_layers.find(name) != in_proxy]
                if differ:
                    mismatches.append(f"{material.name}/{tree.name}/{node.name} names '{name}' ({len(differ)} objects, e.g. {differ[:3]})")
    return named, mismatches
