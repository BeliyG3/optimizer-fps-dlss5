"""Re-export facecap_notex.glb with the original face texture restored.

The upstream three.js asset (facecap.glb) carries one material, ``lambert5``,
shared by every mesh (skin, teeth, eyes).  Its base colour texture is a KTX2 /
Basis Universal image, which Blender cannot import.  ``facecap_notex.glb`` is
that file with the image stripped; ``facecap_texture.png`` is the same image
transcoded to plain 8-bit PNG with the Khronos ``ktx`` CLI.

This script glues the two back together: import the untextured GLB, hook the
PNG into the Base Color of every material, pack it, and export a self-contained
``facecap_plain.glb`` for the bench.

UV note
-------
The original material used ``KHR_texture_transform``::

    offset = (0.00724834995, 0.0100790262)
    scale  = (0.000238723238, 0.000239208952)

i.e. the mesh UVs are stored in *pixel* space (0..4095), not 0..1.  Stripping
the texture also stripped the transform, so the imported UVs are unusable as
they stand.  Rather than depend on an extension the consumer may not implement,
the transform is baked straight into the UV loop data here, which makes the
exported GLB plain 0..1 UVs with no extension at all.

Blender flips V on glTF import (``uv_blender.y = 1 - uv_gltf.y``), so the baked
mapping in Blender space is::

    u' = u * sx + ox
    v' = v * sy + (1 - sy - oy)

Watertightness
--------------
The scan is an open shell: the neck is cut off at the bottom and the mouth
cavity has no back wall, so when the jaw morphs open you look straight through
the head at the sky.  Every boundary edge loop of every mesh is therefore
filled with ``bmesh.ops.holes_fill`` and the new faces are given a separate,
dark, untextured ``mouth_interior`` material.  The fill only adds faces (no new
vertices), which keeps the 52 ARKit shape keys on ``Mesh_2`` valid; the shape
key coordinates are snapshotted before the edit and restored afterwards as a
belt-and-braces measure.

Skin tint
---------
``facecap_texture.png`` is a monochrome scan (mean RGB ~166/166/166, zero
saturation), which renders as a grey face.  The glTF exporter only writes a
plain Image Texture plugged into Base Color, so a Mix node would be dropped on
export -- the tint is baked into the pixels instead: sRGB -> linear, multiply
by ``SKIN_TINT``, lift by ``SKIN_GAIN``, linear -> sRGB, into a new packed
image ``facecap_skin``.

Run headless:
    blender.exe --background --factory-startup \
        --python tools/bench/blender/export_facecap.py
"""

import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))  # blender --python does not add the script folder

import bpy

from export_log import make_log
from facecap_holes import fill_holes
from facecap_material import METALLIC, ROUGHNESS, apply_texture, make_cavity_material
from facecap_texture import UV_OFFSET, UV_SCALE, bake_uv_transform, make_tinted_image

HERE = os.path.dirname(os.path.abspath(__file__))
BENCH = os.path.dirname(HERE)
ASSETS = os.path.join(BENCH, "assets", "external")
SRC = os.path.join(ASSETS, "facecap_notex.glb")
TEX = os.path.join(ASSETS, "facecap_texture.png")
DST = os.path.join(ASSETS, "facecap_plain.glb")

# Objects whose names look like eyes or teeth keep the untinted texture.
UNTINTED_HINTS = ("teeth", "tooth", "eye", "cornea", "iris", "sclera")

log = make_log("[facecap]")


def main():
    bpy.ops.wm.read_homefile(use_empty=True)

    if not os.path.isfile(SRC):
        sys.exit("source not found: %s" % SRC)
    if not os.path.isfile(TEX):
        sys.exit("texture not found: %s" % TEX)

    log("importing", SRC)
    bpy.ops.import_scene.gltf(filepath=SRC)

    # ---- inventory -------------------------------------------------------
    log("--- scene inventory ---")
    for obj in sorted(bpy.data.objects, key=lambda o: o.name):
        parent = obj.parent.name if obj.parent else "-"
        if obj.type == "MESH":
            sk = obj.data.shape_keys
            nmorph = (len(sk.key_blocks) - 1) if sk else 0
            mats = [m.name if m else "<none>" for m in obj.data.materials]
            log(
                "MESH  %-24s verts=%-6d morphs=%-3d parent=%-16s mats=%s"
                % (obj.name, len(obj.data.vertices), nmorph, parent, mats)
            )
        else:
            log("%-5s %-24s parent=%s" % (obj.type, obj.name, parent))
    log("materials in file: %s" % [m.name for m in bpy.data.materials])
    log("actions in file: %s" % [a.name for a in bpy.data.actions])

    base_materials = list(bpy.data.materials)

    # ---- close the shell -------------------------------------------------
    log("--- filling open boundaries ---")
    cavity_mat = make_cavity_material()
    total_holes = 0
    total_faces = 0
    for obj in sorted((o for o in bpy.data.objects if o.type == "MESH"),
                      key=lambda o: o.name):
        holes, added = fill_holes(obj, cavity_mat)
        total_holes += holes
        total_faces += added
    log("total: %d holes filled, %d faces added" % (total_holes, total_faces))
    # drop the slot again where nothing used it
    for obj in (o for o in bpy.data.objects if o.type == "MESH"):
        me = obj.data
        if cavity_mat.name in me.materials:
            idx = list(me.materials).index(cavity_mat)
            used = any(p.material_index == idx for p in me.polygons)
            if not used:
                me.materials.pop(index=idx)
                log("mesh %s: no fill faces, cavity slot removed" % obj.name)

    # ---- UVs -------------------------------------------------------------
    log("--- baking KHR_texture_transform into UVs ---")
    log("offset=%s scale=%s" % (UV_OFFSET, UV_SCALE))
    for mesh in bpy.data.meshes:
        if not mesh.uv_layers:
            log("WARNING: mesh %s has no UV layer" % mesh.name)
            continue
        bake_uv_transform(mesh)
        layer = mesh.uv_layers[0]
        us = [d.uv[0] for d in layer.data]
        vs = [d.uv[1] for d in layer.data]
        log(
            "mesh %-24s uv u=[%.4f, %.4f] v=[%.4f, %.4f]"
            % (mesh.name, min(us), max(us), min(vs), max(vs))
        )

    # ---- texture ---------------------------------------------------------
    log("--- applying texture ---")
    img = bpy.data.images.load(TEX, check_existing=True)
    img.name = "facecap_texture"
    log("loaded image %s %dx%d" % (TEX, img.size[0], img.size[1]))

    skin = make_tinted_image(img, "facecap_skin")

    # One material (`lambert5`) is shared by every mesh and the texture covers
    # all of them - skin, teeth and eyes.  Tint it; if the scan happens to keep
    # eyes/teeth as separate objects, hand those a clone using the raw scan.
    if not base_materials:
        sys.exit("no materials in the imported file")
    for mat in base_materials:
        apply_texture(mat, skin)
        log(
            "material %-20s base_color=<facecap_skin> metallic=%.2f roughness=%.2f"
            % (mat.name, METALLIC, ROUGHNESS)
        )

    used_untinted = False
    for obj in (o for o in bpy.data.objects if o.type == "MESH"):
        low = obj.name.lower()
        if not any(h in low for h in UNTINTED_HINTS):
            continue
        me = obj.data
        for i, m in enumerate(me.materials):
            if m is None or m is cavity_mat:
                continue
            clone_name = m.name + "_raw"
            clone = bpy.data.materials.get(clone_name)
            if clone is None:
                clone = m.copy()
                clone.name = clone_name
                apply_texture(clone, img)
            me.materials[i] = clone
            used_untinted = True
            log("object %s slot %d -> %s (untinted scan)" % (obj.name, i, clone_name))

    img.pack()
    log("packed scan image, %d bytes in-file" % len(img.packed_file.data))
    log("packed skin image, %d bytes in-file" % len(skin.packed_file.data))
    if not used_untinted:
        # nothing references the raw scan any more; keep the file to one image
        img.user_clear()
        log("no separate teeth/eye objects; only facecap_skin is exported")

    log("materials at export: %s" % [m.name for m in bpy.data.materials])
    for obj in sorted((o for o in bpy.data.objects if o.type == "MESH"),
                      key=lambda o: o.name):
        log(
            "object %-20s mats=%s"
            % (obj.name, [m.name if m else "<none>" for m in obj.data.materials])
        )

    # ---- export ----------------------------------------------------------
    os.makedirs(os.path.dirname(DST), exist_ok=True)
    log("exporting", DST)
    bpy.ops.export_scene.gltf(
        filepath=DST,
        export_format="GLB",
        export_animations=True,
        export_morph=True,
        export_morph_normal=False,
        export_skins=True,
        export_apply=False,
        export_yup=True,
        export_image_format="AUTO",
    )
    log("done, %d bytes" % os.path.getsize(DST))


if __name__ == "__main__":
    main()
