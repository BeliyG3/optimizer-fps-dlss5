"""Screens of the ghost guard (FOLIAGE_DIAG.md, "Ghost guard"): what the markers change at the disocclusion band.

Usage: python ghost_sheet.py <guard view> <sheet.png> [--view-png PNG] [--root DIR] [--d N]
Sheet: on the full-pass frame where t1 r1 and t1off (t1nomm) differ most inside the band, T0 | t1 (markers) | t1off (no
markers) full frame, below the same 3x nearest-neighbour crop where t1off's error against T0 in the band is densest, and
under it the band (T0 column) or the |error vs T0| x4 (t1, t1off) in that crop.
--view-png: T0 at the middle dump with the moving character's path (centroid of the span-8 motion > 0.5 px every 8
frames, every fourth dot and the last labelled with its frame).
"""
import argparse, os

import numpy as np
from PIL import Image, ImageDraw, ImageFont

from foliage_metrics import N, frames_of, full_pass_frames, rgb
from ghost_metrics import GUARD, ROOT, frame_band, resize_nearest
from f16io import read_ofmv

CROP = (200, 150)          # output px, shown 3x
CELL = 640                 # full frames scaled to 640 px wide
COLS = (("t0_r1", "T0"), ("t1_r1", "t1 (markers)"), ("t1nomm_r1", "t1off (no markers)"))


def font(size):
    try:
        return ImageFont.truetype("arial.ttf", size)
    except OSError:
        return ImageFont.load_default()


def worst_frame(dirs, d):
    """-> (frame, band) of the full pass with the largest mean |t1 - t1off| inside the band."""
    _, _, full = full_pass_frames(dirs["t1acc_r1"], dirs["gt8_r1"], set(), d)
    depth = set(frames_of(dirs["gtdepth_r1"], r"depth_(\d+)\.f32"))
    best = None
    for f in sorted(full):
        if f not in depth or f - N not in depth or not os.path.exists(os.path.join(dirs["gt8_r1"], f"motion_{f}.f32")):
            continue
        a, b = rgb(dirs["t1_r1"], f), rgb(dirs["t1nomm_r1"], f)
        band = resize_nearest(frame_band(dirs["gtdepth_r1"], dirs["gt8_r1"], f), a.shape[:2])
        score = float(np.abs(a[band] - b[band]).mean()) if band.any() else 0.0
        if best is None or score > best[0]:
            best = (score, f, band)
    assert best, "no full-pass frame has gt8 and gtdepth at t and t-8"
    return best[1], best[2]


def crop_box(err, shape):
    """Top-left of the CROP window with the largest summed error (integral image, 8 px steps)."""
    h, w = shape
    s = np.pad(err, ((1, 0), (1, 0))).cumsum(0).cumsum(1)
    cw, ch = CROP
    ys, xs = np.arange(0, h - ch + 1, 8), np.arange(0, w - cw + 1, 8)
    tot = s[ys[:, None] + ch, xs[None, :] + cw] - s[ys[:, None], xs[None, :] + cw] - s[ys[:, None] + ch, xs[None, :]] + s[ys[:, None], xs[None, :]]
    iy, ix = np.unravel_index(np.argmax(tot), tot.shape)
    return int(xs[ix]), int(ys[iy])


def sheet(dirs, d, out, title):
    f, band = worst_frame(dirs, d)
    imgs = {k: rgb(dirs[k], f) for k, _ in COLS}
    err = np.abs(imgs["t1nomm_r1"] - imgs["t0_r1"]).mean(2) * band
    x0, y0 = crop_box(err, band.shape)
    h, w = band.shape
    ch = CELL * h // w
    zw, zh = CROP[0] * 3, CROP[1] * 3
    pad, head = 12, 40
    canvas = Image.new("RGB", (3 * CELL + 4 * pad, head + ch + 2 * zh + 4 * pad + 30), (24, 24, 24))
    draw = ImageDraw.Draw(canvas)
    draw.text((pad, 10), f"{title}  frame {f} (full pass)  band = disocclusion at t-{N}  crop {CROP[0]}x{CROP[1]} at ({x0},{y0}) x3",
              fill=(230, 230, 230), font=font(18))
    for i, (k, label) in enumerate(COLS):
        im = Image.fromarray(np.clip(imgs[k], 0, 255).astype(np.uint8))
        x = pad + i * (CELL + pad)
        canvas.paste(im.resize((CELL, ch), Image.Resampling.BILINEAR), (x, head))
        sx = CELL / w
        draw.rectangle([x + x0 * sx, head + y0 * sx, x + (x0 + CROP[0]) * sx, head + (y0 + CROP[1]) * sx], outline=(255, 64, 64), width=2)
        zoom = im.crop((x0, y0, x0 + CROP[0], y0 + CROP[1])).resize((zw, zh), Image.Resampling.NEAREST)
        canvas.paste(zoom, (x + (CELL - zw) // 2, head + ch + pad))
        if k == "t0_r1":    # the band in the crop, red over a dimmed T0
            low = np.clip(imgs[k] * 0.5, 0, 255)
            low[band] = (255, 40, 40)
        else:               # |error vs T0| x4
            low = np.clip(np.abs(imgs[k] - imgs["t0_r1"]) * 4, 0, 255)
        low = Image.fromarray(low.astype(np.uint8)).crop((x0, y0, x0 + CROP[0], y0 + CROP[1]))
        canvas.paste(low.resize((zw, zh), Image.Resampling.NEAREST), (x + (CELL - zw) // 2, head + ch + zh + 2 * pad))
        sub = "band (red)" if k == "t0_r1" else "|error vs T0| x4"
        draw.text((x, head + ch + 2 * zh + 3 * pad), f"{label}   lower: {sub}", fill=(255, 220, 120), font=font(22))
    canvas.save(out)
    return f, (x0, y0)


def view_png(dirs, out, title):
    frames = frames_of(dirs["gt8_r1"], r"motion_(\d+)\.f32")
    mid = frames[len(frames) // 2]
    im = Image.fromarray(np.clip(rgb(dirs["t0_r1"], mid), 0, 255).astype(np.uint8))
    draw = ImageDraw.Draw(im)
    pts = []
    for f in frames[::N] + ([frames[-1]] if (len(frames) - 1) % N else []):
        _, gt = read_ofmv(os.path.join(dirs["gt8_r1"], f"motion_{f}.f32"))
        ys, xs = np.nonzero(np.hypot(gt[..., 0], gt[..., 1]) > 0.5)
        if xs.size:
            s = im.width / gt.shape[1]
            pts.append((f, xs.mean() * s, ys.mean() * s))
    draw.line([(x, y) for _, x, y in pts], fill=(255, 64, 64), width=4)
    for i, (f, x, y) in enumerate(pts):
        draw.ellipse([x - 7, y - 7, x + 7, y + 7], fill=(255, 64, 64))
        if i % 4 == 0 or i == len(pts) - 1:     # every 32 frames, the labels overlap otherwise
            draw.text((x - 10, y + 12), str(f), fill=(255, 255, 0), font=font(22))
    draw.text((16, 12), f"{title}: T0 frame {mid}, the character's path (motion centroid every {N} frames)",
              fill=(255, 255, 255), font=font(26))
    im.save(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("view", choices=sorted(GUARD))
    ap.add_argument("sheet")
    ap.add_argument("--view-png")
    ap.add_argument("--root", default=ROOT, required=ROOT is None)
    ap.add_argument("--d", type=int)
    a = ap.parse_args()
    base = os.path.join(a.root, GUARD[a.view])
    dirs = {f"{c}_r1": os.path.join(base, c, "r1") for c in ("t0", "t1", "t1nomm", "t1acc", "gt8", "gtdepth")}
    print(a.view, "frame %d, crop at %s" % sheet(dirs, a.d, a.sheet, a.view))
    if a.view_png:
        view_png(dirs, a.view_png, a.view)


if __name__ == "__main__":
    main()
