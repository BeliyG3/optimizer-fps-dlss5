"""Append character scene in memory and validate the exported GLB contract."""
from collections import Counter
from pathlib import Path
import json
import struct
import bpy
import numpy as np

REPLACED = ('girl_complete_03', 'dummy_figure', 'npc_bench_left', 'npc_bench_right')


def append_characters():
    if Path(bpy.data.filepath).name != 'lab_scene.blend':
        return []
    path = Path(bpy.data.filepath).with_name('lab_characters.blend')
    if not path.exists():
        return []
    scene = bpy.context.scene
    with bpy.data.libraries.load(str(path), link=False) as (src, dst):
        dst.scenes = src.scenes
    source, = dst.scenes
    objects = list(source.objects)
    rigs = [o for o in objects if o.type == 'ARMATURE']
    if len(rigs) != 9:
        raise RuntimeError(f'Expected nine character rigs, got {len(rigs)}')
    for obj in objects:
        scene.collection.objects.link(obj)
    scene.frame_start, scene.frame_end = source.frame_start, source.frame_end
    scene.render.fps, scene.render.fps_base = source.render.fps, source.render.fps_base
    bpy.data.scenes.remove(source)
    for name in REPLACED:
        obj = bpy.data.objects.get(name)
        if obj:
            bpy.data.objects.remove(obj, do_unlink=True)
    scene.frame_set(scene.frame_start)
    print(f'[characters] appended {len(objects)} objects: {[o.name for o in objects]}', flush=True)
    print(f'[characters] armatures: {[o.name for o in rigs]}', flush=True)
    return [o.name for o in rigs]


def validate_glb(path, rig_names):
    raw = Path(path).read_bytes()
    magic, version, length = struct.unpack_from('<4sII', raw)
    assert magic == b'glTF' and version == 2 and length == len(raw)
    size, kind = struct.unpack_from('<II', raw, 12)
    assert kind == 0x4E4F534A
    doc = json.loads(raw[20:20+size])
    nodes, accessors = doc['nodes'], doc['accessors']
    animations = doc.get('animations', [])
    print(f'[verify] file {path}: {len(raw):,} bytes; animations {len(animations)}; skins {len(doc.get("skins", []))}', flush=True)
    if not rig_names:
        return
    
    assert len(animations) == 1, 'All characters must share animation 0'
    animation = animations[0]
    binary_offset = 20 + size + 8

    def values(accessor_index):
        accessor = accessors[accessor_index]
        view = doc['bufferViews'][accessor['bufferView']]
        assert accessor['componentType'] == 5126
        components = {'SCALAR': 1, 'VEC3': 3, 'VEC4': 4}[accessor['type']]
        offset = binary_offset + view.get('byteOffset', 0) + accessor.get('byteOffset', 0)
        return np.ndarray((accessor['count'], components), dtype='<f4', buffer=raw,
                          offset=offset, strides=(view.get('byteStride', components*4), 4))

    worst = 0
    seen = set()
    for channel in animation['channels']:
        target = channel['target']
        key = (target['node'], target['path'])
        assert key not in seen, f'Duplicate animation target: {key}'
        seen.add(key)
        sampler = animation['samplers'][channel['sampler']]
        times = values(sampler['input'])[:, 0]
        assert abs(times[0]) < 1e-6 and np.all(np.diff(times) > 0)
        data = values(sampler['output'])
        error = float(np.max(np.abs(data[0]-data[-1])))
        if target['path'] == 'rotation':
            error = min(error, float(np.max(np.abs(data[0]+data[-1]))))
        worst = max(worst, error)
        assert error < 0.001, f'Loop discontinuity: {nodes[target["node"]].get("name")} {error}'
    print(f'[verify] maximum animation endpoint error: {worst:.8f}', flush=True)
    counts = Counter(c['target']['node'] for c in animation['channels'])
    duration = max(accessors[s['input']]['max'][0] for s in animation['samplers'])
    expected = (bpy.context.scene.frame_end-bpy.context.scene.frame_start)/bpy.context.scene.render.fps
    assert abs(duration-expected) < 0.001, (duration, expected)
    print(f'[verify] animation 0 duration {duration:.6f}s; channels {len(animation["channels"])}', flush=True)
    for index, count in sorted(counts.items()):
        print(f'[channel] {index} {nodes[index].get("name", "")}: {count}')
    for name in rig_names:
        root, = [i for i, n in enumerate(nodes) if n.get('name') == name]
        descendants, pending = set(), [root]
        while pending:
            i = pending.pop()
            descendants.add(i)
            pending.extend(nodes[i].get('children', []))
        skin_nodes = [nodes[i] for i in descendants if 'skin' in nodes[i]]
        assert skin_nodes, f'{name}: no skinned mesh'
        joint_ids = {j for n in skin_nodes for j in doc['skins'][n['skin']]['joints']}
        assert joint_ids & counts.keys(), f'{name}: no animated joints'
        total = sum(counts[i] for i in descendants)
        triangles = 0
        for i in descendants:
            if 'mesh' not in nodes[i]:
                continue
            for p in doc['meshes'][nodes[i]['mesh']]['primitives']:
                assert 'JOINTS_0' in p['attributes'] and 'WEIGHTS_0' in p['attributes']
                triangles += accessors[p['indices']]['count']//3
        assert triangles <= 120000
        if name in ('hero_armature', 'olivia_armature', 'chad_armature'):
            paths = {c['target']['path'] for c in animation['channels'] if c['target']['node'] == root}
            assert {'translation', 'rotation'} <= paths
        print(f'[verify] {name}: {total} channels ({counts[root]} root), {len(joint_ids)} joints, {triangles} triangles', flush=True)
    assert all(n.get('name') not in REPLACED for n in nodes)
    assert any(n.get('name') == 'CharacterAnchor' for n in nodes)
    assert all(s.get('interpolation', 'LINEAR') in ('LINEAR', 'STEP') for s in animation['samplers'])

