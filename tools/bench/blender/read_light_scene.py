"""Read the tuned ``light_setup.blend`` back out as bench parameters.

Run:
    "C:\\Program Files\\Blender Foundation\\Blender 5.2\\blender.exe" --background \\
        tools/bench/assets/external/cutegirl/light_setup.blend \\
        --python tools/bench/blender/read_light_scene.py [-- --out params.json]

It prints exactly one JSON line (the only line starting with '{' -- everything else on
stdout is Blender's own chatter), e.g.:
    blender --background light_setup.blend --python read_light_scene.py | grep '^{'

Coordinate conventions
----------------------
Blender is Z-up, the bench/glTF side is Y-up.  Conversion used here:
    (x, y, z)_gltf = (bx, bz, -by)
``sun_dir_world`` is the direction the light *travels toward* -- i.e. the lamp
object's local -Z axis in world space (Blender's sun shines down its -Z).  Negate it
to get the "direction to the sun" that most shading code wants.

Bench mapping (documented approximations)
-----------------------------------------
exposure:
    bench --exposure = 2^exposure_ev * hdri_strength * BENCH_DEFAULT_EXPOSURE
    where BENCH_DEFAULT_EXPOSURE = 0.7 is the bench's current default, i.e. we
    ASSUME that Blender at EV 0 with Background Strength 1.0 looks like the bench at
    --exposure 0.7.  This is an approximation: Blender's Filmic/Standard view
    transform is not bit-identical to the bench's Khronos PBR Neutral tonemap, and
    the bench's HDRI importance sampling differs from Cycles'.  Treat the number as
    a starting point, then nudge by eye.

hdri yaw:
    bench --hdri-yaw is being wired up by another change and its sign convention may
    not match Blender's Mapping-node Z rotation, so BOTH signs are reported
    (`hdri_yaw_deg` and `hdri_yaw_deg_negated`).  The bench samples the environment
    with
        u = atan2(d.x, -d.z) / (2*pi) + 0.5,    v = acos(d.y) / pi
    (d = world-space direction, glTF Y-up), so a yaw that rotates d about +Y by
    +theta shifts u by +theta/2pi.  Blender's Mapping node rotates the *lookup
    vector* about Blender +Z (= glTF +Y) by +Z_rot, which shifts the visible
    environment the opposite way.  Pick the sign by rendering both and matching
    where the sun sits; `sun_dir_world_gltf` in this JSON is the ground truth to
    match against.

Nothing here writes to the .blend.
"""

import json
import math
import os
import sys

import bpy
from mathutils import Vector

BENCH_DEFAULT_EXPOSURE = 0.7


def to_yup(v):
    """Blender Z-up -> glTF Y-up: (x, y, z) = (bx, bz, -by)."""
    return [round(v.x, 6), round(v.z, 6), round(-v.y, 6)]


def r(v, n=6):
    return round(float(v), n)


def find_node(world, name, bl_idname):
    if world is None or world.node_tree is None:
        return None
    nt = world.node_tree
    node = nt.nodes.get(name)
    if node is not None:
        return node
    for n in nt.nodes:
        if n.label == name or n.bl_idname == bl_idname:
            return n
    return None


def main():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    out_path = None
    if "--out" in argv:
        out_path = argv[argv.index("--out") + 1]

    scene = bpy.context.scene
    dg = bpy.context.evaluated_depsgraph_get()

    # ---- world -------------------------------------------------------------
    world = scene.world
    mapping = find_node(world, "HDRI_Rotation", "ShaderNodeMapping")
    bg = find_node(world, "HDRI_Strength", "ShaderNodeBackground")
    env = find_node(world, "HDRI_Env", "ShaderNodeTexEnvironment")

    yaw_deg = 0.0
    if mapping is not None:
        yaw_deg = math.degrees(mapping.inputs["Rotation"].default_value[2])
    strength = float(bg.inputs["Strength"].default_value) if bg is not None else 1.0
    hdri_path = ""
    if env is not None and env.image is not None:
        hdri_path = bpy.path.abspath(env.image.filepath) or env.image.name

    # ---- color management --------------------------------------------------
    vs = scene.view_settings
    exposure_ev = float(vs.exposure)

    # ---- sun ---------------------------------------------------------------
    sun = bpy.data.objects.get("BenchSun")
    if sun is None:
        sun = next((o for o in scene.objects
                    if o.type == "LIGHT" and o.data.type == "SUN"), None)
    if sun is None:
        sun_dir = Vector((0.0, 0.0, -1.0))
        sun_strength, sun_color, sun_angle = 0.0, [1.0, 1.0, 1.0], 0.0
        sun_rot = [0.0, 0.0, 0.0]
    else:
        m = sun.evaluated_get(dg).matrix_world
        sun_dir = (m.to_3x3() @ Vector((0.0, 0.0, -1.0))).normalized()
        sun_strength = float(sun.data.energy)
        sun_color = [r(c) for c in sun.data.color]
        sun_angle = math.degrees(sun.data.angle)
        sun_rot = [r(math.degrees(a)) for a in sun.rotation_euler]

    # ---- camera ------------------------------------------------------------
    cam = bpy.data.objects.get("BenchFaceCamera") or scene.camera
    cam_pos = Vector((0.0, 0.0, 0.0))
    cam_target = Vector((0.0, 0.0, 0.0))
    vfov = 0.0
    if cam is not None:
        mw = cam.evaluated_get(dg).matrix_world
        cam_pos = mw.to_translation()
        fwd = (mw.to_3x3() @ Vector((0.0, 0.0, -1.0))).normalized()
        aim = bpy.data.objects.get("FaceAim")
        if aim is not None:
            cam_target = aim.evaluated_get(dg).matrix_world.to_translation()
        else:
            cam_target = cam_pos + fwd
        cd = cam.data
        sensor = cd.sensor_height if cd.sensor_fit == "VERTICAL" else cd.sensor_width
        if cd.sensor_fit == "AUTO":
            # AUTO fits the sensor to the long axis; derive the vertical FOV from
            # the render aspect.
            rx = scene.render.resolution_x * scene.render.pixel_aspect_x
            ry = scene.render.resolution_y * scene.render.pixel_aspect_y
            sensor = cd.sensor_width * (ry / rx if rx >= ry else 1.0)
        vfov = math.degrees(2.0 * math.atan((sensor * 0.5) / cd.lens))

    bench_exposure = (2.0 ** exposure_ev) * strength * BENCH_DEFAULT_EXPOSURE

    data = {
        "blend_file": bpy.data.filepath,
        "frame": scene.frame_current,
        "fps": scene.render.fps / scene.render.fps_base,

        "hdri_path": hdri_path,
        "hdri_yaw_deg": r(yaw_deg, 4),
        "hdri_yaw_deg_negated": r(-yaw_deg, 4),
        "hdri_strength": r(strength, 6),
        "exposure_ev": r(exposure_ev, 6),
        "view_transform": vs.view_transform,
        "look": vs.look,
        "gamma": r(vs.gamma, 6),

        "sun_rotation_euler_deg": sun_rot,
        "sun_dir_world": [r(sun_dir.x), r(sun_dir.y), r(sun_dir.z)],
        "sun_dir_world_gltf": to_yup(sun_dir),
        "sun_dir_to_sun_gltf": to_yup(-sun_dir),
        "sun_strength": r(sun_strength, 6),
        "sun_color": sun_color,
        "sun_angle_deg": r(sun_angle, 4),

        "camera_position": to_yup(cam_pos),
        "camera_target": to_yup(cam_target),
        "camera_position_blender": [r(cam_pos.x), r(cam_pos.y), r(cam_pos.z)],
        "camera_target_blender": [r(cam_target.x), r(cam_target.y), r(cam_target.z)],
        "camera_vfov_deg": r(vfov, 4),
        "camera_lens_mm": r(cam.data.lens, 4) if cam else 0.0,
        "resolution": [scene.render.resolution_x, scene.render.resolution_y],

        "bench": {
            "--exposure": r(bench_exposure, 6),
            "--hdri-yaw": r(yaw_deg, 4),
            "--hdri-yaw-alt-sign": r(-yaw_deg, 4),
            "--sun-dir": to_yup(sun_dir),
            "--sun-strength": r(sun_strength, 6),
        },
        "notes": {
            "units": "metres, Blender Z-up; glTF conversion (x,y,z)=(bx,bz,-by)",
            "sun_dir_world": "direction the light TRAVELS TOWARD (lamp -Z axis)",
            "exposure_mapping": ("--exposure = 2^exposure_ev * hdri_strength * %.3f "
                                 "(%.3f = bench default that is ASSUMED to match "
                                 "Blender at EV 0 / strength 1; approximate, the "
                                 "tonemaps are not identical)"
                                 % (BENCH_DEFAULT_EXPOSURE, BENCH_DEFAULT_EXPOSURE)),
            "hdri_yaw_sign": ("bench EquirectUV: u = atan2(d.x, -d.z)/(2*pi) + 0.5, "
                              "v = acos(d.y)/pi (d in glTF Y-up). Blender's Mapping "
                              "node rotates the lookup vector about +Z (= glTF +Y), "
                              "which moves the environment the other way, so try "
                              "both --hdri-yaw and --hdri-yaw-alt-sign and match the "
                              "sun position against sun_dir_world_gltf."),
        },
    }

    line = json.dumps(data, separators=(",", ":"))
    print(line)
    if out_path:
        with open(os.path.abspath(out_path), "w", encoding="utf-8") as fh:
            fh.write(line + "\n")


if __name__ == "__main__":
    main()
