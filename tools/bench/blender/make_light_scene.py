"""Build ``light_setup.blend`` -- the artist-facing scene the user tunes the offline
bench lighting in.

The scene mirrors what ``pw_bench`` renders for the "face" shot:

* the CuteGirl head (``cutegirl.glb``) posed at the mouth-open frame of "Emote",
* the same HDRI (``golden_gate_hills_2k.hdr``) as an environment light,
* a camera with the bench's face-camera framing (35 deg vertical FOV, 16:9),
* one sun lamp standing in for the bench's key light.

The user rotates the HDRI / tweaks exposure / drags the sun in Blender's UI, saves the
file, and ``read_light_scene.py`` converts the tuned values back into bench flags.

Run headless:
    "C:\\Program Files\\Blender Foundation\\Blender 5.2\\blender.exe" --background \
        --factory-startup --python tools/bench/blender/make_light_scene.py
Optional args after ``--``:  --no-render   skip the test render.

Conventions
-----------
glTF is Y-up and this asset faces glTF +Z.  Blender's glTF importer converts to Z-up
with (x, y, z)_gltf -> (x, -z, y)_blender, so the face looks down Blender -Y and
"up" is +Z.  Everything below is computed in Blender space; ``read_light_scene.py``
converts back with (x, y, z)_gltf = (bx, bz, -by).
"""

import math
import os
import sys

import bpy
from mathutils import Vector

HERE = os.path.dirname(os.path.abspath(__file__))
BENCH = os.path.dirname(HERE)
EXTERNAL = os.path.join(BENCH, "assets", "external")
GLB = os.path.join(EXTERNAL, "cutegirl", "cutegirl.glb")
HDRI = os.path.join(EXTERNAL, "golden_gate_hills_2k.hdr")
OUT_BLEND = os.path.join(EXTERNAL, "cutegirl", "light_setup.blend")
OUT_PNG = os.path.join(EXTERNAL, "cutegirl", "light_setup_test.png")

FPS = 60
FRAME_START = 0
FRAME_END = 300
FRAME_MOUTH_OPEN = 278       # ~4.63 s into the 12 s "Emote" clip

VFOV_DEG = 35.0              # bench face camera, vertical FOV
RES_X, RES_Y = 3840, 2160    # 16:9
HEAD_FILL = 0.75             # head covers ~75 % of the frame height
CAM_YAW_DEG = 15.0           # camera off the face axis, horizontally
SUN_YAW_DEG = 40.0           # sun to the right of the camera axis (seen from the camera)
SUN_ELEV_DEG = 40.0
SUN_STRENGTH = 3.0           # W/m^2
SUN_ANGLE_DEG = 1.0

BENCH_NOTES = (
    "PeripheralWarp bench lighting scene.\n"
    "  * HDRI yaw     : World shader -> node 'HDRI_Rotation' (Mapping) -> Rotation Z\n"
    "  * HDRI strength: World shader -> node 'HDRI_Strength' (Background) -> Strength\n"
    "  * Exposure     : Render Properties -> Color Management -> Exposure (EV)\n"
    "  * Sun          : object 'BenchSun' -- rotate it, and set Light Data -> Strength\n"
    "  * Camera       : 'BenchFaceCamera' is Track To-constrained onto the empty\n"
    "                   'FaceAim'; drag either one, the aim follows.\n"
    "Keep view transform on 'Standard' -- the bench tonemaps with Khronos PBR Neutral.\n"
    "When happy: save this .blend, then run\n"
    "  blender --background light_setup.blend --python read_light_scene.py\n"
    "to print the bench parameters."
)


def log(*a):
    print("[light-scene]", *a)


# --------------------------------------------------------------------------- helpers
def object_bboxes(objects, depsgraph):
    """Per-object world-space AABBs over the *evaluated* (posed, morphed) meshes."""
    out = []
    for obj in objects:
        if obj.type != "MESH":
            continue
        ev = obj.evaluated_get(depsgraph)
        try:
            mesh = ev.to_mesh()
        except RuntimeError:
            continue
        if mesh is None or not len(mesh.vertices):
            continue
        lo = Vector((1e30, 1e30, 1e30))
        hi = Vector((-1e30, -1e30, -1e30))
        mw = ev.matrix_world
        for v in mesh.vertices:
            p = mw @ v.co
            lo.x, lo.y, lo.z = min(lo.x, p.x), min(lo.y, p.y), min(lo.z, p.z)
            hi.x, hi.y, hi.z = max(hi.x, p.x), max(hi.y, p.y), max(hi.z, p.z)
        ev.to_mesh_clear()
        out.append((obj.name, lo, hi))
    return out


def head_bbox(objects, depsgraph, verbose=False):
    """AABB of the head cluster.

    The GLB carries a stray 2-unit "Icosphere" sitting at the origin, far below the
    head; taking a naive AABB over every mesh would swallow it.  So: take the mesh
    with the largest diagonal (the head/hair) and keep only meshes whose own box
    overlaps it (teeth, eyes, lashes ... all sit inside the head).
    """
    boxes = object_bboxes(objects, depsgraph)
    if not boxes:
        sys.exit("no mesh geometry imported")
    if verbose:
        for name, lo, hi in boxes:
            log("  mesh %-34s lo=(%.3f %.3f %.3f) hi=(%.3f %.3f %.3f)"
                % (name, lo.x, lo.y, lo.z, hi.x, hi.y, hi.z))
    boxes.sort(key=lambda b: (b[2] - b[1]).length, reverse=True)
    _, lo, hi = boxes[0]
    lo, hi = lo.copy(), hi.copy()
    for name, blo, bhi in boxes[1:]:
        if all(blo[i] <= hi[i] and bhi[i] >= lo[i] for i in range(3)):
            for i in range(3):
                lo[i] = min(lo[i], blo[i])
                hi[i] = max(hi[i], bhi[i])
        elif verbose:
            log("  (bbox: ignoring detached mesh %s)" % name)
    return lo, hi


def report_animation():
    """The glTF importer assigns the single action itself and leaves its NLA track
    muted -- do NOT unmute it, that would evaluate the clip twice."""
    acts = list(bpy.data.actions)
    log("actions:", [a.name for a in acts])
    for obj in bpy.data.objects:
        ad = obj.animation_data
        if ad is not None:
            if ad.action is None and acts:
                cand = next((a for a in acts if "emote" in a.name.lower()), acts[0])
                try:
                    ad.action = cand
                    log("assigned action", cand.name, "to", obj.name)
                except Exception as exc:                  # noqa: BLE001
                    log("could not assign action to", obj.name, exc)
            log("anim  %-24s action=%s nla=%s" % (
                obj.name, ad.action.name if ad.action else "-",
                [(t.name, "muted" if t.mute else "live") for t in ad.nla_tracks]))
        key = getattr(obj.data, "shape_keys", None) if obj.data else None
        kad = getattr(key, "animation_data", None) if key else None
        if kad is not None:
            log("morph %-24s action=%s nla=%s" % (
                obj.name, kad.action.name if kad.action else "-",
                [(t.name, "muted" if t.mute else "live") for t in kad.nla_tracks]))


def normalize_scale(scene, depsgraph):
    """Put the head at real-world size.

    Blender's glTF importer leaves this asset at ~1 unit = 0.1 mm (the Sketchfab
    export never got the 0.01 node scale baked in), so a raw import measures ~26 m
    tall.  We keep the scene in metres (unit scale 1.0) and scale the imported root
    by the nearest power of ten that lands the head near 28 cm -- a plain unit
    conversion, no squashing of the model's own proportions.  The factor actually
    used is printed.
    """
    scene.unit_settings.system = "METRIC"
    scene.unit_settings.scale_length = 1.0
    if hasattr(scene.unit_settings, "length_unit"):
        try:
            scene.unit_settings.length_unit = "CENTIMETERS"
        except TypeError:
            pass

    lo, hi = head_bbox(list(scene.objects), depsgraph, verbose=True)
    raw_h = (hi - lo).z
    k = 10.0 ** round(math.log10(0.28 / raw_h))
    log("raw head height = %.1f units -> unit factor %g (1 unit = %g m)"
        % (raw_h, k, k))
    if abs(k - 1.0) < 1e-9:
        return 1.0
    for obj in scene.objects:
        if obj.parent is None:
            obj.scale = tuple(s * k for s in obj.scale)
            log("scaled root %s by %g" % (obj.name, k))
    return k


def setup_world(scene):
    world = bpy.data.worlds.new("BenchWorld")
    scene.world = world
    world.use_nodes = True
    nt = world.node_tree
    nt.nodes.clear()

    out = nt.nodes.new("ShaderNodeOutputWorld")
    out.location = (600, 0)
    bg = nt.nodes.new("ShaderNodeBackground")
    bg.name = bg.label = "HDRI_Strength"
    bg.location = (380, 0)
    bg.inputs["Strength"].default_value = 1.0

    env = nt.nodes.new("ShaderNodeTexEnvironment")
    env.name = env.label = "HDRI_Env"
    env.location = (120, 0)
    img = bpy.data.images.load(HDRI, check_existing=True)
    env.image = img

    mapping = nt.nodes.new("ShaderNodeMapping")
    mapping.name = mapping.label = "HDRI_Rotation"
    mapping.location = (-120, 0)
    mapping.inputs["Rotation"].default_value = (0.0, 0.0, 0.0)

    texco = nt.nodes.new("ShaderNodeTexCoord")
    texco.location = (-340, 0)

    nt.links.new(texco.outputs["Generated"], mapping.inputs["Vector"])
    nt.links.new(mapping.outputs["Vector"], env.inputs["Vector"])
    nt.links.new(env.outputs["Color"], bg.inputs["Color"])
    nt.links.new(bg.outputs["Background"], out.inputs["Surface"])
    return img


def setup_cycles(scene):
    scene.render.engine = "CYCLES"
    cy = scene.cycles
    cy.samples = 64
    if hasattr(cy, "preview_samples"):
        cy.preview_samples = 32
    cy.use_denoising = True
    cy.use_adaptive_sampling = True

    # Background DLSS breaks the frame -- turn every use_dlss_* toggle off if present.
    for holder in (cy, scene.render, getattr(scene, "view_settings", None)):
        if holder is None:
            continue
        for attr in dir(holder):
            if attr.startswith("use_dlss"):
                try:
                    setattr(holder, attr, False)
                    log("disabled", attr)
                except Exception:                          # noqa: BLE001
                    pass

    # GPU (OptiX preferred) if this machine has one.
    try:
        prefs = bpy.context.preferences.addons["cycles"].preferences
        chosen = None
        for kind in ("OPTIX", "CUDA", "HIP", "ONEAPI"):
            try:
                prefs.compute_device_type = kind
            except TypeError:
                continue
            prefs.get_devices()
            if any(d.type == kind for d in prefs.devices):
                chosen = kind
                break
        if chosen:
            for d in prefs.devices:
                d.use = d.type in (chosen, "CPU")
            scene.cycles.device = "GPU"
            log("cycles device: GPU /", chosen,
                "->", [d.name for d in prefs.devices if d.use])
        else:
            scene.cycles.device = "CPU"
            log("cycles device: CPU (no GPU backend found)")
    except Exception as exc:                               # noqa: BLE001
        scene.cycles.device = "CPU"
        log("cycles device: CPU (", exc, ")")


# --------------------------------------------------------------------------- main
def main():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    do_render = "--no-render" not in argv

    for path in (GLB, HDRI):
        if not os.path.isfile(path):
            sys.exit("missing input: %s" % path)

    bpy.ops.wm.read_homefile(use_empty=True)
    scene = bpy.context.scene

    # fps must be right *before* the import: the glTF importer converts animation
    # times to frames using the scene fps.
    scene.render.fps = FPS
    scene.render.fps_base = 1.0
    scene.frame_start = FRAME_START
    scene.frame_end = FRAME_END

    log("importing", GLB)
    bpy.ops.import_scene.gltf(filepath=GLB, import_shading="NORMALS")
    log("importer unit scale =", scene.unit_settings.scale_length,
        "| system =", scene.unit_settings.system)

    report_animation()
    scene.frame_set(FRAME_MOUTH_OPEN)

    dg = bpy.context.evaluated_depsgraph_get()
    normalize_scale(scene, dg)
    bpy.context.view_layer.update()
    dg = bpy.context.evaluated_depsgraph_get()
    lo, hi = head_bbox(list(scene.objects), dg)
    dim = hi - lo
    centre = (lo + hi) * 0.5
    log("bbox min = (%.4f, %.4f, %.4f) m" % tuple(lo))
    log("bbox max = (%.4f, %.4f, %.4f) m" % tuple(hi))
    log("bbox dim = (%.4f, %.4f, %.4f) m  -> %.1f x %.1f x %.1f cm"
        % (dim.x, dim.y, dim.z, dim.x * 100, dim.y * 100, dim.z * 100))

    height = dim.z
    depth = dim.y

    # --- aim point: eye line = bbox centre + 0.1 * height ----------------------
    aim = Vector((centre.x, centre.y, centre.z + 0.1 * height))

    empty = bpy.data.objects.new("FaceAim", None)
    empty.empty_display_type = "PLAIN_AXES"
    empty.empty_display_size = max(0.02, height * 0.15)
    empty.location = aim
    scene.collection.objects.link(empty)

    # --- camera ---------------------------------------------------------------
    half = math.radians(VFOV_DEG) * 0.5
    dist = 0.5 * height / math.tan(half) / HEAD_FILL + depth * 0.5

    # Which way the face looks, in Blender space.  The nominal glTF->Blender rule
    # (+Z_gltf -> -Y_blender) does not hold for this asset -- the Sketchfab root
    # carries its own orientation -- so this was pinned empirically from the test
    # render: the face looks along Blender +Y.  The camera sits in front of it,
    # CAM_YAW_DEG around +Z off that axis.
    face_forward = Vector((0.0, 1.0, 0.0))
    yaw = math.radians(CAM_YAW_DEG)
    front = Vector((math.sin(yaw) * face_forward.y,
                    math.cos(yaw) * face_forward.y, 0.0))   # face -> viewer, horizontal
    cam_pos = aim + front * dist

    cam_data = bpy.data.cameras.new("BenchFaceCamera")
    cam_data.type = "PERSP"
    cam_data.sensor_fit = "VERTICAL"
    cam_data.sensor_height = 24.0
    cam_data.lens = (cam_data.sensor_height * 0.5) / math.tan(half)
    cam_data.clip_start = 0.01
    cam_data.clip_end = 100.0
    cam = bpy.data.objects.new("BenchFaceCamera", cam_data)
    cam.location = cam_pos
    scene.collection.objects.link(cam)
    scene.camera = cam

    fwd = (aim - cam_pos).normalized()
    cam.rotation_euler = (-fwd).to_track_quat("Z", "Y").to_euler()

    trk = cam.constraints.new("TRACK_TO")
    trk.name = "FaceAim"
    trk.target = empty
    trk.track_axis = "TRACK_NEGATIVE_Z"
    trk.up_axis = "UP_Y"

    log("camera lens = %.2f mm (vFOV %.1f deg, sensor_fit VERTICAL, h=%.1f mm)"
        % (cam_data.lens, VFOV_DEG, cam_data.sensor_height))
    log("camera dist = %.4f m, pos = (%.4f, %.4f, %.4f), aim = (%.4f, %.4f, %.4f)"
        % (dist, cam_pos.x, cam_pos.y, cam_pos.z, aim.x, aim.y, aim.z))

    # --- sun ------------------------------------------------------------------
    # "40 deg to the right of the camera axis as seen from the camera, 40 deg up"
    up = Vector((0.0, 0.0, 1.0))
    right = fwd.cross(up).normalized()                     # camera's right, horizontal
    to_cam_h = Vector((-fwd.x, -fwd.y, 0.0)).normalized()  # face -> camera, horizontal
    s = math.radians(SUN_YAW_DEG)
    e = math.radians(SUN_ELEV_DEG)
    h = (to_cam_h * math.cos(s) + right * math.sin(s)).normalized()
    to_sun = (h * math.cos(e) + up * math.sin(e)).normalized()   # face -> sun

    sun_data = bpy.data.lights.new("BenchSun", type="SUN")
    sun_data.energy = SUN_STRENGTH
    sun_data.angle = math.radians(SUN_ANGLE_DEG)
    sun_data.color = (1.0, 1.0, 1.0)
    sun = bpy.data.objects.new("BenchSun", sun_data)
    sun.location = aim + to_sun * max(1.0, dist * 3.0)
    sun.rotation_euler = (-to_sun).to_track_quat("-Z", "Y").to_euler()
    scene.collection.objects.link(sun)
    log("sun rotation = (%.2f, %.2f, %.2f) deg, travels toward (%.4f, %.4f, %.4f)"
        % (math.degrees(sun.rotation_euler.x), math.degrees(sun.rotation_euler.y),
           math.degrees(sun.rotation_euler.z), -to_sun.x, -to_sun.y, -to_sun.z))

    # --- world / render / color management -------------------------------------
    hdri_img = setup_world(scene)

    scene.render.resolution_x = RES_X
    scene.render.resolution_y = RES_Y
    scene.render.resolution_percentage = 50
    scene.render.image_settings.file_format = "PNG"
    scene.render.film_transparent = False

    vs = scene.view_settings
    try:
        vs.view_transform = "Standard"
    except TypeError:
        vs.view_transform = "Raw"
    vs.look = "None"
    vs.exposure = 0.0
    vs.gamma = 1.0

    setup_cycles(scene)

    scene["bench_notes"] = BENCH_NOTES
    txt = bpy.data.texts.new("bench_notes")
    txt.write(BENCH_NOTES)
    scene.frame_current = FRAME_MOUTH_OPEN

    # --- save ------------------------------------------------------------------
    try:
        hdri_img.pack()
    except Exception as exc:                               # noqa: BLE001
        log("image.pack() failed:", exc)
    try:
        bpy.ops.file.pack_all()
    except Exception as exc:                               # noqa: BLE001
        log("pack_all() failed:", exc)

    os.makedirs(os.path.dirname(OUT_BLEND), exist_ok=True)
    bpy.ops.wm.save_as_mainfile(filepath=OUT_BLEND, compress=True)
    log("saved", OUT_BLEND, "%.1f MB" % (os.path.getsize(OUT_BLEND) / 1e6))

    # --- test render (25 %) -----------------------------------------------------
    if do_render:
        scene.render.resolution_percentage = 25
        scene.render.filepath = OUT_PNG
        log("rendering test frame ->", OUT_PNG)
        bpy.ops.render.render(write_still=True)
        if os.path.isfile(OUT_PNG):
            log("test render OK, %d bytes (%dx%d)"
                % (os.path.getsize(OUT_PNG), RES_X // 4, RES_Y // 4))
        else:
            log("TEST RENDER MISSING:", OUT_PNG)


if __name__ == "__main__":
    main()
