"""One world-space copy of all static meshes (the proxy), for packing the lightmap UVs and for the bake.

- Packing: Lightmap Pack makes one box per face; with the lab's 1.1 M faces its smallest box swallows the atlas. Smart
  UV Project on the proxy packs the islands of every object into one atlas, at one texel density in metres (the
  proxy has no object transforms).
- Baking one object runs one Cycles session instead of one per object.

Each face and loop of the proxy remembers its source (generic attributes, which the join keeps), so the packed UVs
go back to the source meshes loop by loop, and check() compares the proxy with the scene it stands for.
"""
import math

import bpy
import numpy as np

from lab_lightmap_uv import LAYER

SRC_OBJECT, SRC_LOOP = "pw_src_object", "pw_src_loop"


def _get(collection, prop, count, dtype, width=1):
    values = np.empty(count * width, dtype)
    collection.foreach_get(prop, values)
    return values.reshape(-1, width) if width > 1 else values


def _select_only(obj):
    bpy.ops.object.select_all(action='DESELECT')
    obj.select_set(True)
    bpy.context.view_layer.objects.active = obj


def _slot_materials(obj):
    """The object's materials per slot (object-linked slots included)."""
    return [slot.material for slot in obj.material_slots]


def build(static, scene, texture_name):
    """The joined world-space copy; every first UV map is named texture_name, the materials are the objects' slots."""
    copies = []
    for index, obj in enumerate(static):
        mesh = obj.data.copy()
        mesh.transform(obj.matrix_world)
        mesh.uv_layers[0].name = texture_name
        for slot, material in enumerate(_slot_materials(obj)):
            mesh.materials[slot] = material
        mesh.attributes.new(SRC_OBJECT, 'INT', 'FACE').data.foreach_set('value', np.full(len(mesh.polygons), index, np.int32))
        mesh.attributes.new(SRC_LOOP, 'INT', 'CORNER').data.foreach_set('value', np.arange(len(mesh.loops), dtype=np.int32))
        copy = bpy.data.objects.new(f"lightmap_proxy_{index}", mesh)
        scene.collection.objects.link(copy)
        copies.append(copy)
    bpy.ops.object.select_all(action='DESELECT')
    for copy in copies:
        copy.select_set(True)
    bpy.context.view_layer.objects.active = copies[0]
    bpy.ops.object.join()
    proxy = bpy.context.view_layer.objects.active
    proxy.name = proxy.data.name = "lightmap_proxy"
    layers = proxy.data.uv_layers
    if [layer.name for layer in layers] != [texture_name, LAYER]:
        raise RuntimeError(f"proxy UV maps {[layer.name for layer in layers]}")
    layers[texture_name].active_render = True
    layers.active = layers[LAYER]
    return proxy


def pack(proxy, size, margin_texels, angle_limit=66.0):
    """Smart UV Project over the whole proxy, then Pack Islands again: Smart UV Project's own packing fills 40 % of the
    lab's atlas, the second pass 57 %. The island margin is a fraction of the atlas (margin_texels / size)."""
    _select_only(proxy)
    bpy.ops.object.mode_set(mode='EDIT')
    bpy.ops.mesh.select_all(action='SELECT')
    margin = margin_texels / size
    bpy.ops.uv.smart_project(angle_limit=math.radians(angle_limit), margin_method='FRACTION', island_margin=margin,
                             area_weight=0.0, correct_aspect=False, scale_to_bounds=False)
    bpy.ops.uv.pack_islands(udim_source='CLOSEST_UDIM', rotate=True, rotate_method='ANY', scale=True, merge_overlap=False,
                            margin_method='FRACTION', margin=margin, shape_method='CONCAVE')
    bpy.ops.object.mode_set(mode='OBJECT')


def _sources(proxy):
    """Per proxy loop: the source object index, the source loop index, and the proxy face."""
    mesh = proxy.data
    faces, loops = len(mesh.polygons), len(mesh.loops)
    starts = _get(mesh.polygons, 'loop_start', faces, np.int32)
    totals = _get(mesh.polygons, 'loop_total', faces, np.int32)
    if starts[0] != 0 or np.any(starts[1:] != starts[:-1] + totals[:-1]):
        raise RuntimeError("proxy loops are not stored face by face")
    loop_face = np.repeat(np.arange(faces, dtype=np.int64), totals)
    loop_object = _get(mesh.attributes[SRC_OBJECT].data, 'value', faces, np.int32)[loop_face]
    loop_source = _get(mesh.attributes[SRC_LOOP].data, 'value', loops, np.int32)
    return loop_object, loop_source, loop_face


def _per_object(proxy, static):
    """Yields (object, proxy loop indices, their source loop indices); each source loop exactly once."""
    loop_object, loop_source, _ = _sources(proxy)
    order = np.argsort(loop_object, kind='stable')
    counts = np.bincount(loop_object, minlength=len(static))
    start = 0
    for index, obj in enumerate(static):
        count = len(obj.data.loops)
        if counts[index] != count:
            raise RuntimeError(f"{obj.name}: {counts[index]} proxy loops for {count} loops")
        rows = order[start:start + count]
        start += count
        if np.any(np.bincount(loop_source[rows], minlength=count) != 1):
            raise RuntimeError(f"{obj.name}: proxy loops do not cover its loops one to one")
        yield obj, rows, loop_source[rows]


def copy_uvs_back(proxy, static):
    uv = _get(proxy.data.uv_layers[LAYER].data, 'uv', len(proxy.data.loops), np.float32, 2)
    for obj, rows, source in _per_object(proxy, static):
        target = np.empty((len(obj.data.loops), 2), np.float32)
        target[source] = uv[rows]
        obj.data.uv_layers[LAYER].data.foreach_set('uv', target.ravel())


def _corner_normals(mesh):
    return _get(mesh.corner_normals, 'vector', len(mesh.loops), np.float32, 3)


def check(proxy, static):
    """Compares every proxy loop with its source: world position, shading normal, texture UV and material."""
    mesh = proxy.data
    loops = len(mesh.loops)
    co = _get(mesh.vertices, 'co', len(mesh.vertices), np.float32, 3)
    position = co[_get(mesh.loops, 'vertex_index', loops, np.int32)]
    normal = _corner_normals(mesh)
    texture_uv = _get(mesh.uv_layers[0].data, 'uv', loops, np.float32, 2)
    _, _, loop_face = _sources(proxy)
    names = [m.name if m else "" for m in mesh.materials]
    material = np.array(names, dtype=object)[_get(mesh.polygons, 'material_index', len(mesh.polygons), np.int32)][loop_face]
    worst_position, worst_uv, bent, wrong_material = 0.0, 0.0, 0, 0
    for obj, rows, source in _per_object(proxy, static):
        data = obj.data
        n = len(data.loops)
        matrix = np.array(obj.matrix_world, dtype=np.float64)
        local = _get(data.vertices, 'co', len(data.vertices), np.float32, 3)[_get(data.loops, 'vertex_index', n, np.int32)]
        world = local @ matrix[:3, :3].T + matrix[:3, 3]
        worst_position = max(worst_position, float(np.abs(world[source] - position[rows]).max()))
        expected = _corner_normals(data) @ np.linalg.inv(matrix[:3, :3])  # rows times the inverse: the inverse transpose
        expected /= np.maximum(np.linalg.norm(expected, axis=1, keepdims=True), 1e-12)
        bent += int(np.count_nonzero(np.sum(expected[source] * normal[rows], axis=1) < 0.999))
        worst_uv = max(worst_uv, float(np.abs(_get(data.uv_layers[0].data, 'uv', n, np.float32, 2)[source] - texture_uv[rows]).max()))
        faces = np.repeat(np.arange(len(data.polygons)), _get(data.polygons, 'loop_total', len(data.polygons), np.int32))
        own = np.array([m.name if m else "" for m in _slot_materials(obj)] or [""], dtype=object)
        index = np.minimum(_get(data.polygons, 'material_index', len(data.polygons), np.int32), len(own) - 1)
        wrong_material += int(np.count_nonzero(own[index][faces][source] != material[rows]))
    result = dict(loops=loops, worst_position=worst_position, bent_normals=bent, worst_texture_uv=worst_uv, wrong_material=wrong_material)
    print("[lightmap] proxy check:", result, flush=True)
    if worst_position > 1e-3 or worst_uv > 1e-5 or wrong_material or bent > loops * 1e-3:
        raise RuntimeError(f"the proxy does not match the scene: {result}")
    return result


def stats(proxy, size):
    """Triangles, areas and texel densities: the bound size / sqrt(area) and the packed one."""
    mesh = proxy.data
    mesh.calc_loop_triangles()
    tris = len(mesh.loop_triangles)
    corners = _get(mesh.loop_triangles, 'loops', tris, np.int32, 3)
    co = _get(mesh.vertices, 'co', len(mesh.vertices), np.float64, 3)[_get(mesh.loops, 'vertex_index', len(mesh.loops), np.int32)][corners]
    uv = _get(mesh.uv_layers[LAYER].data, 'uv', len(mesh.loops), np.float64, 2)[corners]
    area = 0.5 * np.linalg.norm(np.cross(co[:, 1] - co[:, 0], co[:, 2] - co[:, 0]), axis=1).sum()
    e1, e2 = uv[:, 1] - uv[:, 0], uv[:, 2] - uv[:, 0]
    uv_area = 0.5 * np.abs(e1[:, 0] * e2[:, 1] - e1[:, 1] * e2[:, 0]).sum()
    result = dict(triangles=tris, area_m2=float(area), uv_fill=float(uv_area), uv_min=uv.min(axis=(0, 1)).tolist(), uv_max=uv.max(axis=(0, 1)).tolist(),
                  texels_per_meter=float(size / math.sqrt(area)), packed_texels_per_meter=float(size * math.sqrt(uv_area / area)))
    print("[lightmap] UV stats:", result, flush=True)
    if min(result['uv_min']) < 0.0 or max(result['uv_max']) > 1.0:
        raise RuntimeError("lightmap UVs leave [0, 1]")
    return result


def remove(proxy):
    mesh = proxy.data
    bpy.data.objects.remove(proxy, do_unlink=True)
    bpy.data.meshes.remove(mesh)
