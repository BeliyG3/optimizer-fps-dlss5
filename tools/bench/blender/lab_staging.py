"""Pack movable furniture along bay walls and desk props into free tabletop space."""
import math
import bpy
from mathutils import Vector
from lab_geometry import bounds, center, move_bottom, overlaps


def place_tabletop(obj, desks, occupied):
    lo, hi = bounds(obj)
    size = hi-lo
    origin = center(obj)
    for desk in sorted(desks, key=lambda d:(center(d)-origin).length):
        a,b = bounds(desk)
        # Monitor boxes occupy a 0.08m strip centered 0.34m behind desk center.
        back_limit = center(desk).y + 0.30 - 0.25
        xmin, xmax = a.x+.25+size.x/2, b.x-.25-size.x/2
        ymin, ymax = a.y+.25+size.y/2, min(b.y-.25,back_limit)-size.y/2
        if xmin > xmax or ymin > ymax:
            continue
        for yi in range(9):
            y = ymin+(ymax-ymin)*yi/8
            for xi in range(25):
                x = xmin+(xmax-xmin)*xi/24
                box = (Vector((x-size.x/2,y-size.y/2,.91)),Vector((x+size.x/2,y+size.y/2,.91+size.z)))
                if any(overlaps(box, other, .25) for other in occupied):
                    continue
                before = list(origin)
                move_bottom(obj,x,y,.91)
                occupied.append(bounds(obj))
                return dict(object=obj.name, before=before, after=list(center(obj)), desk=desk.name)
    raise RuntimeError(f'No free tabletop for {obj.name}: {tuple(size)}')


def stage_bays(objects, obstacles):
    # Independent asset instances allow real bounds, without scaling furniture.
    for obj in objects:
        obj.rotation_euler.z = math.pi/2 if center(obj).x<0 else -math.pi/2
        bpy.context.view_layer.update()
        lo,hi=bounds(obj)
        if hi.x-lo.x > 1.68:
            best = None
            for step in range(72):
                obj.rotation_euler.z = step*math.pi/36
                bpy.context.view_layer.update()
                a,b = bounds(obj)
                if best is None or b.x-a.x < best[0]: best=(b.x-a.x,obj.rotation_euler.z)
            obj.rotation_euler.z = best[1]
            bpy.context.view_layer.update()
        assert bounds(obj)[1].x-bounds(obj)[0].x <= 1.73, obj.name
    crates=sorted([o for o in objects if 'crate' in o.name],key=lambda o:bounds(o)[1].x-bounds(o)[0].x,reverse=True)
    others=[o for o in objects if o not in crates]
    units=[[o] for o in others]
    while crates:
        base=crates.pop(0)
        blo,bhi=bounds(base)
        partner=next((o for o in crates if all(bounds(o)[1][i]-bounds(o)[0][i]<=bhi[i]-blo[i]+.001 for i in (0,1))),None)
        unit=[base]
        if partner:
            crates.remove(partner)
            unit.append(partner)
        units.append(unit)
    # Largest units first, then fill holes. Both bays are available for redistribution.
    units.sort(key=lambda u:(0 if any(k in u[0].name for k in ('rack','drawer_cabinet')) else 1, -(bounds(u[0])[1].y-bounds(u[0])[0].y)))
    placed=[]
    changes=[]
    for unit in units:
        obj=unit[0]
        lo,hi=bounds(obj)
        width,length=hi.x-lo.x,hi.y-lo.y
        preferred=-1 if center(obj).x<0 else 1
        found=False
        for side in (preferred,-preferred):
            x=side*(9.35-width/2)
            for step in range(561*12):
                if any(k in obj.name for k in ('rack','drawer_cabinet')) and step%12: continue
                y=-12+length/2+(step//12)*.05
                x=side*(9.35-width/2-(step%12)*(1.73-width)/11)
                if y+length/2>15: break
                box=(Vector((x-width/2,y-length/2,0)),Vector((x+width/2,y+length/2,hi.z-lo.z)))
                if any(overlaps(box,b,.4) for b in placed): continue
                if any(overlaps(box,b,.05) for b in obstacles): continue
                z=0
                for member in unit:
                    before=list(center(member))
                    # Positive local Y faces the aisle at either wall.
                    if side != preferred:
                        member.rotation_euler.z += math.pi
                        bpy.context.view_layer.update()
                    move_bottom(member,x,y,z)
                    z=bounds(member)[1].z
                    changes.append(dict(object=member.name,before=before,after=list(center(member)),stack_level=unit.index(member)+1))
                placed.append(box)
                found=True
                break
            if found: break
        if not found: raise RuntimeError(f'No wall space for {obj.name}')
    for obj in objects:
        lo,hi=bounds(obj)
        assert hi.x <= -7.6 or lo.x >= 7.6, (obj.name,lo,hi)
    return changes



