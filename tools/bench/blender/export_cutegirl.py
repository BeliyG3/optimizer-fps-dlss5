r"""Repack the Sketchfab "CuteGirl G1" head (gltf/scene.gltf) as a single self-contained
``cutegirl.glb`` for the bench: skin (102 joints), the 12.1 s "Emote" action (bone TRS +
morph weights), 152 morph targets on the head mesh, textures embedded.

Nothing is baked -- the armature and the shape keys survive the round trip so the bench
loader does the skinning / morphing itself.

On top of the imported "Emote" this script APPENDS a deliberate facial test segment at
13.0 s - 19.0 s (see ``build_face_test``), so the exported clip is 19.0 s long and the
last 6 s exercise the morph-target path hard (the mouth flapping wide open, then cheek
puffs, then the lips pushed forward into a tube) while the skeleton simply holds the
pose the Emote left it in.

Run headless:
    "C:\Program Files\Blender Foundation\Blender 5.2\blender.exe" --background \
        --factory-startup --python tools/bench/blender/export_cutegirl.py

Add ``-- --check`` to also write Workbench check renders next to the GLB:
    ... --python tools/bench/blender/export_cutegirl.py -- --check
"""

import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))  # blender --python does not add the script folder

import bpy

from cutegirl_asset import DST, SRC
from cutegirl_check_render import check_render
from cutegirl_face_test import build_face_test
from cutegirl_measure import morph_meshes
from cutegirl_report import report_keys
from export_log import make_log

log = make_log("[cutegirl]")


def main():
    do_check = "--check" in sys.argv
    bpy.ops.wm.read_homefile(use_empty=True)
    if not os.path.isfile(SRC):
        sys.exit("source not found: %s" % SRC)

    log("importing", SRC)
    bpy.ops.import_scene.gltf(filepath=SRC, import_shading="NORMALS")

    log("--- scene inventory ---")
    for obj in sorted(bpy.data.objects, key=lambda o: o.name):
        parent = obj.parent.name if obj.parent else "-"
        if obj.type == "MESH":
            sk = obj.data.shape_keys
            nmorph = (len(sk.key_blocks) - 1) if sk else 0
            mats = [(m.name, m.blend_method if hasattr(m, "blend_method") else "?")
                    for m in obj.data.materials if m]
            vg = len(obj.vertex_groups)
            mods = [m.type for m in obj.modifiers]
            log("MESH  %-34s verts=%-6d morphs=%-4d groups=%-4d parent=%-16s mods=%s mats=%s"
                % (obj.name, len(obj.data.vertices), nmorph, vg, parent, mods, mats))
        elif obj.type == "ARMATURE":
            log("ARMA  %-34s bones=%-4d parent=%s" % (obj.name, len(obj.data.bones), parent))
            names = [b.name for b in obj.data.bones]
            log("      face bones: %s" % [n for n in names if any(
                k in n.lower() for k in ("head", "jaw", "neck", "eye", "tongue", "teeth"))])
        else:
            log("%-5s %-34s parent=%s" % (obj.type, obj.name, parent))
    log("actions: %s" % [a.name for a in bpy.data.actions])
    log("images:  %s" % [(i.name, tuple(i.size)) for i in bpy.data.images])

    sc = bpy.context.scene
    fps = int(round(sc.render.fps / sc.render.fps_base))
    emote = bpy.data.actions.get("Emote")
    log("fps=%d  Emote frame range %s (%.3f s)"
        % (fps, tuple(emote.frame_range), emote.frame_range[1] / fps))

    meshes = morph_meshes()
    report_keys(meshes)
    end = build_face_test(meshes, fps)

    sc.frame_start = 0
    sc.frame_end = end
    log("scene frame range 0..%d (%.3f s); Emote action now %s"
        % (end, end / fps, tuple(emote.frame_range)))

    os.makedirs(os.path.dirname(DST), exist_ok=True)
    log("exporting", DST)
    bpy.ops.export_scene.gltf(
        filepath=DST,
        export_format="GLB",
        export_animations=True,
        export_skins=True,
        export_morph=True,
        export_morph_normal=False,
        export_apply=False,
        export_yup=True,
        export_image_format="AUTO",
        export_materials="EXPORT",
        export_texcoords=True,
        export_normals=True,
        export_optimize_animation_size=False,
        export_frame_range=False,
    )
    log("done, %d bytes" % os.path.getsize(DST))

    if do_check:
        log("--- check renders ---")
        # (seconds, frame width as a fraction of the head height): the mouth alone for the
        # flap peak / trough and the pucker peak / release, the whole lower face for the
        # puff baseline / peak
        check_render([(int(round(t * fps)), w) for t, w in
                      ((13.25, 0.25), (13.50, 0.25), (15.00, 0.45), (15.50, 0.45),
                       (17.50, 0.25), (18.00, 0.25))],
                     meshes)


if __name__ == "__main__":
    main()
