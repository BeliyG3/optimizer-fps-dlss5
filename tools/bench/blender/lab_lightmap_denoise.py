"""The final lightmaps from the noisy bakes (bake_lab_lightmaps.py, also alone with --denoise-only).

OIDN runs through the compositor of an empty scene (Image -> Denoise, HDR, no guide passes). `sun`, `lamp` and the
rest of `full` are denoised apart, and the rest is a map of its own: RGBE keeps ~8 mantissa bits of the brightest
channel, so full - sun read back from two files would lose the indirect light under the skylight.

    sun  := D(sun_noisy)
    lamp := D(lamp_noisy)                                  (the lamp groups' direct light)
    rest := D(max(full_noisy - sun_noisy - lamp_noisy, 0)) (indirect light and the sky's direct light)
    full := rest + sun + lamp                              (kept for previews)

The renderer uses rest + lamp x lamp visibility + sun x dynamic shadow. Bakes from before the lamp map (no
*_lamp_0_noisy.hdr) are finished as before: rest := D(max(full_noisy - sun_noisy, 0)), full := rest + sun.

If the denoise fails, the noisy maps become the final ones (rest = max(full - sun [- lamp], 0), undenoised) and
finish() returns False, so the bakes are kept.
"""
import os
import shutil
import time
import traceback

import bpy
import numpy as np

PREFIX = "lab_lightmap"  # the lab's file names; bake_lab_lightmaps.py --prefix names another scene's maps
MAPS = ('full', 'sun', 'lamp')  # baked (lamp: from 2026-10-07 on)
FINAL_MAPS = ('full', 'sun', 'rest', 'lamp')  # written


def final_path(out, name, prefix=PREFIX):
    return out / f"{prefix}_{name}_0.hdr"


def noisy_path(out, name, prefix=PREFIX):
    return out / f"{prefix}_{name}_0_noisy.hdr"


def has_lamp(out, prefix=PREFIX):
    """True when the bake has a lamp map (bakes from before 2026-10-07 have full and sun alone)."""
    return noisy_path(out, 'lamp', prefix).exists()


def final_maps(out, prefix=PREFIX):
    """The final maps of this bake: FINAL_MAPS, without lamp for an older bake."""
    return tuple(name for name in FINAL_MAPS if name != 'lamp' or has_lamp(out, prefix))


def _load(path):
    image = bpy.data.images.load(str(path), check_existing=False)
    w, h = image.size
    pixels = np.empty(w * h * 4, np.float32)
    image.pixels.foreach_get(pixels)
    bpy.data.images.remove(image)
    return pixels.reshape(h, w, 4)


def float_image(name, width, height):
    """A float image whose pixels are written and read as they are. A new float image is tagged sRGB, and saving it to
    .hdr then converts the values (a 0.30 mean became 0.44); bakes write linear values whatever the tag."""
    image = bpy.data.images.new(name, width, height, alpha=False, float_buffer=True)
    image.colorspace_settings.is_data = True
    return image


def _image(name, pixels):
    image = float_image(name, pixels.shape[1], pixels.shape[0])
    image.pixels.foreach_set(pixels.ravel())
    return image


def save(path, pixels):
    """An RGBA float array (Blender's bottom-up rows) as Radiance .hdr, values unchanged up to RGBE rounding."""
    image = _image("lightmap_final", pixels)
    try:
        image.filepath_raw = str(path)
        image.file_format = 'HDR'
        image.save()
    finally:
        bpy.data.images.remove(image)


def oidn(pixels, scratch):
    """The denoised copy of an RGBA float array (negative output clamped to 0), through a scratch .hdr file."""
    scene = bpy.data.scenes.new("lightmap_denoise")
    tree = bpy.data.node_groups.new("lightmap_denoise", 'CompositorNodeTree')
    image = _image("lightmap_noisy", pixels)
    try:
        tree.interface.new_socket("Image", in_out='OUTPUT', socket_type='NodeSocketColor')
        source = tree.nodes.new('CompositorNodeImage')
        source.image = image
        denoise = tree.nodes.new('CompositorNodeDenoise')
        denoise.inputs['HDR'].default_value = True
        tree.links.new(source.outputs['Image'], denoise.inputs['Image'])
        tree.links.new(denoise.outputs['Image'], tree.nodes.new('NodeGroupOutput').inputs[0])
        scene.compositing_node_group = tree
        scene.render.resolution_x, scene.render.resolution_y = pixels.shape[1], pixels.shape[0]
        scene.render.resolution_percentage = 100
        scene.render.image_settings.file_format = 'HDR'
        scene.view_settings.view_transform = 'Standard'  # HDR output stays linear; this only rules out a display look
        bpy.ops.render.render(write_still=False, scene=scene.name)
        bpy.data.images['Render Result'].save_render(str(scratch), scene=scene)
        result = _load(scratch)
    finally:
        bpy.data.images.remove(image)
        bpy.data.node_groups.remove(tree)
        bpy.data.scenes.remove(scene)
        if os.path.exists(scratch):
            os.remove(scratch)
    result[..., 3] = 1.0
    return np.maximum(result, 0.0)


def _noisy_rest(out, prefix):
    """(noisy sun, noisy lamp or None, max(noisy full - noisy sun - noisy lamp, 0)) as RGBA arrays."""
    sun = _load(noisy_path(out, 'sun', prefix))
    lamp = _load(noisy_path(out, 'lamp', prefix)) if has_lamp(out, prefix) else None
    rest = _load(noisy_path(out, 'full', prefix)) - sun
    if lamp is not None:
        rest -= lamp
    rest = np.maximum(rest, 0.0)
    rest[..., 3] = 1.0
    return sun, lamp, rest


def finish(out, prefix=PREFIX):
    """Writes the final maps from the noisy ones. Returns True if they are denoised, False if they are the noisy ones."""
    try:
        start = time.perf_counter()
        noisy_sun, noisy_lamp, noisy_rest = _noisy_rest(out, prefix)
        scratch = out / f"{prefix}_denoise_scratch.hdr"
        sun = oidn(noisy_sun, scratch)
        del noisy_sun
        lamp = None if noisy_lamp is None else oidn(noisy_lamp, scratch)
        del noisy_lamp
        rest = oidn(noisy_rest, scratch)
        del noisy_rest
        save(final_path(out, 'sun', prefix), sun)
        save(final_path(out, 'rest', prefix), rest)
        full = rest + sun
        if lamp is not None:
            save(final_path(out, 'lamp', prefix), lamp)
            full += lamp
        full[..., 3] = 1.0
        save(final_path(out, 'full', prefix), full)
        lamp_mean = "none" if lamp is None else f"{lamp[..., :3].mean():.4f}"
        print(f"[denoise] final maps written in {time.perf_counter() - start:.1f} s: full mean {full[..., :3].mean():.4f}, "
              f"sun mean {sun[..., :3].mean():.4f}, lamp mean {lamp_mean}, rest mean {rest[..., :3].mean():.4f}", flush=True)
        return True
    except Exception:
        traceback.print_exc()
        banner = "!" * 100
        print(f"{banner}\n[denoise] FAILED: the final maps are the noisy bakes (\"denoised\": false in the JSON). "
              f"Rerun with --denoise-only once fixed.\n{banner}", flush=True)
        keep_noisy(out, prefix)
        return False


def keep_noisy(out, prefix=PREFIX):
    """The noisy bakes as the final maps (--no-denoise, a failed denoise): full, sun and lamp copied, rest computed."""
    for name in MAPS:
        if name != 'lamp' or has_lamp(out, prefix):
            shutil.copyfile(noisy_path(out, name, prefix), final_path(out, name, prefix))
    save(final_path(out, 'rest', prefix), _noisy_rest(out, prefix)[2])
