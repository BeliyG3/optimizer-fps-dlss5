# Builds tools/bench/assets/lab_scene.glb: a dim industrial lab lit by lamp tubes, in the spirit of the
# MI6 workshop of 007 First Light, for pw_bench (--gltf ... --hdr). Background Blender, nothing interactive:
#
#   "C:\Program Files\Blender Foundation\Blender 5.2\blender.exe" --background --factory-startup ^
#       --python tools\bench\blender\build_lab_scene.py
#
# What the scene is for (each item is a defect first seen in the game and invisible in the daylight scene):
#   - lamp tubes and screens with a radiance of tens of units next to dark surfaces (linear HDR frames);
#   - a polished floor: the lamps' highlights slide over it while its motion vectors describe the floor;
#   - a character seen from behind, large in the frame, in front of clutter (disocclusion, silhouettes);
#   - thin and high-contrast geometry: racks, cables, a fire extinguisher on a pillar.
#
# The props are CC0 models from Poly Haven fetched by tools/bench/assets/fetch_polyhaven.py; the character
# is appended from assets/bench_scene.blend. Every emissive material becomes a point light in the bench.
import math
import os
import sys

import bpy

HERE = os.path.dirname(os.path.abspath(__file__))
ASSETS = os.path.normpath(os.path.join(HERE, "..", "assets"))
HAVEN = os.path.join(ASSETS, "external", "polyhaven")
OUT = os.path.join(ASSETS, "lab_scene.glb")
BLEND = os.path.join(ASSETS, "lab_scene.blend")  # the editable scene: hand-tuned after the first build

HALL_X, HALL_Y, HALL_Z = 22.0, 16.0, 4.6  # metres; the character stands at the origin


# ---- materials ------------------------------------------------------------------------------------------------
def principled(name):
    mat = bpy.data.materials.new(name)
    mat.use_nodes = True
    return mat, mat.node_tree.nodes["Principled BSDF"]


def textured(name, folder, tint, roughness):
    """Base colour = image x tint (the one pattern the glTF exporter carries), constant roughness."""
    mat, bsdf = principled(name)
    bsdf.inputs["Roughness"].default_value = roughness
    directory = os.path.join(HAVEN, "textures", folder)
    diffuse = next((f for f in sorted(os.listdir(directory)) if "diff" in f.lower()), None) if os.path.isdir(directory) else None
    if diffuse is None:
        bsdf.inputs["Base Color"].default_value = (*tint, 1.0)
        return mat
    nodes, links = mat.node_tree.nodes, mat.node_tree.links
    image = nodes.new("ShaderNodeTexImage")
    image.image = bpy.data.images.load(os.path.join(directory, diffuse))
    mix = nodes.new("ShaderNodeMix")
    mix.data_type = "RGBA"
    mix.blend_type = "MULTIPLY"
    mix.inputs["Factor"].default_value = 1.0
    links.new(image.outputs["Color"], mix.inputs["A"])
    mix.inputs["B"].default_value = (*tint, 1.0)
    links.new(mix.outputs["Result"], bsdf.inputs["Base Color"])
    return mat


def emissive(name, colour, strength):
    mat, bsdf = principled(name)
    bsdf.inputs["Base Color"].default_value = (0.0, 0.0, 0.0, 1.0)
    bsdf.inputs["Emission Color"].default_value = (*colour, 1.0)
    bsdf.inputs["Emission Strength"].default_value = strength
    return mat


def plain(name, colour, roughness=0.8):
    mat, bsdf = principled(name)
    bsdf.inputs["Base Color"].default_value = (*colour, 1.0)
    bsdf.inputs["Roughness"].default_value = roughness
    return mat


# ---- geometry ---------------------------------------------------------------------------------------------------
def box(name, centre, size, material, tile=2.5):
    """An axis-aligned box with box-projected UVs (one repeat per `tile` metres)."""
    cx, cy, cz = centre
    hx, hy, hz = size[0] / 2, size[1] / 2, size[2] / 2
    corners = [(cx + sx * hx, cy + sy * hy, cz + sz * hz) for sx in (-1, 1) for sy in (-1, 1) for sz in (-1, 1)]
    faces = [(0, 1, 3, 2), (4, 6, 7, 5), (0, 4, 5, 1), (2, 3, 7, 6), (0, 2, 6, 4), (1, 5, 7, 3)]
    mesh = bpy.data.meshes.new(name)
    mesh.from_pydata(corners, [], faces)
    uv = mesh.uv_layers.new(name="UVMap")
    for polygon in mesh.polygons:
        normal = polygon.normal
        axis = max(range(3), key=lambda k: abs(normal[k]))
        u_axis, v_axis = [(1, 2), (0, 2), (0, 1)][axis]
        for loop in polygon.loop_indices:
            position = mesh.vertices[mesh.loops[loop].vertex_index].co
            uv.data[loop].uv = (position[u_axis] / tile, position[v_axis] / tile)
    mesh.materials.append(material)
    obj = bpy.data.objects.new(name, mesh)
    bpy.context.scene.collection.objects.link(obj)
    return obj


def prop(asset, location, turn=0.0, scale=1.0):
    """One Poly Haven model under an empty; missing files are reported and skipped."""
    path = os.path.join(HAVEN, "models", asset, asset + ".gltf")
    if not os.path.exists(path):
        print("[lab] missing", asset)
        return None
    before = set(bpy.data.objects)
    bpy.ops.import_scene.gltf(filepath=path)
    imported = [o for o in bpy.data.objects if o not in before]
    root = bpy.data.objects.new("prop_" + asset, None)
    bpy.context.scene.collection.objects.link(root)
    for obj in imported:
        if obj.parent is None:
            obj.parent = root
    root.location = location
    root.rotation_euler = (0.0, 0.0, math.radians(turn))
    root.scale = (scale, scale, scale)
    return root


# ---- the hall ---------------------------------------------------------------------------------------------------
def build():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    floor = textured("lab_floor_polished", "smooth_concrete_floor", (0.16, 0.19, 0.24), 0.12)  # under 0.3: the bench mirrors the lamps in it
    wall = textured("lab_wall", "concrete_wall_008", (0.22, 0.22, 0.24), 0.9)
    iron = textured("lab_corrugated", "corrugated_iron_02", (0.16, 0.17, 0.20), 0.6)
    ceiling = plain("lab_ceiling", (0.05, 0.05, 0.06))
    tube = emissive("lab_tube", (1.0, 0.96, 0.88), 40.0)
    spot = emissive("lab_spot", (1.0, 0.82, 0.55), 25.0)
    screen = emissive("lab_screen", (0.35, 0.65, 1.0), 2.5)
    yellow = plain("lab_marking", (0.75, 0.55, 0.05), 0.5)

    box("floor", (0, 0, -0.05), (HALL_X, HALL_Y, 0.1), floor, tile=3.0)
    box("ceiling", (0, 0, HALL_Z + 0.05), (HALL_X, HALL_Y, 0.1), ceiling)
    box("wall_north", (0, HALL_Y / 2, HALL_Z / 2), (HALL_X, 0.2, HALL_Z), iron, tile=2.0)
    box("wall_south", (0, -HALL_Y / 2, HALL_Z / 2), (HALL_X, 0.2, HALL_Z), wall)
    box("wall_east", (HALL_X / 2, 0, HALL_Z / 2), (0.2, HALL_Y, HALL_Z), wall)
    box("wall_west", (-HALL_X / 2, 0, HALL_Z / 2), (0.2, HALL_Y, HALL_Z), wall)
    for x in (-6.0, 0.0, 6.0):  # pillars behind the character's path, and two floor markings
        box(f"pillar_{x:+.0f}", (x, 4.0, HALL_Z / 2), (0.7, 0.7, HALL_Z), wall, tile=1.5)
    box("marking_a", (0, 1.6, 0.003), (9.0, 0.12, 0.006), yellow)
    box("marking_b", (-4.5, -1.0, 0.003), (0.12, 5.0, 0.006), yellow)

    # Lamp tubes: three rows under the ceiling (bright, thin, against a dark ceiling), two warm spots, screens.
    for row, y in enumerate((-3.0, 2.0, 6.0)):
        for column in range(5):
            x = -8.0 + column * 4.0
            box(f"tube_{row}_{column}", (x, y, HALL_Z - 0.55), (1.4, 0.07, 0.05), tube)
            box(f"tube_housing_{row}_{column}", (x, y, HALL_Z - 0.50), (1.5, 0.16, 0.04), ceiling)
    for index, (x, y) in enumerate(((-7.5, -5.5), (7.5, 5.0))):
        box(f"spot_{index}", (x, y, 2.6), (0.35, 0.35, 0.04), spot)
    for index, (x, y, turn) in enumerate(((3.2, 6.6, 0.0), (4.4, 6.6, 0.0))):
        box(f"screen_{index}", (x, y, 1.25), (0.9, 0.03, 0.55), screen)

    # Clutter. Along the north wall a workbench line, racks to the west, carts and crates around the path.
    prop("metal_office_desk", (3.8, 6.4, 0.0), 180)
    prop("classic_laptop", (2.4, 6.3, 0.76), 170)
    prop("industrial_microscope", (5.3, 6.4, 0.76), 200)
    prop("chemistry_set", (-2.0, 6.5, 0.9), 0)
    prop("circuit_board", (4.6, 6.0, 0.77), 30)
    prop("SchoolChair_01", (3.6, 5.3, 0.0), 20)
    prop("mid_century_lounge_chair", (8.5, -4.5, 0.0), -60)
    for index, x in enumerate((-10.2, -10.2, -10.2)):
        prop("worn_metal_rack", (x, -2.0 + index * 2.2, 0.0), 90)
    prop("drawer_cabinet", (-10.2, 5.2, 0.0), 90)
    prop("metal_tool_chest", (-8.6, 6.6, 0.0), 0)
    prop("metal_toolbox", (-9.9, -1.9, 0.95), 80)
    prop("tool_cart", (-3.2, 2.6, 0.0), 35)
    prop("industrial_storage_cart", (6.2, 1.2, 0.0), -20)
    prop("plastic_crate_01", (1.6, 3.0, 0.0), 10)
    prop("plastic_crate_02", (2.1, 3.1, 0.0), -25)
    prop("plastic_crate_01", (1.8, 3.05, 0.32), 40)
    prop("old_military_crate", (-5.2, -3.2, 0.0), 15)
    prop("portable_generator", (8.8, 3.0, 0.0), -90)
    prop("modular_electric_cables", (7.4, 3.2, 0.0), 0)
    prop("WetFloorSign_01", (-1.4, -2.2, 0.0), 30)
    prop("Drill_01", (-3.1, 2.7, 0.92), 60)
    prop("small_plastic_torch", (-3.4, 2.5, 0.92), 0)
    prop("sledgehammer_01", (-9.6, 3.0, 0.0), 0)
    prop("service_pistol", (4.1, 6.2, 0.77), 45)
    prop("garden_hose_wall_mounted_01", (10.85, -2.0, 1.3), -90)
    prop("korean_fire_extinguisher_01", (-0.45, 4.0, 0.0), 0)      # on the middle pillar: small, saturated, high contrast
    prop("korean_public_payphone_01", (-6.45, 4.0, 0.9), 0)       # the red phone box's stand-in
    prop("rollershutter_door", (9.0, 7.85, 0.0), 180)
    prop("modular_industrial_pipes_01", (-10.7, -6.5, 0.0), 0)
    prop("mounted_fluorescent_lights", (0.0, -7.8, 3.2), 0)
    prop("hanging_industrial_lamp", (-7.5, -5.5, HALL_Z), 0)
    prop("hanging_industrial_lamp", (7.5, 5.0, HALL_Z), 0)

    # The character, appended from the daylight scene (a static mesh plus its anchor).
    source = os.path.join(ASSETS, "bench_scene.blend")
    if os.path.exists(source):
        with bpy.data.libraries.load(source, link=False) as (data_from, data_to):
            data_to.objects = [name for name in data_from.objects if name in ("girl_complete_03", "CharacterAnchor")]
        for obj in data_to.objects:
            if obj is not None:
                bpy.context.scene.collection.objects.link(obj)
                obj.animation_data_clear()
                obj.location = (0.0, 0.0, 0.0)
                obj.rotation_euler.z += math.pi  # her back to the bench's third-person camera (her own tilt kept: the mesh is stored Y-up)
    else:
        print("[lab] bench_scene.blend not found: the scene is exported without a character")

    # The .blend is the scene's owner from now on: it is edited by hand in Blender and exported with
    # export_bench_scene.py, so a rebuild must not silently replace it (pass -- --force to start over).
    if os.path.exists(BLEND) and "--force" not in sys.argv:
        print("[lab] kept the existing", BLEND, "(pass -- --force to rebuild it from this script)")
    else:
        bpy.ops.wm.save_as_mainfile(filepath=BLEND)
        print("[lab] saved", BLEND)
    bpy.ops.export_scene.gltf(filepath=OUT, export_format="GLB", export_apply=True, export_yup=True,
                              export_cameras=False, export_animations=False, export_image_format="AUTO")
    print("[lab] written", OUT, f"{os.path.getsize(OUT) / 1e6:.1f} MB")


build()
