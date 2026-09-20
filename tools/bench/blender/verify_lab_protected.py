"""Record immutable hall object fingerprints for before/after comparison."""
import hashlib
import json
from pathlib import Path
import bpy
import numpy as np

prefixes=('booth_','shell_','lamp_','dress_','sign_','ring_','screens_big')
allowed={'lamp_screens_left','lamp_screens_right','dress_cables'}
result={}
for obj in bpy.context.scene.objects:
    if not obj.name.startswith(prefixes) or obj.name in allowed: continue
    digest=hashlib.sha256()
    digest.update(np.array(obj.matrix_world,dtype='<f8').tobytes())
    if obj.type=='MESH':
        coords=np.empty(len(obj.data.vertices)*3,dtype=np.float32)
        obj.data.vertices.foreach_get('co',coords)
        digest.update(coords.tobytes())
        digest.update(str([tuple(p.vertices) for p in obj.data.polygons]).encode())
        digest.update(str([m.name if m else None for m in obj.data.materials]).encode())
        for layer in obj.data.uv_layers:
            uv=np.empty(len(layer.data)*2,dtype=np.float32)
            layer.data.foreach_get('uv',uv)
            digest.update(uv.tobytes())
    result[obj.name]=digest.hexdigest()
assets=Path(__file__).resolve().parents[1]/'assets'
if Path(bpy.data.filepath).name=='lab_scene.before_round2.blend':
    (assets/'round2_protected_before.json').write_text(json.dumps(result,indent=2))
else:
    before=json.loads((assets/'round2_protected_before.json').read_text())
    assert before==result, [name for name in before if before[name]!=result.get(name)]
    print(f'[protected] {len(result)} objects: transforms, vertices, topology, UVs, materials unchanged',flush=True)
