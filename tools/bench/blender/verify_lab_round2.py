"""Verify round-two staging, protected geometry, routes, and standing clearances."""
import sys
import json
import math
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent))
import bpy
from lab_geometry import bounds, components, distance, overlaps
from character_motion import route
from character_export import append_characters, REPLACED

ASSETS=Path(__file__).resolve().parents[1]/'assets'
scene=bpy.context.scene
report={}
boxes=[]
for obj in list(scene.objects):
    if obj.type!='MESH' or obj.name in REPLACED or obj.name=='dress_cables': continue
    lo,hi=bounds(obj)
    if hi.z<=.04 or lo.z>=2: continue
    if obj.parent:
        boxes.append((obj.name,lo,hi))
    else:
        # Atlas batches combine distant boxes; test each component, not the empty
        # space enclosed by the aggregate bounding box of the entire hall.
        boxes.extend((obj.name,lo,hi) for _,lo,hi in components(obj) if hi.z>.04 and lo.z<2)
lanes=[]
for side in (-1,1):
    hits=[name for name,lo,hi in boxes if overlaps((lo,hi),((side*6.8-.8,-9),(side*6.8+.8,13)))]
    lanes.append(dict(x=side*6.8,collisions=sorted(set(hits))))
report['lanes']=lanes
hall=json.loads((ASSETS/'round2_hall_report.json').read_text())
bay=[bpy.data.objects[r['object']] for r in hall['bay_moves']]
pairs=[]
for i,a in enumerate(bay):
    alo,ahi=bounds(a)
    for b in bay[i+1:]:
        blo,bhi=bounds(b)
        if min(ahi.z,bhi.z)-max(alo.z,blo.z)>.001 and overlaps((alo,ahi),(blo,bhi),.399):
            pairs.append([a.name,b.name])
report['bay_spacing_failures']=pairs
rig_names=append_characters()
scene.frame_set(0)
clearances=[]
for name in rig_names:
    if name in ('hero_armature','olivia_armature','chad_armature'): continue
    rig=bpy.data.objects[name]
    xy=tuple(rig.location[:2])
    nearest=sorted((distance(xy,lo,hi)-.45,obj) for obj,lo,hi in boxes)
    clearances.append(dict(character=name,xy=xy,clearance=nearest[0][0],nearest=nearest[0][1]))
report['standing_clearances']=clearances
period=(scene.frame_end-scene.frame_start)/30
separation=min(math.dist(route(k/30,period,'hero')[0],route(k/30,period,'olivia')[0]) for k in range(scene.frame_end+1))
report['walker_minimum_separation']=separation
report['salsa_floor_lift']=bpy.data.objects['salsa_armature'].get('floor_lift')
report['salsa_floor_minimum']=bpy.data.objects['salsa_armature'].get('floor_minimum_before',0)+report['salsa_floor_lift']
(ASSETS/'round2_verification.json').write_text(json.dumps(report,indent=2))
print('[round2 verify]',json.dumps(report),flush=True)
assert not any(l['collisions'] for l in lanes), lanes
assert not pairs,pairs
assert all(c['clearance']>=0 for c in clearances),clearances
assert separation>=.7
assert report['salsa_floor_minimum']>=0
print('[round2 verify] PASSED',flush=True)
