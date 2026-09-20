"""Back up once, repair lab staging, validate, and save from background Blender."""
import sys
import json
import shutil
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent))
import bpy
import bmesh
from lab_geometry import bounds, center, components, delete_boxes, separate_asset
from lab_staging import place_tabletop, stage_bays

ASSETS=Path(__file__).resolve().parents[1]/'assets'


def cables():
    obj=bpy.data.objects['dress_cables']
    curve=bpy.data.curves.new('round2_floor_hoses','CURVE')
    curve.dimensions='3D'
    curve.resolution_u=16
    curve.bevel_depth=.0175
    curve.bevel_resolution=4  # Blender generates 4 + 2*resolution = 12 radial vertices.
    curve.use_fill_caps=True
    # One crossing of both centre walking lines, near y=6; other runs stay outside.
    paths=[[(1.35,5.3),(1.1,5.6),(.85,5.9),(.1,6),(-.75,6.05),(-1.1,6.3),(-1.35,6.7)],
           [(3.5,8),(4,8.3),(4.5,8.1),(5,8.45),(5.45,8.15),(5.85,8.5),(6.2,8.7),(6.35,9.2)],
           [(-3.5,10),(-4,10.3),(-4.5,10.1),(-5,10.5),(-5.4,10.3),(-5.85,10.7),(-6.2,11),(-6.35,11.4)]]
    for index,side in ((1,1),(2,-1)):
        cart=next(o for o in bpy.context.scene.objects if o.type=='EMPTY' and o.name.startswith('prop_tool_cart') and center(o).x*side>0)
        c=center(cart)
        paths[index]=[(side*3.5,-4.2),(side*4.2,-4.7),(side*5.5,-5.2),(side*5.6,-11.8),(side*6.0,-12.7),(side*7.5,-12.7),(side*8,-11.8),(c.x,c.y)]
    for points in paths:
        spline=curve.splines.new('BEZIER')
        spline.bezier_points.add(len(points)-1)
        for p,(x,y) in zip(spline.bezier_points,points):
            p.co=(x,y,.0175)
            p.handle_left_type=p.handle_right_type='AUTO'
    for material in obj.data.materials: curve.materials.append(material)
    temp=bpy.data.objects.new('round2_hose_conversion',curve)
    bpy.context.scene.collection.objects.link(temp)
    bpy.ops.object.select_all(action='DESELECT')
    temp.select_set(True)
    bpy.context.view_layer.objects.active=temp
    bpy.ops.object.convert(target='MESH')
    obj.data=temp.data
    bm=bmesh.new()
    bm.from_mesh(obj.data)
    bmesh.ops.remove_doubles(bm,verts=list(bm.verts),dist=1e-6)
    bm.to_mesh(obj.data)
    bm.free()
    for polygon in obj.data.polygons: polygon.use_smooth=True
    bpy.data.objects.remove(temp,do_unlink=True)
    return dict(curves=3,control_points=[len(p) for p in paths],diameter=.035,radial_sides=12)


def main():
    path=ASSETS/'lab_scene.blend'
    backup=ASSETS/'lab_scene.before_round2.blend'
    if not backup.exists():
        # Exclusive creation prevents accidental overwrite of an existing backup.
        with path.open('rb') as source, backup.open('xb') as target:
            shutil.copyfileobj(source,target)
    if Path(bpy.data.filepath)!=path:
        bpy.ops.wm.open_mainfile(filepath=str(path))
    if '--rebuild' in sys.argv:
        bpy.ops.wm.open_mainfile(filepath=str(backup))
    scene=bpy.context.scene
    if scene.get('lab_round2_complete'):
        (ASSETS/'lab_gaps.json').write_text(scene['lab_round2_gaps'])
        print('[round2] Already complete; no scene changes or save.',flush=True)
        return
    report={'removed_desks':[],'removed_boxes':{},'split_assets':[],'tabletop_moves':[]}
    roots=lambda:[o for o in scene.objects if o.type=='EMPTY' and o.name.startswith('prop_')]
    for root in list(roots()):
        if root.name.startswith(('prop_SchoolChair','prop_WetFloorSign')):
            groups=[[m] for m in root.children_recursive if m.type=='MESH']
        elif root.name=='prop_classic_laptop':
            meshes=[m for m in root.children_recursive if m.type=='MESH']
            groups=[[m for m in meshes if (center(m)-center(base)).length<.5] for base in meshes if m_base(base)]
        else: continue
        old=root.name
        items=separate_asset(root,groups)
        report['split_assets'].append(dict(source=old,items=[o.name for o in items]))
    desks=[o for o in roots() if o.name.startswith('prop_metal_office_desk')]
    gaps={}
    for side,index,sign in [('right',1,1),('left',2,-1)]:
        desk=sorted([d for d in desks if center(d).x*sign>0],key=lambda d:center(d).y)[index]
        c=center(desk)
        gaps[side]=[c.x,c.y]
        report['removed_desks'].append(dict(name=desk.name,center=list(c)))
        desks.remove(desk)
        for obj in [*desk.children_recursive,desk]: bpy.data.objects.remove(obj,do_unlink=True)
    for name in ('lamp_screens_left','lamp_screens_right','screen_stands'):
        report['removed_boxes'][name]=delete_boxes(bpy.data.objects[name],list(gaps.values()))
    bpy.context.view_layer.update()
    # Move all portable bay objects; overhead lights and wall pipe installations stay.
    bay=[]
    for obj in roots():
        if obj in desks: continue
        lo,hi=bounds(obj)
        c=(lo+hi)/2
        if (abs(c.x)>4.4 and lo.z<1 and hi.z<3 and 'modular_industrial_pipes' not in obj.name) or obj.name.startswith('prop_WetFloorSign'):
            bay.append(obj)
        elif obj.name.startswith('prop_modular_industrial_pipes') and abs(c.x)<9:
            bay.append(obj)
    obstacles=[]
    for obj in scene.objects:
        if obj.type=='MESH' and not obj.parent and obj.name.startswith(('booth_','shell_','dress_')) and obj.name!='dress_cables':
            for _,lo,hi in components(obj):
                if hi.z>.04 and lo.z<2: obstacles.append((lo,hi))
    report['bay_moves']=stage_bays(bay,obstacles)
    candidates=[]
    occupied=[]
    for obj in roots():
        if obj in desks or obj in bay: continue
        lo,hi=bounds(obj)
        if hi.z>2 or abs(center(obj).x)>4.4: continue
        on_desk=abs(lo.z-.91)<.03 and any(all(a[i]<=lo[i] and hi[i]<=b[i] for i in (0,1)) for a,b in map(bounds,desks))
        # Laptops are desk equipment even if previously placed on the floor.
        if not on_desk and (lo.z>=.03 or 'laptop' in obj.name):
            candidates.append(obj)
        elif on_desk: occupied.append((lo,hi))
    for obj in sorted(candidates,key=lambda o:(bounds(o)[1]-bounds(o)[0]).x,reverse=True):
        if 'chemistry' in obj.name:
            obj.rotation_euler.z=0
            bpy.context.view_layer.update()
        report['tabletop_moves'].append(place_tabletop(obj,desks,occupied))
    report['cables']=cables()
    scene['lab_round2_complete']=True
    scene['lab_round2_gaps']=json.dumps(gaps)
    (ASSETS/'lab_gaps.json').write_text(json.dumps(gaps,indent=2))
    (ASSETS/'round2_hall_report.json').write_text(json.dumps(report,indent=2))
    bpy.context.preferences.filepaths.save_version=0
    bpy.ops.wm.save_as_mainfile(filepath=str(path))
    print('[round2] Gaps:',json.dumps(gaps),flush=True)
    print('[round2] Changes:',json.dumps(report),flush=True)


def m_base(obj):
    return obj.name=='classic_laptop' or obj.name.startswith('classic_laptop.')


if __name__=='__main__': main()



