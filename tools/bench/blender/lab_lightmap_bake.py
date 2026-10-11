"""The three DIFFUSE bakes of the proxy (bake_lab_lightmaps.py), saved as the noisy maps that lab_lightmap_denoise.py
finishes: `full` = direct + indirect of every light, `sun` = direct only with every emission and the sky muted, `lamp` =
direct only with the sun hidden and the sky muted (the lamp groups' direct light: every emissive material is a lamp
group in bench12).

Tiles: Cycles bakes one image in one session, and an 8192² atlas needs more VRAM than this machine has free. With
--tile T the atlas is baked tile by tile: for tile (i, j) the proxy's Lightmap UVs become uv * size/T - (i, j), so the
tile fills [0, 1] of a T² image and everything else falls outside it; the tile's pixels go into a float32 atlas, and
the UVs are restored. A texel is baked in exactly one tile, the same as untiled (only its noise differs). The 4-texel
EXTEND margin is per tile: a gap texel at a tile border is filled from its own tile only.
"""
import time

import bpy
import numpy as np

from lab_lightmap_denoise import PREFIX, float_image, noisy_path, save
from lab_lightmap_lighting import mute_emission, restore, restore_simplify, simplify
from lab_lightmap_uv import LAYER

BAKE_NODE = "lightmap_bake"


def materials_of(obj):
    return [m for m in dict.fromkeys(slot.material for slot in obj.material_slots) if m is not None]


def add_bake_nodes(materials, image):
    """An unconnected Image Texture node, active, in every material: where IMAGE_TEXTURES bakes write."""
    for material in materials:
        node = material.node_tree.nodes.new('ShaderNodeTexImage')
        node.name, node.image = BAKE_NODE, image
        material.node_tree.nodes.active = node


def remove_bake_nodes(materials):
    for material in materials:
        nodes = material.node_tree.nodes
        if BAKE_NODE in nodes:
            nodes.remove(nodes[BAKE_NODE])


def bake(proxy, passes, margin):
    bpy.ops.object.select_all(action='DESELECT')
    proxy.select_set(True)
    bpy.context.view_layer.objects.active = proxy
    start = time.perf_counter()
    bpy.ops.object.bake(type='DIFFUSE', pass_filter=passes, margin=margin, margin_type='EXTEND', use_selected_to_active=False,
                        target='IMAGE_TEXTURES', uv_layer=LAYER, use_clear=True)
    return time.perf_counter() - start


def bake_atlas(proxy, image, passes, margin, size, name):
    """One map as a (size, size, 4) float32 array (Blender's bottom-up rows), baked tile by tile into `image` (the tile
    size). Returns (atlas, seconds)."""
    tile = image.size[0]
    tiles = size // tile
    mesh = proxy.data
    original = np.empty(len(mesh.loops) * 2, np.float32)
    mesh.uv_layers[LAYER].data.foreach_get('uv', original)
    atlas = np.zeros((size, size, 4), np.float32)
    pixels = np.empty(tile * tile * 4, np.float32)
    total = 0.0
    try:
        for j in range(tiles):
            for i in range(tiles):
                moved = original.reshape(-1, 2) * np.float32(tiles) - np.array([i, j], np.float32)
                mesh.uv_layers[LAYER].data.foreach_set('uv', moved.ravel())
                mesh.update()
                seconds = bake(proxy, passes, margin)
                total += seconds
                image.pixels.foreach_get(pixels)
                atlas[j * tile:(j + 1) * tile, i * tile:(i + 1) * tile] = pixels.reshape(tile, tile, 4)
                if tiles > 1:
                    print(f"[bake] {name} tile ({i}, {j}) of {tiles}x{tiles}: {seconds:.1f} s", flush=True)
    finally:
        mesh.uv_layers[LAYER].data.foreach_set('uv', original)
        mesh.update()
    return atlas, total


def save_hdr(atlas, path):
    rgb = atlas[..., :3]
    lit = float(np.count_nonzero(rgb.max(axis=2) > 0)) / (atlas.shape[0] * atlas.shape[1])
    print(f"[bake] saving {path}: mean {rgb.mean():.4f} max {rgb.max():.2f} non-zero texels {lit:.3f}", flush=True)
    if not rgb.any():
        raise RuntimeError(f"{path} is black: check the GPU device list and the active image nodes")
    save(path, atlas)


def _hide_all_but(scene, keep, dynamic):
    """Hides every other object from render (meshes, lights, instancers, anything) and logs it. Returns them; raises
    (after unhiding) if a character still renders, since its shadow would be baked."""
    hidden = [o for o in scene.objects if o not in keep and not o.hide_render]
    for obj in hidden:
        obj.hide_render = True
    characters = sum(1 for o in dynamic if o.hide_render)
    kinds = {}
    for obj in hidden:
        kinds[obj.type] = kinds.get(obj.type, 0) + 1
    print(f"[bake] hidden from render for every bake: {characters} of {len(dynamic)} dynamic (character) meshes, "
          f"{len(hidden)} objects in all {kinds}; rendering: {[o.name for o in keep]}", flush=True)
    if characters != len(dynamic):
        _unhide(hidden)
        raise RuntimeError("a character would cast baked shadows")
    return hidden


def _unhide(hidden):
    for obj in hidden:
        obj.hide_render = False


def preflight(scene, proxy, sun, dynamic):
    """What the bakes will hide and mute, tried and undone at once, so a problem stops the script before hours of bake."""
    _unhide(_hide_all_but(scene, (proxy, sun), dynamic))
    restore(mute_emission(scene.world))
    restore(mute_emission(scene.world, materials=False))


# name, Cycles passes, what is muted: every emission and the sky ('sun'), the sky with the sun hidden ('lamp'), or nothing.
BAKES = (('full', {'DIRECT', 'INDIRECT'}, None), ('sun', {'DIRECT'}, 'sun'), ('lamp', {'DIRECT'}, 'lamp'))


def _isolate(scene, sun, only):
    """Mutes for one bake; returns the undo for _release()."""
    if only == 'sun':
        return mute_emission(scene.world), False
    if only == 'lamp':
        sun.hide_render = True
        return mute_emission(scene.world, materials=False), True
    return [], False


def _release(sun, isolated):
    muted, sun_hidden = isolated
    restore(muted)
    if sun_hidden:
        sun.hide_render = False


def bake_maps(scene, proxy, sun, dynamic, size, tile, margin, out, prefix=PREFIX):
    """The three maps, saved as <prefix>_{full,sun,lamp}_0_noisy.hdr in out. Only the proxy and the sun render (the
    characters' shadows stay dynamic); Simplify limits textures to 2048. Returns the seconds per bake."""
    tile = tile or size
    if size % tile:
        raise ValueError(f"--size {size} is not a multiple of --tile {tile}")
    materials = materials_of(proxy)
    image = float_image("lightmap_bake", tile, tile)
    hidden, simplified, seconds = [], None, {}
    try:
        hidden = _hide_all_but(scene, (proxy, sun), dynamic)
        simplified = simplify(scene)
        add_bake_nodes(materials, image)
        for name, passes, only in BAKES:
            isolated = _isolate(scene, sun, only)
            try:
                atlas, seconds[name] = bake_atlas(proxy, image, passes, margin, size, name)
            finally:
                _release(sun, isolated)
            print(f"[bake] {name}: {seconds[name]:.1f} s at {scene.cycles.samples} samples, {size}² in {tile}² tiles", flush=True)
            save_hdr(atlas, noisy_path(out, name, prefix))
            del atlas
    finally:
        remove_bake_nodes(materials)
        if simplified is not None:
            restore_simplify(scene, simplified)
        _unhide(hidden)
        bpy.data.images.remove(image)
    return seconds
