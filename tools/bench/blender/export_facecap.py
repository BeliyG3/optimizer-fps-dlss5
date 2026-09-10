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

import bmesh
import bpy
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
BENCH = os.path.dirname(HERE)
ASSETS = os.path.join(BENCH, "assets", "external")
SRC = os.path.join(ASSETS, "facecap_notex.glb")
TEX = os.path.join(ASSETS, "facecap_texture.png")
DST = os.path.join(ASSETS, "facecap_plain.glb")

# KHR_texture_transform of the original `lambert5` material.
UV_OFFSET = (0.00724834995, 0.0100790262)
UV_SCALE = (0.000238723238, 0.000239208952)

METALLIC = 0.0
ROUGHNESS = 0.6

# Linear-space multiplier that turns the grey scan into skin, plus a small
# overall lift so the tinted result does not read darker than the original.
SKIN_TINT = (1.0, 0.78, 0.68)
SKIN_GAIN = 1.08

# Material for the generated hole-fill geometry (mouth cavity / neck cap).
CAVITY_MAT = "mouth_interior"
CAVITY_COLOR = (0.10, 0.04, 0.04, 1.0)
CAVITY_ROUGHNESS = 0.8

# Objects whose names look like eyes or teeth keep the untinted texture.
UNTINTED_HINTS = ("teeth", "tooth", "eye", "cornea", "iris", "sclera")


def log(*a):
    print("[facecap]", *a)


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


def bake_uv_transform(mesh):
    """Apply the original KHR_texture_transform to the mesh's UV loops."""
    sx, sy = UV_SCALE
    ox, oy = UV_OFFSET
    v_bias = 1.0 - sy - oy
    for layer in mesh.uv_layers:
        for datum in layer.data:
            u, v = datum.uv
            datum.uv = (u * sx + ox, v * sy + v_bias)


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


# ---------------------------------------------------------------------------
# hole filling
# ---------------------------------------------------------------------------


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


def count_boundary_loops(edges):
    """Number of connected components in the boundary-edge graph."""
    remaining = set(edges)
    loops = 0
    while remaining:
        seed = remaining.pop()
        loops += 1
        stack = [seed]
        while stack:
            e = stack.pop()
            for v in e.verts:
                for ne in v.link_edges:
                    if ne in remaining:
                        remaining.discard(ne)
                        stack.append(ne)
    return loops


def snapshot_shape_keys(me):
    """Return [(name, value, slider_min, slider_max, relative_key, coords)]."""
    sk = me.shape_keys
    if not sk:
        return None
    out = []
    n = len(me.vertices)
    for kb in sk.key_blocks:
        arr = np.empty(n * 3, dtype=np.float32)
        kb.data.foreach_get("co", arr)
        out.append(
            {
                "name": kb.name,
                "value": kb.value,
                "min": kb.slider_min,
                "max": kb.slider_max,
                "relative_key": kb.relative_key.name if kb.relative_key else None,
                "co": arr,
            }
        )
    return out


def restore_shape_keys(obj, snap):
    """Rebuild shape keys on `obj` from a snapshot (only if Blender lost them)."""
    me = obj.data
    for entry in snap:
        kb = obj.shape_key_add(name=entry["name"], from_mix=False)
        kb.data.foreach_set("co", entry["co"])
    sk = me.shape_keys
    for entry in snap:
        kb = sk.key_blocks[entry["name"]]
        kb.value = entry["value"]
        kb.slider_min = entry["min"]
        kb.slider_max = entry["max"]
    for entry in snap:
        if entry["relative_key"]:
            sk.key_blocks[entry["name"]].relative_key = sk.key_blocks[
                entry["relative_key"]
            ]


def fill_holes(obj, cavity_mat):
    """Cap every open boundary loop of `obj`.  Returns (holes, faces_added)."""
    me = obj.data
    n_verts_before = len(me.vertices)
    n_polys_before = len(me.polygons)
    snap = snapshot_shape_keys(me)
    n_keys_before = len(snap) if snap else 0

    # material slot for the generated faces
    slot = -1
    for i, m in enumerate(me.materials):
        if m is cavity_mat:
            slot = i
            break
    if slot < 0:
        me.materials.append(cavity_mat)
        slot = len(me.materials) - 1

    bm = bmesh.new()
    bm.from_mesh(me)
    bm.verts.ensure_lookup_table()
    bm.edges.ensure_lookup_table()

    boundary = [e for e in bm.edges if e.is_boundary]
    if not boundary:
        bm.free()
        log("mesh %-20s no boundary edges, already closed" % obj.name)
        return 0, 0

    holes = count_boundary_loops(boundary)
    v_in_bm = len(bm.verts)

    new_faces = []
    try:
        res = bmesh.ops.holes_fill(bm, edges=boundary, sides=0)
        new_faces = [f for f in res.get("faces", []) if f.is_valid]
    except Exception as exc:  # pragma: no cover - fallback path
        log("holes_fill failed on %s (%s), trying edgenet_fill" % (obj.name, exc))
        res = bmesh.ops.edgenet_fill(bm, edges=boundary)
        new_faces = [f for f in res.get("faces", []) if f.is_valid]
    if not new_faces:
        log("holes_fill produced nothing on %s, trying triangle_fill" % obj.name)
        res = bmesh.ops.triangle_fill(bm, edges=boundary, use_beauty=True)
        new_faces = [f for f in res.get("geom", []) if isinstance(f, bmesh.types.BMFace)]

    if new_faces:
        tri = bmesh.ops.triangulate(bm, faces=new_faces)
        new_faces = [f for f in tri.get("faces", new_faces) if f.is_valid]
        for f in new_faces:
            f.material_index = slot
            f.smooth = False

    added = len(new_faces)
    v_in_bm_after = len(bm.verts)
    if v_in_bm_after != v_in_bm:
        log(
            "WARNING: %s gained %d verts during fill (shape keys will be padded)"
            % (obj.name, v_in_bm_after - v_in_bm)
        )

    bm.to_mesh(me)
    bm.free()
    me.update()

    n_verts_after = len(me.vertices)
    sk = me.shape_keys
    n_keys_after = len(sk.key_blocks) if sk else 0

    if snap and n_verts_after == n_verts_before:
        if n_keys_after != n_keys_before:
            log(
                "shape keys lost on %s (%d -> %d), restoring from snapshot"
                % (obj.name, n_keys_before, n_keys_after)
            )
            restore_shape_keys(obj, snap)
        else:
            # re-stamp the coordinates; to_mesh can shuffle them
            for entry in snap:
                kb = me.shape_keys.key_blocks.get(entry["name"])
                if kb is not None:
                    kb.data.foreach_set("co", entry["co"])
    elif snap:
        log(
            "WARNING: vertex count changed on %s (%d -> %d); shape keys NOT restamped"
            % (obj.name, n_verts_before, n_verts_after)
        )

    sk = me.shape_keys
    log(
        "mesh %-20s holes=%-3d faces_added=%-6d polys %d->%d verts %d->%d keys %d->%d"
        % (
            obj.name,
            holes,
            added,
            n_polys_before,
            len(me.polygons),
            n_verts_before,
            n_verts_after,
            n_keys_before,
            len(sk.key_blocks) if sk else 0,
        )
    )
    return holes, added


# ---------------------------------------------------------------------------
# skin tint
# ---------------------------------------------------------------------------


def srgb_to_linear(c):
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def linear_to_srgb(c):
    return np.where(c <= 0.0031308, c * 12.92, 1.055 * (c ** (1.0 / 2.4)) - 0.055)


def make_tinted_image(src, name):
    """Return a new packed image = `src` multiplied by SKIN_TINT in linear space."""
    w, h = src.size
    ch = src.channels
    buf = np.empty(w * h * ch, dtype=np.float32)
    src.pixels.foreach_get(buf)
    buf = buf.reshape(-1, ch)

    rgb = buf[:, :3].astype(np.float64)
    mean_before = rgb.mean(axis=0)

    lin = srgb_to_linear(rgb)
    lin *= np.array(SKIN_TINT, dtype=np.float64)
    lin *= SKIN_GAIN
    np.clip(lin, 0.0, 1.0, out=lin)
    out = linear_to_srgb(lin)
    np.clip(out, 0.0, 1.0, out=out)
    mean_after = out.mean(axis=0)

    buf[:, :3] = out.astype(np.float32)

    dst = bpy.data.images.new(name, width=w, height=h, alpha=(ch == 4))
    dst.colorspace_settings.name = src.colorspace_settings.name
    dst.file_format = "PNG"
    dst.pixels.foreach_set(buf.reshape(-1))
    dst.update()
    dst.pack()

    log(
        "tint %s: %dx%d ch=%d mean sRGB %s -> %s (tint=%s gain=%.2f)"
        % (
            name,
            w,
            h,
            ch,
            np.round(mean_before * 255.0, 1).tolist(),
            np.round(mean_after * 255.0, 1).tolist(),
            SKIN_TINT,
            SKIN_GAIN,
        )
    )
    return dst


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
