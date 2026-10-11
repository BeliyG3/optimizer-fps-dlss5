"""The light and render setup of the lab bake (bake_lab_lightmaps.py): the device, Cycles settings, the sun and sky of
the bench's lab command (a constant, or the --hdri panorama), and the emission muting of the sun-only and lamp-only
bakes."""
import math

import bpy
from mathutils import Vector

from lab_lightmap_uv import node_trees

EMISSION_STRENGTH = {'BSDF_PRINCIPLED': 'Emission Strength', 'PRINCIPLED_VOLUME': 'Emission Strength', 'EMISSION': 'Strength',
                     'BACKGROUND': 'Strength'}
PASSIVE = {'GROUP', 'GROUP_INPUT', 'GROUP_OUTPUT', 'REROUTE', 'FRAME'}  # their sockets pass values on; groups are scanned
TEXTURE_LIMIT = '2048'  # Simplify's render texture limit during the bakes (VRAM)


def enable_optix(scene):
    """--factory-startup has no compute device: enable every OptiX device, or the bake silently runs on the CPU."""
    prefs = bpy.context.preferences.addons['cycles'].preferences
    prefs.compute_device_type = 'OPTIX'
    (prefs.refresh_devices if hasattr(prefs, 'refresh_devices') else prefs.get_devices)()
    for device in prefs.devices:
        device.use = device.type == 'OPTIX'
        print(f"[bake] device {device.name!r} type {device.type} use {device.use}", flush=True)
    if not any(d.use for d in prefs.devices):
        raise RuntimeError("no OptiX device: the bake would run on the CPU (and an earlier attempt came out black)")
    scene.cycles.device = 'GPU'


def render_settings(scene, samples, bounces):
    scene.render.engine = 'CYCLES'
    cycles = scene.cycles
    cycles.samples = samples
    cycles.use_adaptive_sampling = False
    cycles.use_denoising = False
    # The bench's path tracer: --bounces 4 by default and no clamp before its final pixel clamp.
    for name in ('max_bounces', 'diffuse_bounces', 'glossy_bounces', 'transmission_bounces'):
        setattr(cycles, name, bounces)
    cycles.sample_clamp_direct = cycles.sample_clamp_indirect = 0.0
    for owner in (cycles, scene.render):  # DLSS builds: off for offline work (a background render came out cropped)
        for name in ('use_dlss_render', 'use_dlss_preview', 'use_dlss_frame_generation'):
            if hasattr(owner, name):
                setattr(owner, name, False)


def simplify(scene):
    """Simplify on with the render texture limit, for the bakes. Returns the undo for restore_simplify()."""
    saved = (scene.render.use_simplify, scene.cycles.texture_limit_render)
    scene.render.use_simplify = True
    scene.cycles.texture_limit_render = TEXTURE_LIMIT
    return saved


def restore_simplify(scene, saved):
    scene.render.use_simplify, scene.cycles.texture_limit_render = saved


def blender_direction(bench):
    """The bench's --sun-dir is in its left-handed Y-up space, glTF (x, y, -z) (pw_gltf.h). glTF (x, y, z) is Blender
    Z-up (x, -z, y) under the exporter's +Y-up conversion, so the bench's (x, y, z) is Blender (x, z, y)."""
    x, y, z = bench
    return Vector((x, z, y)).normalized()


def add_sun(scene, bench_dir, strength, angle_deg):
    light = bpy.data.lights.new("lightmap_sun", 'SUN')
    light.energy = strength  # W/m², the irradiance the bench's --sun-strength is (unit factor 1; scaleSun calibrates)
    light.angle = math.radians(angle_deg)  # the angular diameter, as the bench's --sun-angle
    sun = bpy.data.objects.new("lightmap_sun", light)
    scene.collection.objects.link(sun)
    travel = blender_direction(bench_dir)
    sun.rotation_mode = 'QUATERNION'
    sun.rotation_quaternion = travel.to_track_quat('-Z', 'Y')
    bpy.context.view_layer.update()
    axis = (sun.matrix_world.to_3x3() @ Vector((0.0, 0.0, -1.0))).normalized()
    print(f"[bake] sun -Z {tuple(round(v, 5) for v in axis)} expected {tuple(round(v, 5) for v in travel)} "
          f"angle {math.degrees(light.angle):.3f} deg strength {light.energy}", flush=True)
    if (axis - travel).length > 1e-5:
        raise RuntimeError("sun orientation")
    return sun


def set_sky(scene, rgb):
    world = bpy.data.worlds.new("lightmap_sky")
    if world.node_tree is None:
        world.use_nodes = True
    background = next(n for n in world.node_tree.nodes if n.type == 'BACKGROUND')
    background.inputs['Color'].default_value = (*rgb, 1.0)
    background.inputs['Strength'].default_value = 1.0
    previous, scene.world = scene.world, world
    print(f"[bake] world background {tuple(background.inputs['Color'].default_value)[:3]} strength 1", flush=True)
    return previous


def set_environment(scene, path, strength=1.0):
    """The World as the bench's --hdri sky: the panorama's texels at bench12's lookup (scene.hlsli Sky(): u =
    atan2(z, x) / 2pi + 0.5, v = acos(y) / pi from the top, bench (x, y, z) = Blender (x, z, y)). Blender's
    equirectangular lookup is u = 0.5 - atan2(y, x) / 2pi (measured: +X at u 0.5, +Y at 0.25), the bench's mirror
    image, so the direction's y is negated (Mapping, scale (1, -1, 1)); rotation 0, since the prepared file is already
    aligned (tools/bench/hdri_prepare.py), and strength 1, since the bench applies no scale. Returns the previous
    World."""
    world = bpy.data.worlds.new("lightmap_hdri")
    if world.node_tree is None:
        world.use_nodes = True
    nodes, links = world.node_tree.nodes, world.node_tree.links
    background = next(n for n in nodes if n.type == 'BACKGROUND')
    coordinates, mapping = nodes.new('ShaderNodeTexCoord'), nodes.new('ShaderNodeMapping')
    mapping.vector_type = 'VECTOR'
    mapping.inputs['Scale'].default_value = (1.0, -1.0, 1.0)
    texture = nodes.new('ShaderNodeTexEnvironment')
    texture.image = bpy.data.images.load(str(path), check_existing=True)
    texture.image.colorspace_settings.name = 'Linear Rec.709'  # radiance as stored, never an sRGB decode
    texture.interpolation = 'Linear'
    links.new(coordinates.outputs['Generated'], mapping.inputs['Vector'])
    links.new(mapping.outputs['Vector'], texture.inputs['Vector'])
    links.new(texture.outputs['Color'], background.inputs['Color'])
    background.inputs['Strength'].default_value = strength
    previous, scene.world = scene.world, world
    print(f"[bake] world environment {path} ({texture.image.size[0]}x{texture.image.size[1]}, "
          f"{texture.image.colorspace_settings.name}), direction y mirrored, strength {strength}", flush=True)
    return previous


def _animated_paths(tree):
    """Data paths of the tree that drivers or an action set."""
    data = tree.animation_data
    if data is None:
        return set()
    paths = {driver.data_path for driver in data.drivers}
    action = data.action
    if action is not None:
        curves = getattr(action, 'fcurves', None)
        if curves is None:  # layered actions
            curves = [c for layer in action.layers for strip in layer.strips for bag in strip.channelbags for c in bag.fcurves]
        paths |= {curve.data_path for curve in curves}
    return paths


def mute_emission(world, materials=True):
    """For the sun-only bake: every emission strength in the materials, their node groups and the sky set to 0; for
    the lamp-only bake (materials=False) the sky's alone. Raises instead, listing them, on a strength that is linked or
    animated (setting it would not mute it) and on a node that may emit with no strength to mute. A linked Emission
    Color is fine: strength 0 makes it irrelevant. Returns the undo list for restore()."""
    trees = node_trees(([m.node_tree for m in bpy.data.materials] if materials else []) + [world.node_tree])
    kind = 'sun' if materials else 'lamp'
    sockets, problems, coloured = [], [], 0
    for tree in trees:
        animated = _animated_paths(tree)
        for node in tree.nodes:
            name = EMISSION_STRENGTH.get(node.type)
            if name:
                socket = node.inputs[name]
                if socket.is_linked or socket.path_from_id('default_value') in animated:
                    problems.append(f"{tree.name}/{node.name}: {name} is {'linked' if socket.is_linked else 'animated'}")
                sockets.append(socket)
                coloured += any(s.is_linked for s in node.inputs if s.name in ('Emission Color', 'Color'))
            elif node.type == 'SCRIPT' or (node.type not in PASSIVE and any('emission' in s.name.lower() for s in node.inputs)):
                problems.append(f"{tree.name}/{node.name}: a {node.type} node that may emit")
    if problems:
        raise RuntimeError(f"the {kind}-only bake cannot mute this emission:\n  " + "\n  ".join(problems))
    saved = [(socket, socket.default_value) for socket in sockets]
    for socket in sockets:
        socket.default_value = 0.0
    print(f"[bake] {kind} only: {len(sockets)} emission strengths muted in {len(trees)} node trees "
          f"({coloured} with a linked colour, irrelevant at strength 0)", flush=True)
    return saved


def restore(saved):
    for socket, value in saved:
        socket.default_value = value
