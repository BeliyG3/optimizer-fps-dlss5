# Exports tools/bench/assets/bench_scene.blend to bench_scene.glb for pw_bench (--gltf), in background Blender:
#
#   "C:\Program Files\Blender Foundation\Blender 5.2\blender.exe" --background --factory-startup ^
#       tools\bench\assets\bench_scene.blend --python tools\bench\blender\export_bench_scene.py -- tools\bench\assets\bench_scene.glb
#
# The glTF exporter only carries a base colour that is a plain Image Texture (times a constant). The
# scene tints the dress in the shader: Base Color <- Mix (Color blend, factor 1) of A = Math MULTIPLY
# (texture, k) and B = a constant colour. Cycles baking in background Blender produced black images
# here, so that pattern is evaluated in numpy instead (Blender's Color blend: hue and saturation of B,
# HSV value of A) and the result is packed as a PNG wired straight to Base Color before the export.
# The .blend on disk is not modified. Other materials export as they are.
import bpy, sys, os
import numpy as np

out = sys.argv[sys.argv.index("--") + 1] if "--" in sys.argv else os.path.splitext(bpy.data.filepath)[0] + ".glb"


def srgb_to_linear(c):
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def linear_to_srgb(c):
    return np.where(c <= 0.0031308, c * 12.92, 1.055 * np.power(np.maximum(c, 0.0), 1 / 2.4) - 0.055)


def image_linear_rgb(img):
    # Blender gives byte images as sRGB-encoded floats; the shader works on linear values.
    w, h = img.size
    px = np.array(img.pixels[:], dtype=np.float32).reshape(h, w, 4)[..., :3]
    return srgb_to_linear(px) if img.colorspace_settings.name == "sRGB" else px


def tinted_base_color(mat):
    """Returns (linear rgb array, source image) for the Mix-Color-of-Math(texture) pattern, else None."""
    nt = mat.node_tree
    if not nt: return None
    outs = [n for n in nt.nodes if n.type == "OUTPUT_MATERIAL" and n.inputs["Surface"].is_linked]
    if not outs: return None
    bsdf = outs[0].inputs["Surface"].links[0].from_node
    if "Base Color" not in bsdf.inputs or not bsdf.inputs["Base Color"].is_linked: return None
    mix = bsdf.inputs["Base Color"].links[0].from_node
    if mix.type != "MIX" or mix.data_type != "RGBA" or mix.blend_type != "COLOR": return None
    a_in = [i for i in mix.inputs if i.name == "A" and i.type == "RGBA"][0]
    b_in = [i for i in mix.inputs if i.name == "B" and i.type == "RGBA"][0]
    fac = mix.inputs[0].default_value if not mix.inputs[0].is_linked else 1.0
    if not a_in.is_linked or b_in.is_linked: return None
    math = a_in.links[0].from_node
    if math.type != "MATH" or math.operation != "MULTIPLY": return None
    tex_node, k = None, 1.0
    for i in math.inputs[:2]:
        if i.is_linked and i.links[0].from_node.type == "TEX_IMAGE": tex_node = i.links[0].from_node
        elif not i.is_linked: k = float(i.default_value)
    if tex_node is None or tex_node.image is None: return None
    A = image_linear_rgb(tex_node.image) * k                      # Math MULTIPLY (a greyscale value in the node graph: the value of the colour)
    V = np.clip(A.max(axis=2), 0.0, 1.0)                           # HSV value of A
    B = np.array(b_in.default_value[:3], dtype=np.float64)
    s_B = 0.0 if B.max() <= 0.0 else 1.0 - B.min() / B.max()
    # hsv_to_rgb(hue_B, sat_B, V): B scaled so that its maximum channel is 1, times V
    tint = B / B.max() if B.max() > 0.0 else np.ones(3)
    result = V[..., None] * tint
    if fac < 1.0: result = (1.0 - fac) * np.clip(A, 0, 1) + fac * result
    return result, tex_node.image


for mat in bpy.data.materials:
    t = tinted_base_color(mat)
    if t is None: continue
    rgb, src = t
    h, w = rgb.shape[:2]
    img = bpy.data.images.new(f"{mat.name}_basecolor_baked", w, h, alpha=False)
    srgb = np.clip(linear_to_srgb(rgb), 0.0, 1.0)
    px = np.concatenate([srgb, np.ones((h, w, 1))], axis=2).astype(np.float32)
    img.pixels.foreach_set(px.ravel())
    img.pack()
    nt = mat.node_tree
    node = nt.nodes.new("ShaderNodeTexImage"); node.image = img
    bsdf = [n for n in nt.nodes if n.type == "OUTPUT_MATERIAL"][0].inputs["Surface"].links[0].from_node
    for l in list(bsdf.inputs["Base Color"].links): nt.links.remove(l)
    nt.links.new(node.outputs["Color"], bsdf.inputs["Base Color"])
    print(f"[export] {mat.name}: base colour composed from {src.name} ({w}x{h}), mean value {rgb.max(axis=2).mean():.3f}")

bpy.ops.export_scene.gltf(filepath=out, export_format="GLB", export_animations=True, export_apply=True,
                          export_cameras=True, export_yup=True, export_image_format="AUTO")
print("[export] written", out)
