"""CuteGirl Workbench check renders: close-ups of the facial test segment written next to the GLB."""

import os

import bpy
import mathutils

from cutegirl_asset import HEAD_OBJ, ROOT
from cutegirl_measure import inner_lip_pair
from export_log import make_log

log = make_log("[cutegirl]")


def check_render(frames, meshes):
    """Workbench close-ups of the test segment, written next to the GLB.

    ``frames`` is a list of ``(frame, width)`` pairs, ``width`` being how much of the face
    (in fractions of the head height) the frame should span: ~0.25 puts the mouth alone in
    the picture, ~0.45 fits the cheeks in as well.
    """
    head = bpy.data.objects[HEAD_OBJ]
    eyes = [bpy.data.objects[n] for n in ("7", "8", "9", "10") if n in bpy.data.objects]
    pair = inner_lip_pair(head, meshes[head])

    def deformed(obj):
        """World-space vertices of ``obj`` as posed at the current frame."""
        dg = bpy.context.evaluated_depsgraph_get()
        ev = obj.evaluated_get(dg)
        me = ev.to_mesh()
        cos = [obj.matrix_world @ v.co for v in me.vertices]
        ev.to_mesh_clear()
        return cos

    def aim():
        """(mouth centre, front direction, head height) for the current frame.

        Everything is re-read per frame: the Emote still moves the head around, so a camera
        parked on the rest pose would drift off the mouth.
        """
        cos = deformed(head)
        height = max(c.z for c in cos) - min(c.z for c in cos)
        hc = sum(cos, mathutils.Vector()) / len(cos)
        # the eye meshes sit toward the face, so head->eyes gives us "front"
        ec = mathutils.Vector()
        for o in eyes:
            e = deformed(o)
            ec += sum(e, mathutils.Vector()) / len(e)
        ec /= len(eyes)
        fwd = mathutils.Vector((ec.x - hc.x, ec.y - hc.y, 0.0))
        if fwd.length < 1e-6:
            fwd = mathutils.Vector((0.0, -1.0, 0.0))
        fwd.normalize()
        # aim at the lip line itself, dropped a touch so the chin stays in frame
        mouth = (cos[pair[0]] + cos[pair[1]]) * 0.5
        mouth.z -= height * 0.05
        return mouth, fwd, height

    cam_data = bpy.data.cameras.new("check_cam")
    cam_data.lens = 85.0
    cam = bpy.data.objects.new("check_cam", cam_data)
    bpy.context.scene.collection.objects.link(cam)
    bpy.context.scene.camera = cam

    sc = bpy.context.scene
    sc.render.engine = "BLENDER_WORKBENCH"
    sc.render.resolution_x = 960
    sc.render.resolution_y = 960
    sc.render.resolution_percentage = 100
    sc.render.image_settings.file_format = "PNG"
    sh = sc.display.shading
    sh.light = "STUDIO"
    sh.color_type = "SINGLE"
    sh.show_cavity = True

    out = []
    for fr, width in frames:
        sc.frame_set(fr)
        mouth, fwd, height = aim()
        span = height * width                      # world units across the frame
        dist = span * cam_data.lens / cam_data.sensor_width
        cam.location = mouth + fwd * dist
        cam.rotation_euler = (mouth - cam.location).to_track_quat("-Z", "Y").to_euler()
        path = os.path.join(ROOT, "anim_check_%d.png" % fr)
        sc.render.filepath = path
        bpy.ops.render.render(write_still=True)
        out.append(path)
        log("   rendered %s (%.3f s, %.1f units across)" % (path, fr / sc.render.fps, span))
    return out
