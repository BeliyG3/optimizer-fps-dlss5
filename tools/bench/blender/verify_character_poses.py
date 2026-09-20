"""Inspect baked rigs and render a temporary lineup without saving any blend file."""
import bpy
import math
from mathutils import Vector, Quaternion
from pathlib import Path
scene = bpy.context.scene
rigs = [o for o in scene.objects if o.type == 'ARMATURE']
scene.frame_set(scene.frame_start)
initial = {r.name: [r.matrix_world @ b.matrix for b in r.pose.bones] for r in rigs}
scene.frame_set(scene.frame_end)
for rig in rigs:
    error = max(max(abs(a[i][j]-b[i][j]) for i in range(4) for j in range(4)) for a,b in zip(initial[rig.name], [rig.matrix_world @ b.matrix for b in rig.pose.bones]))
    print(f'[pose] {rig.name}: loop matrix error {error:.8f}', flush=True)
    assert error < 0.001
scene.frame_set(scene.frame_start)
for index, rig in enumerate(rigs):
    meshes = [o for o in scene.objects if o.type == 'MESH' and any(m.type == 'ARMATURE' and m.object == rig for m in o.modifiers)]
    dg = bpy.context.evaluated_depsgraph_get()
    zs = []
    for mesh in meshes:
        evaluated = mesh.evaluated_get(dg)
        data = evaluated.to_mesh()
        zs.extend((evaluated.matrix_world @ v.co).z for v in data.vertices)
        evaluated.to_mesh_clear()
    print(f'[pose] {rig.name}: start floor {min(zs):.4f}, top {max(zs):.4f}; bone mode {rig.pose.bones[0].rotation_mode}', flush=True)
    left = next(b for b in rig.pose.bones if b.name.endswith('LeftArm'))
    right = next(b for b in rig.pose.bones if b.name.endswith('RightArm'))
    forward = Vector((0, 0, 1)).cross(rig.matrix_world.to_3x3() @ (right.head-left.head))
    angle = math.atan2(forward.y, forward.x)
    expected = {'hero_armature': math.pi/2, 'olivia_armature': -math.pi/2,
                'chad_armature': -math.pi/2, 'hiphop_armature': -0.25,
                'salsa_armature': -math.pi/2, 'mannequin_armature': math.pi/2, 'dummy_armature': -math.pi/2,
                'olivia_talking_armature': math.pi/2, 'chad_talking_armature': -math.pi/2}[rig.name]
    assert abs(math.atan2(math.sin(angle-expected), math.cos(angle-expected))) < 0.001
    print(f'[pose] {rig.name}: heading {math.degrees(angle):.3f} degrees', flush=True)
    rig.animation_data_clear()
    rig.rotation_quaternion = Quaternion((0, 0, 1), -math.pi/2-angle) @ rig.rotation_quaternion
    rig.location.x = index*2.1
    rig.location.y = 0
    # Present original local forward towards the preview camera.

bpy.context.view_layer.update()
bpy.ops.object.camera_add(location=(8.4,-20,5.5))
camera = bpy.context.object
camera.rotation_euler = (Vector((8.4,0,0.9))-camera.location).to_track_quat('-Z','Y').to_euler()
camera.data.type = 'ORTHO'
camera.data.ortho_scale = 20
scene.camera = camera
scene.render.engine = 'BLENDER_WORKBENCH'
scene.display.shading.light = 'STUDIO'
scene.display.shading.color_type = 'MATERIAL'
scene.display.shading.show_shadows = True
scene.display.shading.show_cavity = True
scene.render.resolution_x, scene.render.resolution_y = 2100, 500
scene.render.resolution_percentage = 100
scene.render.filepath = str(Path(bpy.data.filepath).with_name('characters_preview.png'))
bpy.ops.render.render(write_still=True)

