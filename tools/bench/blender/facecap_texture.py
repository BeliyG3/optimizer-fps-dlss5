"""Facecap texture work: baking KHR_texture_transform into the UVs and tinting the grey scan into skin."""

import bpy
import numpy as np

from export_log import make_log

log = make_log("[facecap]")

# KHR_texture_transform of the original `lambert5` material.
UV_OFFSET = (0.00724834995, 0.0100790262)
UV_SCALE = (0.000238723238, 0.000239208952)

# Linear-space multiplier that turns the grey scan into skin, plus a small
# overall lift so the tinted result does not read darker than the original.
SKIN_TINT = (1.0, 0.78, 0.68)
SKIN_GAIN = 1.08


def bake_uv_transform(mesh):
    """Apply the original KHR_texture_transform to the mesh's UV loops."""
    sx, sy = UV_SCALE
    ox, oy = UV_OFFSET
    v_bias = 1.0 - sy - oy
    for layer in mesh.uv_layers:
        for datum in layer.data:
            u, v = datum.uv
            datum.uv = (u * sx + ox, v * sy + v_bias)


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
