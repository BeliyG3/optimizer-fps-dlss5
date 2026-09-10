# Builds the pw_bench 3D scene in Blender so it can be edited by hand and exported back as glTF.
#   blender.exe --background --factory-startup --python build_bench_scene.py -- <out.blend> [out.glb]
# Bench axes are D3D (x right, y up, z forward, left-handed); Blender is z-up right-handed:
#   blender (X, Y, Z) = bench (x, z, y).
# Objects: "Ground" (600 m plane, checker material), "Box_NN" (palette colour + stripes by local
# height, colour index in the custom property "pw_colour"), the figure boxes in the collection
# "Character", the camera "BenchCamera" animated with the bench's scripted path (60 fps, 600 frames:
# still / yaw 45 deg/s / forward 2 m/s / strafe 2 m/s / yaw + forward), 60 deg vertical FOV, 3840x2160.
import bpy, json, math, os, sys
from mathutils import Vector

argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
out_blend = argv[0] if argv else os.path.join(os.path.dirname(__file__), "bench_scene.blend")
out_glb = argv[1] if len(argv) > 1 else None
here = os.path.dirname(os.path.abspath(__file__))
boxes = json.load(open(os.path.join(here, "bench_boxes.json")))

PALETTE = [(0.85, 0.25, 0.2), (0.2, 0.7, 0.3), (0.25, 0.4, 0.9), (0.9, 0.8, 0.2), (0.8, 0.3, 0.8), (0.9, 0.9, 0.9)]

scene = bpy.context.scene
for o in list(scene.objects):
    bpy.data.objects.remove(o, do_unlink=True)
scene.render.fps = 60
scene.frame_start = 0
scene.frame_end = 599
scene.render.resolution_x = 3840
scene.render.resolution_y = 2160


def to_blender(v):  # bench (x, y, z) -> blender (x, z, y)
    return (v[0], v[2], v[1])


def link(obj, coll):
    coll.objects.link(obj)


def cube_mesh(name, half):
    hx, hy, hz = half  # bench half extents (x, y up, z) -> blender (x, z, y)
    bx, by, bz = hx, hz, hy
    verts = [(sx * bx, sy * by, sz * bz) for sz in (-1, 1) for sy in (-1, 1) for sx in (-1, 1)]
    faces = [(0, 2, 3, 1), (4, 5, 7, 6), (0, 1, 5, 4), (2, 6, 7, 3), (0, 4, 6, 2), (1, 3, 7, 5)]
    m = bpy.data.meshes.new(name)
    m.from_pydata(verts, [], faces)
    m.update()
    return m


def stripe_material(name, rgb):
    """Palette colour with 0.2 m stripes (factor 1.0 / 0.55) by the object's local height."""
    mat = bpy.data.materials.new(name)
    mat.use_nodes = True
    nt = mat.node_tree
    for n in list(nt.nodes):
        nt.nodes.remove(n)
    out = nt.nodes.new("ShaderNodeOutputMaterial")
    bsdf = nt.nodes.new("ShaderNodeBsdfPrincipled")
    bsdf.inputs["Base Color"].default_value = (*rgb, 1.0)  # constant: this is what the glTF exporter writes
    bsdf.inputs["Roughness"].default_value = 1.0
    coord = nt.nodes.new("ShaderNodeTexCoord")
    sep = nt.nodes.new("ShaderNodeSeparateXYZ")
    # stripe = floor((z_local + half_height) * 5) mod 2 -> the object origin is the box centre, so add the
    # half height through an object property later; here the pattern simply alternates every 0.2 m.
    mul = nt.nodes.new("ShaderNodeMath"); mul.operation = "MULTIPLY"; mul.inputs[1].default_value = 5.0
    flo = nt.nodes.new("ShaderNodeMath"); flo.operation = "FLOOR"
    mod = nt.nodes.new("ShaderNodeMath"); mod.operation = "MODULO"; mod.inputs[1].default_value = 2.0
    ab = nt.nodes.new("ShaderNodeMath"); ab.operation = "ABSOLUTE"
    less = nt.nodes.new("ShaderNodeMath"); less.operation = "LESS_THAN"; less.inputs[1].default_value = 1.0
    ramp = nt.nodes.new("ShaderNodeMapRange"); ramp.inputs["From Min"].default_value = 0.0; ramp.inputs["From Max"].default_value = 1.0
    ramp.inputs["To Min"].default_value = 0.55; ramp.inputs["To Max"].default_value = 1.0
    colour = nt.nodes.new("ShaderNodeRGB"); colour.outputs[0].default_value = (*rgb, 1.0)
    mix = nt.nodes.new("ShaderNodeMix"); mix.data_type = "RGBA"; mix.blend_type = "MULTIPLY"; mix.inputs["Factor"].default_value = 1.0
    l = nt.links.new
    l(coord.outputs["Object"], sep.inputs[0]); l(sep.outputs["Z"], mul.inputs[0]); l(mul.outputs[0], flo.inputs[0])
    l(flo.outputs[0], ab.inputs[0]); l(ab.outputs[0], mod.inputs[0]); l(mod.outputs[0], less.inputs[0]); l(less.outputs[0], ramp.inputs["Value"])
    l(colour.outputs[0], mix.inputs["A"])
    grey = nt.nodes.new("ShaderNodeCombineColor")
    l(ramp.outputs[0], grey.inputs[0]); l(ramp.outputs[0], grey.inputs[1]); l(ramp.outputs[0], grey.inputs[2])
    l(grey.outputs[0], mix.inputs["B"])
    # The striped preview goes to the emission input so the viewport shows the bands while the exported
    # base colour stays the plain palette colour (the bench draws the stripes itself for Palette_* materials).
    l(mix.outputs["Result"], bsdf.inputs["Emission Color"]); bsdf.inputs["Emission Strength"].default_value = 0.0
    l(bsdf.outputs[0], out.inputs[0])
    mat.diffuse_color = (*rgb, 1.0)
    return mat


def ground_material():
    mat = bpy.data.materials.new("Ground")
    mat.use_nodes = True
    nt = mat.node_tree
    for n in list(nt.nodes):
        nt.nodes.remove(n)
    out = nt.nodes.new("ShaderNodeOutputMaterial")
    bsdf = nt.nodes.new("ShaderNodeBsdfPrincipled")
    bsdf.inputs["Roughness"].default_value = 1.0
    coord = nt.nodes.new("ShaderNodeTexCoord")
    checker = nt.nodes.new("ShaderNodeTexChecker"); checker.inputs["Scale"].default_value = 1.0  # 1 m squares
    checker.inputs["Color1"].default_value = (0.55, 0.5, 0.45, 1.0); checker.inputs["Color2"].default_value = (0.55 * 0.35, 0.5 * 0.35, 0.45 * 0.35, 1.0)
    l = nt.links.new
    l(coord.outputs["Object"], checker.inputs["Vector"]); l(checker.outputs["Color"], bsdf.inputs["Base Color"]); l(bsdf.outputs[0], out.inputs[0])
    mat.diffuse_color = (0.5, 0.48, 0.45, 1.0)
    return mat


mats = [stripe_material("Palette_%d" % i, rgb) for i, rgb in enumerate(PALETTE)]

# Ground
gm = bpy.data.meshes.new("Ground")
gm.from_pydata([(-300, -300, 0), (300, -300, 0), (300, 300, 0), (-300, 300, 0)], [], [(0, 1, 2, 3)])
gm.update()
ground = bpy.data.objects.new("Ground", gm)
ground.data.materials.append(ground_material())
link(ground, scene.collection)

# Boxes: the first six are the figure at the orbit centre.
character = bpy.data.collections.new("Character")
scene.collection.children.link(character)
world = bpy.data.collections.new("World")
scene.collection.children.link(world)
for b in boxes:
    name = "Box_%02d" % b["id"]
    obj = bpy.data.objects.new(name, cube_mesh(name, b["half"]))
    obj.location = to_blender(b["pos"])
    obj.data.materials.append(mats[b["colour"] % 6])
    obj["pw_colour"] = b["colour"]
    obj["pw_moving"] = 1 if b["id"] >= len(boxes) - 2 else 0  # the last two boxes move in the bench (--moving-boxes)
    link(obj, character if b["id"] < 6 else world)

# Camera with the bench's scripted path.
cam_data = bpy.data.cameras.new("BenchCamera")
cam_data.sensor_fit = "VERTICAL"
cam_data.angle_y = math.radians(60.0)
cam_data.clip_start = 0.1
cam_data.clip_end = 300.0
cam = bpy.data.objects.new("BenchCamera", cam_data)
link(cam, scene.collection)
scene.camera = cam

phase_length = 120
yaw_rate = math.radians(45.0) / 60.0
move_rate = 2.0 / 60.0


def camera_at(frame):
    yaw = forward = strafe = 0.0
    for f in range(frame):
        p = (f // phase_length) % 5
        if p == 1: yaw += yaw_rate
        elif p == 2: forward += move_rate
        elif p == 3: strafe += move_rate
        elif p == 4: yaw += yaw_rate * 0.6; forward += move_rate * 0.7
    d = (math.sin(yaw), 0.0, math.cos(yaw))
    r = (math.cos(yaw), 0.0, -math.sin(yaw))
    base = (d[0] * forward + r[0] * strafe, 0.0, d[2] * forward + r[2] * strafe)
    target = (base[0], base[1] + 1.0, base[2])
    eye = (target[0] - d[0] * 5.0, target[1] + 0.9, target[2] - d[2] * 5.0)
    return eye, target


bpy.context.preferences.edit.keyframe_new_interpolation_type = "LINEAR"  # the path is sampled per frame
cam.rotation_mode = "QUATERNION"
for frame in range(0, 600):
    eye, target = camera_at(frame)
    e = Vector(to_blender(eye)); t = Vector(to_blender(target))
    cam.location = e
    cam.rotation_quaternion = (t - e).to_track_quat("-Z", "Y")
    cam.keyframe_insert("location", frame=frame)
    cam.keyframe_insert("rotation_quaternion", frame=frame)

# The figure's walk (forward / strafe phases) moves the whole Character collection with the camera
# target; store the target path on an empty so it can be edited as well.
anchor = bpy.data.objects.new("CharacterAnchor", None)
anchor.empty_display_type = "PLAIN_AXES"
link(anchor, scene.collection)
for frame in range(0, 600):
    _, target = camera_at(frame)
    anchor.location = to_blender((target[0], 0.0, target[2]))
    anchor.keyframe_insert("location", frame=frame)
for obj in character.objects:
    obj.parent = anchor

scene.frame_set(0)
bpy.ops.wm.save_as_mainfile(filepath=out_blend)
print("saved", out_blend)
if out_glb:
    bpy.ops.export_scene.gltf(filepath=out_glb, export_format="GLB", export_animations=True, export_cameras=True, export_extras=True, export_apply=True)
    print("exported", out_glb)
