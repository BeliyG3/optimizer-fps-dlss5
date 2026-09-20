"""Share compatible imported resources and correct the complete salsa floor envelope."""
import bpy
import numpy as np


def share_resources(rigs):
    by_name={r.name:r for r in rigs}
    for prefix in ('olivia','chad'):
        walker=by_name[prefix+'_armature']
        talker=by_name[prefix+'_talking_armature']
        sources=[o for o in walker.children_recursive if o.type=='MESH']
        targets=[o for o in talker.children_recursive if o.type=='MESH']
        for target in targets:
            for source in sources:
                if len(source.data.vertices)!=len(target.data.vertices): continue
                if [g.name for g in source.vertex_groups]!=[g.name for g in target.vertex_groups]: continue
                a=np.empty(len(source.data.vertices)*3); b=np.empty_like(a)
                source.data.vertices.foreach_get('co',a); target.data.vertices.foreach_get('co',b)
                if not np.allclose(a,b,atol=1e-6): continue
                # Share only if vertex weights also have the identical bone-index mapping.
                weights=lambda mesh:[[(g.group,round(g.weight,6)) for g in v.groups] for v in mesh.data.vertices]
                if weights(source)!=weights(target): continue
                target.data=source.data
                print(f'[sharing] {target.name} shares mesh/materials/images with {source.name}',flush=True)
                break
            else:
                print(f'[sharing] {target.name}: incompatible rest mesh; retained independent mesh',flush=True)


def lift_salsa(rig, total_frames):
    meshes=[o for o in rig.children_recursive if o.type=='MESH']
    minimum=float('inf')
    for frame in range(total_frames+1):
        bpy.context.scene.frame_set(frame)
        dg=bpy.context.evaluated_depsgraph_get()
        for obj in meshes:
            evaluated=obj.evaluated_get(dg)
            mesh=evaluated.to_mesh()
            vertices=np.empty(len(mesh.vertices)*3,dtype=np.float32)
            mesh.vertices.foreach_get('co',vertices)
            matrix=np.array(evaluated.matrix_world)
            z=vertices.reshape(-1,3)@matrix[2,:3]+matrix[2,3]
            minimum=min(minimum,float(z.min()))
            evaluated.to_mesh_clear()
    lift=max(0,.001-minimum)
    bag=rig.animation_data.action.layers[0].strips[0].channelbag(rig.animation_data.action_slot)
    curve=next(c for c in bag.fcurves if c.data_path=='location' and c.array_index==2)
    for point in curve.keyframe_points: point.co.y+=lift
    curve.update()
    rig['floor_lift']=lift
    rig['floor_minimum_before']=minimum
    print(f'[floor] salsa: all {total_frames+1} frames, minimum {minimum:.6f}m; lift {lift:.6f}m; final minimum {minimum+lift:.6f}m',flush=True)
