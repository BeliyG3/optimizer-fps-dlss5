"""The garden's people (build_lab_garden.py, garden v2.4): copies of the lab's characters with their clips, saved in
lab_scene_garden.blend under the empty ROOT (it sorts after zz_garden, so the lab's and the garden's glTF nodes keep
their order).

Each person (garden_people_layout.resolve) is a copy of a rig of lab_characters.blend (SOURCES, loaded and dropped
again) named <person>_armature, with copies of its mesh objects named <person>_mesh_<k> that share the source's mesh
data (and so materials and images). Its animation is the source's shared loop (character_motion.bake: all rigs loop
over the same PERIOD_FRAMES at 30 fps):
- the bones' channels shifted by `pose_offset` frames (the loop is periodic, so the shift is seamless);
- the root: walkers follow garden_people_layout.track (the source's cadence, so the steps match the speed as in the
  hall), the others stand at their spot; the heading turns the source's start facing (measured from its shoulders,
  as character_motion.sample_clip does) to the person's, and z is the source's (the salsa's lift) plus the ground's
  height under the person (raycast onto the yard, soil and path);
- sitters: a looping seated idle (garden_people_sit_motion.py, v2.5): a Mixamo sitting clip when one is in the clips
  folder, else a procedural idle on the static seated pose (garden_people_seat.py), checked for contact over the loop.
The people are skinned meshes, so bench12 animates them (dynamic, with motion vectors) and the bake leaves them out
(lab_lightmap_uv.is_dynamic: an armature modifier), like the lab's characters. glb_share_skins.py makes the copies of
one character share one glTF mesh in the export.
"""
import math
from pathlib import Path

import bpy
import numpy as np
from mathutils import Quaternion

import garden_people_contact as contact
import garden_people_layout as people_layout
import garden_people_sit_motion as sit_motion
from garden_props_ground import surface

ROOT = "zz_people"
SOURCES = ('olivia', 'chad', 'olivia_talking', 'chad_talking', 'hiphop', 'salsa')


def load_sources():
    """{source: (rig, [mesh objects])} loaded from lab_characters.blend (not linked to the scene). Their loops must
    have a key on every frame of PERIOD_FRAMES (curves checks)."""
    characters = Path(bpy.data.filepath).with_name('lab_characters.blend')  # next to the scene, as character_export
    kinds = (bpy.data.meshes, bpy.data.materials, bpy.data.images, bpy.data.armatures, bpy.data.actions)
    before = [set(kind) for kind in kinds]
    with bpy.data.libraries.load(str(characters), link=False) as (src, dst):
        dst.objects = [n for n in src.objects if n.rsplit('_armature', 1)[0] in SOURCES
                       or n.split('_mesh_')[0] in SOURCES and '_mesh_' in n]
    # Their data is saved with the garden under other names, so the characters character_export appends at export
    # keep their own names (materials, meshes, images, as in lab_scene_outdoor.glb); use_lab_meshes then swaps.
    for kind, old in zip(kinds, before):
        for block in set(kind) - old:
            block.name = 'people_src_' + block.name
    objects = {o.name: o for o in dst.objects}
    return {s: (objects[f"{s}_armature"], sorted((o for n, o in objects.items() if n.startswith(f"{s}_mesh_")),
                                                 key=lambda o: o.name)) for s in SOURCES}


def curves(rig):
    """{data_path: values (frames x components)} of the rig's baked loop (a key on every frame 0..PERIOD_FRAMES)."""
    data = rig.animation_data
    bag = data.action.layers[0].strips[0].channelbag(data.action_slot)
    rows = {}
    for curve in bag.fcurves:
        co = np.empty(len(curve.keyframe_points) * 2)
        curve.keyframe_points.foreach_get('co', co)
        co = co.reshape(-1, 2)
        if len(co) != people_layout.PERIOD_FRAMES + 1 or np.abs(co[:, 0] - np.arange(len(co))).max() > 1e-4:
            raise RuntimeError(f"{rig.name} {curve.data_path}: not a key on every frame of the period")
        rows.setdefault(curve.data_path, {})[curve.array_index] = co[:, 1]
    return {path: np.column_stack([d[i] for i in sorted(d)]) for path, d in rows.items()}


def shifted(values, offset, quaternion):
    """The loop's values `offset` frames later (the last frame repeats the first); quaternions kept in one hemisphere."""
    body = np.roll(values[:-1], -offset, axis=0)
    out = np.vstack([body, body[:1]])
    if quaternion:
        for i in range(1, len(out)):
            if np.dot(out[i], out[i - 1]) < 0:
                out[i] = -out[i]
    return out


def write(rig, rows):
    """Replaces the rig's animation with one key per frame and row (character_motion.write_curves, LINEAR keys)."""
    frames = np.arange(people_layout.PERIOD_FRAMES + 1, dtype=float)
    rig.animation_data_clear()
    rig.keyframe_insert('location', frame=1)
    action = rig.animation_data.action
    action.name = rig.name + '_loop'
    bag = action.layers[0].strips[0].channelbag(rig.animation_data.action_slot)
    for curve in list(bag.fcurves):
        bag.fcurves.remove(curve)
    for path, values in rows:
        for index in range(values.shape[1]):
            curve = bag.fcurves.new(path, index=index)
            curve.keyframe_points.add(len(frames))
            curve.keyframe_points.foreach_set('co', np.column_stack((frames, values[:, index])).ravel())
            curve.keyframe_points.foreach_set('interpolation', [1] * len(frames))  # LINEAR
            curve.update()


def _copy(source, name, collection, root):
    rig, meshes = source
    copy = rig.copy()
    copy.data = rig.data.copy()
    copy.name = f"{name}_armature"
    collection.objects.link(copy)
    copy.parent = root
    children = []
    for mesh in meshes:
        child = mesh.copy()  # shares the mesh data
        child.name = f"{name}_mesh_{mesh.name.rsplit('_', 1)[1]}"
        collection.objects.link(child)
        child.parent = copy
        child.matrix_parent_inverse = mesh.matrix_parent_inverse.copy()
        for modifier in child.modifiers:
            if modifier.type == 'ARMATURE':
                modifier.object = copy
        children.append(child)
    return copy, children


LAB_MESHES = {'olivia': 'olivia_mesh_{}', 'chad': 'chad_mesh_{}', 'xbot': 'hiphop_mesh_{}'}  # glb_share_skins.CANONICAL


def use_lab_meshes():
    """At export, after character_export.append_characters: every person's mesh object takes the mesh data of its lab
    character (LAB_MESHES; the same geometry, groups and weights as the copy saved in the .blend), so the export
    uses the lab's materials and images instead of duplicates. Returns the number of objects changed."""
    changed = 0
    for obj in bpy.data.objects:
        if obj.type == 'MESH' and obj.name.startswith('people_') and '_mesh_' in obj.name:
            lab = bpy.data.objects.get(LAB_MESHES[obj.name.split('_')[1]].format(obj.name.rsplit('_', 1)[1]))
            if lab is None:
                raise RuntimeError(f"{obj.name}: no lab character mesh to use")
            if len(lab.data.vertices) != len(obj.data.vertices):
                raise RuntimeError(f"{obj.name}: {lab.name} has another vertex count")
            obj.data = lab.data
            changed += 1
    return changed


def node_names(objects):
    """The glTF node names the people's objects export as: every object, and an armature's bones (joint nodes)."""
    names = []
    for obj in objects:
        names += [obj.name] + ([b.name for b in obj.data.bones] if obj.type == 'ARMATURE' else [])
    return names


def steps(rig, person, every=5):
    """How far (m) the dancer's feet and hands get from its spot over the loop (every few frames); raises if past its
    footprint (garden_people_layout.DANCE_RADIUS, which the checks keep clear) less a body margin."""
    scene, far = bpy.context.scene, 0.0
    ends = [b for b in rig.pose.bones if b.name.endswith(('Foot', 'ToeBase', 'Hand'))]
    for frame in range(0, people_layout.PERIOD_FRAMES + 1, every):
        scene.frame_set(frame)
        for bone in ends:
            p = rig.matrix_world @ bone.head
            far = max(far, math.hypot(p.x - person['x'], p.y - person['y']))
    if far > people_layout.DANCE_RADIUS[person['source']] - 0.1:
        raise RuntimeError(f"{person['name']} reaches {far:.2f} m from its spot (footprint {people_layout.DANCE_RADIUS})")
    return round(far, 3)


def build(people, collection, surfaces, bench_objects, clips=None):
    """Creates every person under ROOT. Returns (objects, report): report per person (kind, ground, seat contact and
    loop). `clips`: the folder of Mixamo sitting clips (garden_people_mixamo.find), None for the procedural idle only."""
    scene = bpy.context.scene
    scene.frame_start, scene.frame_end = 0, people_layout.PERIOD_FRAMES
    sources = load_sources()
    loops = {s: curves(rig) for s, (rig, _) in sources.items()}
    root = bpy.data.objects.new(ROOT, None)
    collection.objects.link(root)
    at = surface(surfaces)
    frames = np.arange(people_layout.PERIOD_FRAMES + 1)
    objects, report, joints = [root], [], {}
    for person in people:
        rig, children = _copy(sources[person['source']], person['name'], collection, root)
        objects += [rig] + children
        loop = loops[person['source']]
        scene.frame_set(0)
        bpy.context.view_layer.update()
        base = Quaternion((0, 0, 1), -contact.facing(rig)) @ Quaternion(loop['rotation_quaternion'][0])
        entry = dict(name=person['name'], role=person['role'], source=person['source'], armature=rig.name,
                     meshes=[c.name for c in children])
        if person['role'] == 'sit':
            rows, seated = sit_motion.animate(rig, children, person, base, bench_objects[person['bench']], at, clips)
            write(rig, rows)
            joints[person['name']] = seated['loop']['contact'].pop('joints')
            entry.update(seated)
            report.append(entry)
            continue
        offset = person['pose_offset']
        rows = [(p, shifted(v, offset, p.endswith('rotation_quaternion'))) for p, v in loop.items() if p.startswith('pose.')]
        lift = shifted(loop['location'], offset, False)[:, 2]
        if person['role'] == 'walk':
            xy, heading = people_layout.track(person['line'], person['legs'], person['route_offset'], frames)
        else:
            xy = np.tile([person['x'], person['y']], (len(frames), 1))
            heading = np.full(len(frames), person['heading'])
        floor = np.array([at(x, y) for x, y in xy])
        rotations = [Quaternion((0, 0, 1), h) @ base for h in heading]
        for i in range(1, len(rotations)):
            if rotations[i].dot(rotations[i - 1]) < 0:
                rotations[i].negate()
        rows += [('location', np.column_stack([xy, lift + floor])), ('rotation_quaternion', np.array([tuple(q) for q in rotations]))]
        write(rig, rows)
        entry.update(ground=[round(float(floor.min()), 4), round(float(floor.max()), 4)], pose_offset=offset)
        if person['role'] == 'walk':
            entry.update(length=round(person['length'], 2), legs=person['legs'], route_offset=person['route_offset'],
                         speed=round(person['length'] / (people_layout.PERIOD / (2 * person['legs']) - people_layout.TURN), 3))
        if person['role'] == 'dance':
            entry['steps'] = steps(rig, person)
        report.append(entry)
        print(f"[people] {person['name']}: {person['role']} copy of {person['source']}, ground z "
              f"{floor.min():.3f}..{floor.max():.3f}", flush=True)
    clearance = people_layout.sitter_clearance(people, joints)  # v2.5b: walkers off the sitters' knees and feet
    for entry in report:
        entry.update(clearance.get(entry['name'], {}))
    for rig, meshes in sources.values():
        for obj in meshes + [rig]:
            bpy.data.objects.remove(obj, do_unlink=True)
    scene.frame_set(0)
    return objects, report
