"""Render an unsaved staging overview with overhead architecture hidden."""
import sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent))
import bpy
from mathutils import Vector
from character_export import append_characters
from lab_geometry import bounds
append_characters()
scene=bpy.context.scene
scene.frame_set(0)
for obj in scene.objects:
    if obj.type=='MESH' and not obj.parent and obj.name.startswith(('shell_','lamp_','ring_','dress_','screens_big','sign_')):
        lo,hi=bounds(obj)
        if hi.z>2.4: obj.hide_render=True
bpy.ops.object.camera_add(location=(22,-30,35))
camera=bpy.context.object
camera.rotation_euler=(Vector((0,2,0))-camera.location).to_track_quat('-Z','Y').to_euler()
camera.data.type='ORTHO'
camera.data.ortho_scale=34
scene.camera=camera
scene.render.engine='BLENDER_WORKBENCH'
scene.display.shading.light='STUDIO'
scene.display.shading.color_type='MATERIAL'
scene.display.shading.show_shadows=True
scene.display.shading.show_cavity=True
scene.render.resolution_x=1500
scene.render.resolution_y=1500
scene.render.resolution_percentage=100
scene.render.filepath=str(Path(__file__).resolve().parents[1]/'assets/round2_staging_preview.png')
bpy.ops.render.render(write_still=True)
