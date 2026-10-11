"""The garden's props (build_lab_garden.py, garden v2.2/v2.3): benches, tables, rocks, a boulder, a dead trunk, branches,
pots, barrels and yard tools from the Poly Haven glTFs in tools/bench/assets/polyhaven/ (CC0, read only).

Each template is some of an asset's nodes joined into one mesh at the asset's axes (Blender: z up, the glTF's +z is
-y here, which is the template's front), centred on its footprint, standing on z = 0 (`min`: its lowest vertex;
`origin`: the asset's own ground, for the rocks that are modelled partly below it) and sunk by `sink`. Heavy parts
are decimated (collapse) to `budget` triangles for the template or `limits` per node. The modular bench's section is
its nodes (legs, seat with back, arm rests) moved together from the kit's exploded layout (garden_props_assembly.SEATING,
v2.3); the double bench adds that section mirrored about x = 0 (the shared double leg). garden_props_assembly.check
fails the build if a template has a part or group of parts off its base, and the template's lowest point must be its
base (`min`) or below it (`origin`).

Props are lit dynamically, like the foliage: their nodes are named `prop_*`, which bake_lab_lightmaps.py leaves out of
the lightmap UVs and hides in every bake (lab_lightmap_uv.is_unbaked); bench12's dynamic-only shadow maps draw every
triangle without lightmap UVs, so they still cast the sun's and the lamps' shadows. They are not `veg_` nodes, so the
wind leaves them alone.

One UV map per template (TEXCOORD_0 only; the seating's unused second set is removed). Textures: every image is
scaled to at most TEXTURE_SIZE and written as JPEG to <out>/garden_props/ (packed into the
.blend), the potted plant's leaves get an alpha from their diffuse map (black around each leaf; the JPEG has none),
cut out like the tree's leaves (garden_plants.cut_out: alphaMode MASK). The compost bags' transmission is dropped
(the bench reads no transmission).

Instances (v2.3): every prop is an object of its template's one mesh (location, turn about z, uniform scale), so the
glTF export writes one mesh per template and a node per prop (bench12 still flattens them, see RASTER_BAKED.md).
"""
import re
from pathlib import Path

import bpy
import numpy as np
from mathutils import Matrix

import garden_plants as plants
import garden_props_assembly as assembly
from garden_props_ground import local_vertices

TEXTURE_SIZE = 1024  # RGBA8 with mips in bench12: ~5.6 MB per map instead of ~22 MB at 2k (about 70 maps)
LOW = 0.7            # vertices below this height make the footprint that grass keeps away from
SECTION = ('legs_single', 'crossbar', 'suspended_support_01', 'back_support_r', 'back_support_l', 'arm_rest_01',
           'arm_rest_02', 'seat', 'seat_back', 'legs_double')
MIRRORED = ('legs_single', 'crossbar', 'suspended_support_01', 'back_support_r', 'back_support_l', 'arm_rest_01',
            'seat', 'seat_back')


def _rock(asset, number, sink):
    return dict(asset=asset, nodes=(f"{asset}_rock{number}",), budget=5000, ground='origin', sink=sink)


TEMPLATES = {
    'bench': dict(asset='modular_street_seating', nodes=SECTION, offsets=assembly.SEATING),
    'bench_double': dict(asset='modular_street_seating', nodes=SECTION, mirror=MIRRORED, offsets=assembly.SEATING),
    'picnic_table': dict(asset='wooden_picnic_table'),
    'table_set': dict(asset='outdoor_table_chair_set_01'),
    **{f"rock_s1_{n}": _rock('rock_moss_set_01', n, 0.0) for n in ('03', '04', '06')},  # modelled sunk already
    **{f"rock_s2_{n}": _rock('rock_moss_set_02', n, 0.12) for n in ('08', '09', '10', '11', '12')},
    'boulder': dict(asset='boulder_01', budget=20000, ground='origin', sink=0.15),
    'trunk': dict(asset='dead_tree_trunk', budget=20000, sink=0.06),
    **{f"branches_{k}": dict(asset='dry_branches_medium_01', nodes=(f"dry_branches_medium_01_{k}",), sink=0.01)
       for k in 'abc'},
    'pot': dict(asset='potted_plant_02', limits={'potted_plant_02_dirt': 2000}),
    'barrel_red': dict(asset='Barrel_01'),
    'barrel_blue': dict(asset='Barrel_02'),
    'jerrycan': dict(asset='metal_jerrycan_green'),
    'bucket': dict(asset='wooden_bucket_01'),
    'hand_truck': dict(asset='hand_truck'),
    'compost_bags': dict(asset='compost_bags'),
    'cement_bag': dict(asset='cement_bag'),
    'gloves': dict(asset='garden_gloves_01'),
    'crates': dict(asset='old_military_crate'),
}
CUT_OUT = {'potted_plant_02_leaves'}  # materials whose alpha is rebuilt from the diffuse map


def _triangles(mesh):
    return sum(len(p.vertices) - 2 for p in mesh.polygons)


def _decimated(mesh, target):
    """A copy of mesh decimated (collapse) to about target triangles."""
    holder = bpy.data.objects.new("decimate_" + mesh.name, mesh)
    bpy.context.scene.collection.objects.link(holder)
    modifier = holder.modifiers.new("decimate", 'DECIMATE')
    modifier.decimate_type, modifier.use_collapse_triangulate = 'COLLAPSE', True
    modifier.ratio = min(1.0, target / _triangles(mesh))
    depsgraph = bpy.context.evaluated_depsgraph_get()
    reduced = bpy.data.meshes.new_from_object(holder.evaluated_get(depsgraph))
    bpy.data.objects.remove(holder, do_unlink=True)
    return reduced


def _join(meshes, name):
    """One mesh of meshes (world space already), materials and UV maps merged by the join operator."""
    objects = [bpy.data.objects.new(f"{name}_part{k}", m) for k, m in enumerate(meshes)]
    for obj in objects:
        bpy.context.scene.collection.objects.link(obj)
    with bpy.context.temp_override(active_object=objects[0], object=objects[0], selected_objects=objects,
                                   selected_editable_objects=objects):
        bpy.ops.object.join()
    joined = objects[0]
    mesh = joined.data
    mesh.name = name
    bpy.data.objects.remove(joined, do_unlink=True)
    return mesh


def _parts(objects, spec):
    """[(node, world-space mesh copy)] of the template's nodes (moved by the spec's offsets, then mirrored copies),
    decimated as the spec asks, and the triangles before."""
    by_node = {re.sub(r"\.\d{3}$", "", o.name): o for o in objects if o.type == 'MESH'}
    nodes = spec.get('nodes') or tuple(by_node)
    copies = [(by_node[n], False) for n in nodes] + [(by_node[n], True) for n in spec.get('mirror', ())]
    meshes = []
    for obj, mirror in copies:
        mesh = obj.data.copy()
        offset = Matrix.Translation(spec.get('offsets', {}).get(re.sub(r"\.\d{3}$", "", obj.name), (0.0, 0.0, 0.0)))
        matrix = (Matrix.Scale(-1, 4, (1, 0, 0)) if mirror else Matrix()) @ offset @ obj.matrix_world
        mesh.transform(matrix)
        if mirror:
            mesh.flip_normals()
        meshes.append((re.sub(r"\.\d{3}$", "", obj.name), mesh))
    total = sum(_triangles(m) for _, m in meshes)
    ratio = min(1.0, spec['budget'] / total) if 'budget' in spec else 1.0
    result = []
    for node, mesh in meshes:
        target = spec.get('limits', {}).get(node, _triangles(mesh) * ratio)
        if target < _triangles(mesh):
            reduced = _decimated(mesh, target)
            bpy.data.meshes.remove(mesh)
            mesh = reduced
        result.append((node, mesh))
    return result, total


def _template(name, spec, objects):
    parts, before = _parts(objects, spec)
    groups = assembly.check(name, parts)
    mesh = _join([m for _, m in parts], "prop_template_" + name)
    for layer in list(mesh.uv_layers)[1:]:  # modular_street_seating carries an unused second set: no TEXCOORD_1
        mesh.uv_layers.remove(layer)
    co =np.array([v.co for v in mesh.vertices])
    lo, hi = co.min(axis=0), co.max(axis=0)
    ground = lo[2] if spec.get('ground', 'min') == 'min' else 0.0
    mesh.transform(Matrix.Translation((-(lo[0] + hi[0]) / 2, -(lo[1] + hi[1]) / 2, -ground - spec.get('sink', 0.0))))
    lowest = min(v.co.z for v in mesh.vertices)
    if lowest > -spec.get('sink', 0.0) + 1e-4 or (ground == lo[2] and lowest < -spec.get('sink', 0.0) - 1e-4):
        raise RuntimeError(f"prop template {name}: lowest point {lowest:.4f}, its base is {-spec.get('sink', 0.0):.4f}")
    triangles = _triangles(mesh)
    half = [float((hi[0] - lo[0]) / 2), float((hi[1] - lo[1]) / 2)]
    print(f"[garden] prop template {name}: {before} -> {triangles} triangles, footprint {2 * half[0]:.2f} x "
          f"{2 * half[1]:.2f} m, height {hi[2] - ground:.2f} m, {len(parts)} parts in {len(groups)} group(s) on its base",
          flush=True)
    return mesh, triangles, half


def _cut_out_leaves(material):
    """The leaves' alpha from their diffuse map (black around each leaf), at TEXTURE_SIZE."""
    diffuse = plants._image_array(plants._texture(material, 'diff'))
    while diffuse.shape[0] > TEXTURE_SIZE:  # 2x2 box filter
        h, w = diffuse.shape[:2]
        diffuse = diffuse.reshape(h // 2, 2, w // 2, 2, 4).mean(axis=(1, 3))
    alpha = np.clip((diffuse[..., :3].max(axis=2) - 0.03) / 0.05, 0.0, 1.0)
    plants.cut_out(material, plants._rgba_image(material.name + "_rgba", diffuse[..., :3], alpha))


def _drop_transmission(material):
    bsdf = next((n for n in material.node_tree.nodes if n.type == 'BSDF_PRINCIPLED'), None)
    socket = bsdf.inputs.get("Transmission Weight") if bsdf else None
    if socket is not None and (socket.is_linked or socket.default_value > 0):
        for link in list(socket.links):
            material.node_tree.links.remove(link)
        socket.default_value = 0.0
        print(f"[garden] {material.name}: transmission dropped", flush=True)


def _shrink_images(materials, folder):
    """Every image of the materials at most TEXTURE_SIZE, written as JPEG to folder, loaded again and packed in place of
    the importer's (which packs the 2k original). Returns the count."""
    folder.mkdir(parents=True, exist_ok=True)
    nodes = [n for m in materials for n in m.node_tree.nodes if n.type == 'TEX_IMAGE' and n.image]
    images = {n.image for n in nodes if not n.image.name.endswith("_rgba")}  # the leaves' RGBA: built at TEXTURE_SIZE
    for image in images:
        w, h = image.size
        if max(w, h) > TEXTURE_SIZE:
            image.scale(max(1, w * TEXTURE_SIZE // max(w, h)), max(1, h * TEXTURE_SIZE // max(w, h)))
        path = folder / (re.sub(r"[^A-Za-z0-9_]+", "_", Path(image.name).stem).replace("_2k", "") + ".jpg")
        image.filepath_raw, image.file_format = str(path), 'JPEG'
        try:
            image.save(filepath=str(path), quality=92)
        except TypeError:
            image.save()
        loaded = bpy.data.images.load(str(path), check_existing=False)
        loaded.colorspace_settings.name = image.colorspace_settings.name
        loaded.pack()
        for node in nodes:
            if node.image == image:
                node.image = loaded
        name = image.name
        bpy.data.images.remove(image)
        loaded.name = name
        if max(loaded.size) > TEXTURE_SIZE:
            raise RuntimeError(f"{name}: {loaded.size[:]} after scaling")
    return len(images)


def templates(polyhaven, out):
    """{name: (mesh, triangles, half extents)} of every template, the imported objects removed."""
    result, materials = {}, set()
    for asset in sorted({spec['asset'] for spec in TEMPLATES.values()}):
        objects = plants._import(polyhaven / asset / f"{asset}_2k.gltf")
        for obj in objects:
            if obj.type == 'MESH':
                materials.update(m for m in obj.data.materials if m)
        for material in [m for m in materials if re.sub(r"\.\d{3}$", "", m.name) in CUT_OUT]:
            if not material.get('garden_cut'):
                _cut_out_leaves(material)
                material['garden_cut'] = True
        for name, spec in TEMPLATES.items():
            if spec['asset'] == asset:
                result[name] = _template(name, spec, objects)
        plants._remove(objects)
    for material in materials:
        _drop_transmission(material)
        material.use_backface_culling = False
    count = _shrink_images(materials, out / "garden_props")
    print(f"[garden] prop templates: {len(result)} from {len({s['asset'] for s in TEMPLATES.values()})} assets, "
          f"{len(materials)} materials, {count} images at most {TEXTURE_SIZE}", flush=True)
    return result


def build(templates_, placed, collection):
    """prop_* objects for the placements (garden_props_layout.resolve, garden_props_scatter.scatter), each an instance
    of its template's mesh; templates no prop uses are removed. Returns (objects, triangles)."""
    objects = []
    for prop in placed:
        obj = bpy.data.objects.new(prop['name'], templates_[prop['template']][0])
        obj.location = (prop['x'], prop['y'], prop['z'])
        obj.rotation_euler = (0.0, 0.0, prop['turn'])
        obj.scale = (prop['scale'],) * 3
        collection.objects.link(obj)
        objects.append(obj)
    for mesh, _, _ in templates_.values():
        if not mesh.users:
            bpy.data.meshes.remove(mesh)
    triangles = sum(templates_[p['template']][1] for p in placed)
    print(f"[garden] props: {len(objects)} objects of {len({p['template'] for p in placed})} shared meshes, "
          f"{triangles} triangles", flush=True)
    return objects, triangles


def low_points(objects):
    """The props' vertices below LOW (world xy, one point per 15 cm cell): the footprints the grass keeps away from."""
    cache, low = {}, []
    for obj in objects:
        if obj.data.name not in cache:
            cache[obj.data.name] = local_vertices(obj.data)
        co = cache[obj.data.name]
        matrix = np.array(obj.matrix_basis)
        world = co @ matrix[:3, :3].T + matrix[:3, 3]
        points = world[world[:, 2] < LOW, :2]
        low.append(np.unique(np.round(points / 0.15), axis=0) * 0.15)
    return np.concatenate(low)
