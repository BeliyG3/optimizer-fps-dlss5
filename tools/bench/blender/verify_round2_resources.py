"""Inspect round-two cable topology and exported resource sharing."""
import sys
import json
import struct
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent))
import bpy
from lab_geometry import components
assets=Path(__file__).resolve().parents[1]/'assets'
obj=bpy.data.objects['dress_cables']
parts=list(components(obj))
assert len(parts)==3
for ids,lo,hi in parts:
    assert lo.z>=-1e-6 and abs(hi.z-.035)<1e-6
    assert len(ids)%12==0
# Check every cable vertex outside the sole authorized central crossing.
for v in obj.data.vertices:
    p=obj.matrix_world@v.co
    if -9<=p.y<=13:
        assert min(abs(p.x-6.8),abs(p.x+6.8))>=.3, tuple(p)
    if -6.1<=p.y<=13 and not 5.5<=p.y<=6.5:
        assert min(abs(p.x-.35),abs(p.x+.45))>=.3, tuple(p)
print('[cables] Three smooth 12-sided components; floor bounds 0..0.035m; routes clear except y~6 crossing',flush=True)
raw=(assets/'lab_scene_chars_test.glb').read_bytes()
size=struct.unpack_from('<I',raw,12)[0]
doc=json.loads(raw[20:20+size])
nodes={n.get('name'):n for n in doc['nodes']}
sharing={}
for prefix in ('olivia','chad'):
    rows=[]
    for name,target in nodes.items():
        if name and name.startswith(prefix+'_talking_mesh_'):
            source=nodes[name.replace('_talking','')]
            a,b=doc['meshes'][source['mesh']],doc['meshes'][target['mesh']]
            rows.append(dict(name=name,same_mesh=source['mesh']==target['mesh'],same_materials=[p.get('material') for p in a['primitives']]==[p.get('material') for p in b['primitives']]))
    sharing[prefix]=rows
(assets/'round2_resource_sharing.json').write_text(json.dumps(sharing,indent=2))
print('[resources]',json.dumps(sharing),flush=True)


