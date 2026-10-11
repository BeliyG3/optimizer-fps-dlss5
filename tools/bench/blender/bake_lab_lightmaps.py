# Bakes the lab's lighting into lightmaps for pw_bench12's raster mode, in background Blender (Cycles, OptiX):
#
#   "C:\Program Files\Blender Foundation\Blender 5.2\blender.exe" --background --factory-startup --python-exit-code 1 ^
#       tools\bench\assets\lab_scene.blend --python tools\bench\blender\bake_lab_lightmaps.py -- --out tools\bench\assets
#
# Writes to --out (default: the folder of the .blend):
#   lab_scene_baked.glb  the export of export_bench_scene.py (characters included) plus the Lightmap UV map as TEXCOORD_1
#                        on every static mesh and mesh extras.lightmapAtlas (0); static meshes are single-user;
#   lab_lightmap_{full,sun,lamp,rest}_0.hdr  Radiance, --size squared, DIFFUSE without colour: direct and indirect
#                        light of every light, direct sun only, the lamps' direct light only, and full - sun - lamp as
#                        a map of its own (lab_lightmap_denoise.py; the *_noisy.hdr files are the three bakes, kept);
#   lab_lightmap.json    atlases, sun, sky, scales, samples, texel density, bake time, denoised.
# Order: UVs, then the export and its check against lab_scene.glb (a stale reference fails before any bake), the JSON,
# the bakes (tiled, --tile) and the denoise, the JSON again. --no-bake does everything but the bakes and the denoise.
# --denoise-only (no .blend needed) makes the final maps again from the *_noisy.hdr files in --out and updates
# "denoised" in the JSON. The .blend on disk is not modified, and the owner's interactive Blender is never involved.
# The outdoor lab (build_lab_outdoor.py) bakes the same way with its own names:
#   ... tools\bench\assets\lab_scene_outdoor.blend --python ... -- --out tools\bench\assets ^
#       --prefix lab_outdoor_lightmap --glb-name lab_scene_outdoor_baked.glb
# (the maps are <prefix>_{full,sun,lamp,rest}_0.hdr, the JSON <prefix>.json; the reference export is the .blend's .glb).
# The garden (build_lab_garden.py) bakes under the prepared HDRI sky (tools/bench/hdri_prepare.py, the bench's --hdri):
#   ... tools\bench\assets\lab_scene_garden.blend --python ... -- --out tools\bench\assets ^
#       --prefix lab_garden_lightmap --glb-name lab_scene_garden_baked.glb ^
#       --hdri tools\bench\assets\autumn_field_puresky_nosun.hdr --scale-full 1.3 --scale-sun 1.0
# Foliage and props (nodes named veg_* and prop_*) are never baked: no Lightmap UVs, hidden from render in every bake,
# so the maps hold none of their shadows (bench12 lights and shadows them dynamically).
# The sun and sky are the bench's lab command (run_lab.bat).
import argparse
import json
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import bpy

import lab_lightmap_bake as cycles_bake
import lab_lightmap_denoise as denoise
import lab_lightmap_lighting as lighting
import lab_lightmap_proxy as proxies
import lab_lightmap_uv as uvs
from character_export import append_characters, validate_glb
from lab_lightmap_check import check_structure, check_uvs, compare_geometry, read_glb
from scene_export import compose_tinted_base_colors, export_glb

SCENES = ('lab_scene.blend', 'lab_scene_outdoor.blend', 'lab_scene_garden.blend')  # characters: lab_characters.blend
BENCH_SKY = (0.12, 0.16, 0.23)  # bench12 scene.cpp: the 1x1 environment the path tracer's Sky() returns without --hdri


def parse_args():
    def vector(text):
        return tuple(float(v) for v in text.split(','))
    parser = argparse.ArgumentParser(prog="bake_lab_lightmaps.py")
    parser.add_argument('--out', type=Path)
    parser.add_argument('--samples', type=int, default=512)
    parser.add_argument('--size', type=int, default=8192)
    parser.add_argument('--tile', type=int, default=2048, help="bake the atlas in tiles of this size (VRAM); 0 = whole")
    parser.add_argument('--margin', type=int, default=4, help="texels between UV islands and of bake dilation")
    parser.add_argument('--bounces', type=int, default=4, help="the bench's --bounces")
    parser.add_argument('--sun-dir', type=vector, default=(0.45, -0.77, 0.45), help="travel, bench space (glTF z negated)")
    parser.add_argument('--sun-strength', type=float, default=1500.0)
    parser.add_argument('--sun-angle', type=float, default=0.5, help="angular diameter, degrees")
    parser.add_argument('--sky', type=vector, default=BENCH_SKY)
    parser.add_argument('--hdri', type=Path, help="the World is this panorama (the bench's --hdri) instead of --sky")
    parser.add_argument('--scale-full', type=float, default=1.0, help="the JSON's scaleFull (calibration, 1 = none)")
    parser.add_argument('--scale-sun', type=float, default=1.0, help="the JSON's scaleSun")
    parser.add_argument('--prefix', default=denoise.PREFIX, help="file names of the maps (<prefix>_full_0.hdr ...) and the JSON")
    parser.add_argument('--glb-name', default='lab_scene_baked.glb', help="the baked export's file name in --out")
    parser.add_argument('--no-bake', action='store_true')
    parser.add_argument('--no-denoise', action='store_true', help="keep the raw bakes (then bake at twice the samples)")
    parser.add_argument('--denoise-only', action='store_true', help="final maps from the *_noisy.hdr files in --out")
    return parser.parse_args(sys.argv[sys.argv.index('--') + 1:] if '--' in sys.argv else [])


def prepare(scene):
    rigs = append_characters()
    texts = uvs.convert_texts(scene)
    static, dynamic, unbaked = uvs.classify(scene)
    problems = uvs.unsupported_geometry(static)
    if problems:
        raise RuntimeError("static objects whose export differs from their mesh data (the proxy would not match):\n  "
                           + "\n  ".join(problems))
    copies = uvs.make_single_user(static, dynamic)
    placeholders = uvs.add_layers(static)
    print(f"[lightmap] {len(static)} static and {len(dynamic)} dynamic mesh objects, {len(unbaked)} foliage and props (veg_, "
          f"prop_: not baked, hidden in every bake); no static modifiers or active shape "
          f"keys; texts made meshes {texts}; {copies} mesh copies for single users; {placeholders} meshes got a constant "
          f"TEXCOORD_0", flush=True)
    return rigs, static, dynamic


def make_uvs(scene, static, size, margin):
    """The world-space copy with packed Lightmap UVs, copied back to the scene and checked. Returns (copy, UV stats)."""
    texture_name = uvs.texture_layer_name(static)
    named, mismatches = uvs.uv_name_mismatches(static, texture_name)
    print(f"[lightmap] proxy texture UV map '{texture_name}'; {named} material nodes name a UV map, "
          f"{len(mismatches)} resolve differently on the proxy", flush=True)
    for line in mismatches:
        print(f"[lightmap] WARNING UV name: {line}", flush=True)
    proxy = proxies.build(static, scene, texture_name)
    start = time.perf_counter()
    proxies.pack(proxy, size, margin)
    print(f"[lightmap] Smart UV Project and pack: {time.perf_counter() - start:.1f} s", flush=True)
    uv_stats = proxies.stats(proxy, size)
    proxies.copy_uvs_back(proxy, static)
    proxies.check(proxy, static)
    return proxy, uv_stats


def light_and_bake(scene, proxy, dynamic, args, out):
    """Sun, sky and render settings; the bakes and the final maps unless --no-bake; then the scene is as before.
    Returns (seconds per bake, denoised)."""
    lighting.render_settings(scene, args.samples, args.bounces)
    sun = lighting.add_sun(scene, args.sun_dir, args.sun_strength, args.sun_angle)
    previous_world = lighting.set_environment(scene, args.hdri) if args.hdri else lighting.set_sky(scene, args.sky)
    seconds, denoised = {}, False
    try:
        cycles_bake.preflight(scene, proxy, sun, dynamic)
        if not args.no_bake:
            lighting.enable_optix(scene)
            seconds = cycles_bake.bake_maps(scene, proxy, sun, dynamic, args.size, args.tile, args.margin, out, args.prefix)
            if args.no_denoise:
                denoise.keep_noisy(out, args.prefix)
            else:
                denoised = denoise.finish(out, args.prefix)
    finally:
        light, sky = sun.data, scene.world
        bpy.data.objects.remove(sun, do_unlink=True)
        bpy.data.lights.remove(light)
        scene.world = previous_world
        bpy.data.worlds.remove(sky)
    return seconds, denoised


def export_checked(scene, proxy, glb, rigs, static, reference, size):
    """The baked glb (the proxy unlinked meanwhile) and its checks; raises if the reference export is another scene."""
    for obj in static:
        obj.data['lightmapAtlas'] = 0
    compose_tinted_base_colors()
    scene.collection.objects.unlink(proxy)
    try:
        export_glb(str(glb), rigs, export_extras=True, export_texcoords=True)
    finally:
        scene.collection.objects.link(proxy)
    validate_glb(str(glb), rigs)
    doc, binary = read_glb(glb)
    report = check_structure(doc)
    if (report['static_without_texcoord1'] or report['dynamic_with_texcoord1'] or report['static_meshes_without_atlas']
            or report['unbaked_with_texcoord1']):
        raise RuntimeError("lightmap UVs missing on static meshes or present on dynamic, foliage or prop ones")
    if not reference.exists():
        raise RuntimeError(f"no reference export {reference}")
    same = compare_geometry(doc, binary, *read_glb(reference))
    if not same['same_node_names_in_order'] or same['primitives_with_other_triangles'] or same['max_corner_distance'] > 1e-4:
        raise RuntimeError(f"the baked export is not {reference.name}'s geometry: re-export the lab, or check the .blend")
    check_uvs(doc, binary, size, gap_limit=12)


def _atlas_names(out, prefix):
    """The atlas entry's map files: full (previews), sun, lamp (the lamps' direct light; not in bakes from before
    2026-10-07) and rest (full - sun - lamp as a map of its own, for the renderer)."""
    return {name: denoise.final_path(out, name, prefix).name for name in denoise.final_maps(out, prefix)}


def write_json(path, args, uv_stats, seconds, denoised):
    meta = dict(version=1, atlases=[dict(_atlas_names(path.parent, args.prefix), size=args.size)],
                sun=dict(dir=list(args.sun_dir), strength=args.sun_strength, angle=args.sun_angle),
                sky=None if args.hdri else list(args.sky), **(dict(hdri=args.hdri.name) if args.hdri else {}),
                scaleFull=args.scale_full, scaleSun=args.scale_sun, samples=args.samples,
                texelsPerMeter=round(uv_stats['texels_per_meter'], 2),
                bakeSeconds=round(sum(seconds.values()), 1), denoised=denoised)
    path.write_text(json.dumps(meta, indent=2) + "\n")
    print(f"[lightmap] written {path}; bake seconds {seconds}; denoised {denoised}", flush=True)


def denoise_only(out, prefix):
    if out is None:
        raise SystemExit("--denoise-only needs --out (the folder with the *_noisy.hdr files)")
    denoised = denoise.finish(out, prefix)
    path = out / f"{prefix}.json"
    if path.exists():
        meta = json.loads(path.read_text())
        meta['denoised'] = denoised
        for atlas in meta.get('atlases', []):
            atlas.update(_atlas_names(out, prefix))
        path.write_text(json.dumps(meta, indent=2) + "\n")
        print(f"[lightmap] {path}: denoised {denoised}", flush=True)
    else:
        print(f"[lightmap] WARNING: no {path} to update", flush=True)


def main():
    args = parse_args()
    if args.denoise_only:
        return denoise_only(args.out or (Path(bpy.data.filepath).parent if bpy.data.filepath else None), args.prefix)
    if not bpy.data.filepath or Path(bpy.data.filepath).name not in SCENES:
        raise SystemExit("open tools/bench/assets/lab_scene.blend (see the command above); if it is missing, build it with "
                         "build_lab_scene.py and fix_lab_scene_round2.py, keeping in mind the owner hand-edits it "
                         "(or lab_scene_outdoor.blend, built by build_lab_outdoor.py)")
    blend = Path(bpy.data.filepath)
    out = args.out or blend.parent
    scene = bpy.context.scene
    rigs, static, dynamic = prepare(scene)
    proxy, uv_stats = make_uvs(scene, static, args.size, args.margin)
    meta = out / f"{args.prefix}.json"
    write_json(meta, args, uv_stats, {}, False)  # kept whatever fails later
    export_checked(scene, proxy, out / args.glb_name, rigs, static, blend.with_suffix('.glb'), args.size)
    seconds, denoised = light_and_bake(scene, proxy, dynamic, args, out)
    proxies.remove(proxy)
    write_json(meta, args, uv_stats, seconds, denoised)


main()
