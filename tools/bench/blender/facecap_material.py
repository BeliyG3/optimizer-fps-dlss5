"""Facecap materials: Principled BSDF lookup, base-colour texture hook-up and the mouth-cavity material."""

import bpy

METALLIC = 0.0
ROUGHNESS = 0.6

# Material for the generated hole-fill geometry (mouth cavity / neck cap).
CAVITY_MAT = "mouth_interior"
CAVITY_COLOR = (0.10, 0.04, 0.04, 1.0)
CAVITY_ROUGHNESS = 0.8


def principled(mat):
    """Return the material's Principled BSDF node, creating one if needed."""
    if not mat.use_nodes:
        mat.use_nodes = True
    nt = mat.node_tree
    for node in nt.nodes:
        if node.type == "BSDF_PRINCIPLED":
            return node
    node = nt.nodes.new("ShaderNodeBsdfPrincipled")
    out = next((n for n in nt.nodes if n.type == "OUTPUT_MATERIAL"), None)
    if out is None:
        out = nt.nodes.new("ShaderNodeOutputMaterial")
    nt.links.new(node.outputs[0], out.inputs["Surface"])
    return node


def apply_texture(mat, img):
    """Link `img` into the material's Base Color and set the PBR constants."""
    node = principled(mat)
    nt = mat.node_tree

    # drop whatever was feeding Base Color before
    for link in [l for l in nt.links if l.to_socket is node.inputs["Base Color"]]:
        nt.links.remove(link)
    for old in [n for n in nt.nodes if n.type == "TEX_IMAGE"]:
        nt.nodes.remove(old)

    tex = nt.nodes.new("ShaderNodeTexImage")
    tex.image = img
    tex.label = "facecap base color"
    tex.location = (node.location.x - 400, node.location.y)
    nt.links.new(tex.outputs["Color"], node.inputs["Base Color"])

    node.inputs["Base Color"].default_value = (1.0, 1.0, 1.0, 1.0)
    if "Metallic" in node.inputs:
        node.inputs["Metallic"].default_value = METALLIC
    if "Roughness" in node.inputs:
        node.inputs["Roughness"].default_value = ROUGHNESS
    return tex


def make_cavity_material():
    mat = bpy.data.materials.new(CAVITY_MAT)
    mat.use_nodes = True
    node = principled(mat)
    node.inputs["Base Color"].default_value = CAVITY_COLOR
    if "Metallic" in node.inputs:
        node.inputs["Metallic"].default_value = 0.0
    if "Roughness" in node.inputs:
        node.inputs["Roughness"].default_value = CAVITY_ROUGHNESS
    mat.diffuse_color = CAVITY_COLOR
    mat.roughness = CAVITY_ROUGHNESS
    # doubleSided in glTF -- the cap must occlude from either side
    mat.use_backface_culling = False
    return mat
