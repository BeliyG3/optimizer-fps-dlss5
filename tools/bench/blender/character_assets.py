"""Import self-contained Mixamo rigs and restore the lab heroine's materials."""
from pathlib import Path
import bpy

ASSETS = Path(__file__).resolve().parents[1] / 'assets'
CHARACTERS = [
    ('hero', 'heroine__female_walk', 1.75),
    ('olivia', 'olivia__female_walk', 1.70),
    ('chad', 'chad__walking', 1.80),
    ('hiphop', 'xbot__hip_hop_dancing', 1.72),
    ('salsa', 'xbot__salsa_dancing', 1.72),
    ('mannequin', 'mannequin__talking', 1.82),
    ('dummy', 'dummy__talking', 1.80),
    ('olivia_talking', 'olivia__talking', 1.70),
    ('chad_talking', 'chad__talking', 1.80),
]


def triangles(mesh):
    mesh.data.calc_loop_triangles()
    return len(mesh.data.loop_triangles)


def heroine_materials():
    with bpy.data.libraries.load(str(ASSETS / 'lab_scene.blend'), link=False) as (src, dst):
        dst.objects = ['girl_complete_03']
    source = dst.objects[0]
    materials = list(source.data.materials)
    bpy.data.objects.remove(source, do_unlink=True)
    return materials


def import_character(name, clip, height, materials):
    before = set(bpy.data.objects)
    bpy.ops.import_scene.fbx(filepath=str(ASSETS / 'mixamo/out' / (clip + '.fbx')))
    objects = list(set(bpy.data.objects) - before)
    rig, = [o for o in objects if o.type == 'ARMATURE']
    meshes = sorted((o for o in objects if o.type == 'MESH'), key=lambda o: o.name)
    rig.name = name + '_armature'
    rig.data.pose_position = 'REST'
    bpy.context.view_layer.update()
    points = [m.matrix_world @ v.co for m in meshes for v in m.data.vertices]
    lo, hi = min(p.z for p in points), max(p.z for p in points)
    factor = height / (hi - lo)
    rig.scale *= factor
    bpy.context.view_layer.update()
    base = rig.matrix_world.copy()
    base.translation.z -= lo * factor
    rig.matrix_world = base
    for i, mesh in enumerate(meshes):
        mesh.name = 'hero_girl' if name == 'hero' and i == 0 else f'{name}_mesh_{i}'
        if name == 'hero':
            by_name = {m.name: m for m in materials if m}
            for slot, old in enumerate(list(mesh.data.materials)):
                key = old.name.rsplit('.', 1)[0] if old and old.name[-3:].isdigit() else old.name if old else ''
                mat = by_name.get(key)
                if mat is None:
                    print(f'[materials] FALLBACK slot {slot}: {key}', flush=True)
                    mat = materials[slot] if slot < len(materials) else None
                if mat is None:
                    raise RuntimeError(f'Unmapped heroine material: {key}')
                mesh.data.materials[slot] = mat
                print(f'[materials] {slot}: {key} -> {mat.name}', flush=True)
    count = sum(triangles(m) for m in meshes)
    if count > 120000:
        for mesh in meshes:
            bpy.context.view_layer.objects.active = mesh
            mod = mesh.modifiers.new('Character budget', 'DECIMATE')
            mod.ratio = 80000 / count
            bpy.ops.object.modifier_apply(modifier=mod.name)
    print(f'[character] {name}: bind height {hi-lo:.4f} -> {height}; triangles {count} -> {sum(triangles(m) for m in meshes)}', flush=True)
    rig.data.pose_position = 'POSE'
    return rig


def pack_images():
    for image in bpy.data.images:
        if image.type != 'IMAGE' or not image.size[0]:
            continue
        w, h = image.size
        if max(w, h) > 2048:
            scale = 2048 / max(w, h)
            image.scale(round(w * scale), round(h * scale))
            print(f'[image] resized {image.name}: {w}x{h} -> {tuple(image.size)}', flush=True)
        image.pack()

