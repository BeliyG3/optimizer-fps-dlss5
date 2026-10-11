"""Prepare a Poly Haven sky panorama for bench12 (--hdri): remove the sun disc, align the panorama to the bench sun,
scale the sky so its horizontal irradiance is a fixed fraction of the bench sun's.

    python hdri_prepare.py IN.hdr OUT.hdr [--sun-dir 0.45,-0.77,0.45] [--sun-strength 1500] [--sky-ratio 0.12]
                           [--preview before_after.png]

Writes OUT.hdr and OUT.json (sun_dir_original, rotation_deg, scale, disc_radius_deg, residual_peak_ratio).
"""
import argparse
import json
import math
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hdri_rgbe import read_hdr, write_hdr  # noqa: E402

LUMA = np.array([0.2126, 0.7152, 0.0722], np.float32)


# --- Equirect convention, mirrored from tools/bench12/shaders/scene.hlsli Sky() and image.cpp LoadHdr -----------------
# uv = (atan2(d.z, d.x) / 2pi + 0.5, acos(d.y) / pi); row 0 is the top scanline (-Y), so +y is up. Bench space is
# left-handed, but the lookup is written in bench space, so no handedness conversion belongs here.
def dir_to_rowcol(d, h, w):
    """Unit direction (bench space) -> float (row, col) in texel-centre coordinates (centre of texel 0 is 0.0)."""
    d = np.asarray(d, np.float64); d = d / np.linalg.norm(d)
    u = math.atan2(d[2], d[0]) / (2 * math.pi) + 0.5
    v = math.acos(max(-1.0, min(1.0, d[1]))) / math.pi
    return v * h - 0.5, (u * w - 0.5) % w


def rowcol_to_dir(row, col, h, w):
    u = (col + 0.5) / w; v = (row + 0.5) / h
    phi = (u - 0.5) * 2 * math.pi; theta = v * math.pi
    return np.array([math.sin(theta) * math.cos(phi), math.cos(theta), math.sin(theta) * math.sin(phi)])


def luminance(img):
    return img @ LUMA


def _window(img, row, col, half):
    """A crop of about +-half texels around (row, col): the image rolled so the column lands at the crop's centre;
    rows are clamped to the image, columns wrap (a window wider than the image is the whole rolled image).
    Returns (crop, r0, shift, c0): crop[i, j] == img[r0 + i, (c0 + j - shift) % w]."""
    h, w = img.shape[:2]
    shift = w // 2 - int(round(col))
    r0 = max(0, int(round(row)) - half); r1 = min(h, int(round(row)) + half + 1)
    c0 = max(0, w // 2 - half); c1 = min(w, w // 2 + half + 1)
    return np.roll(img[r0:r1], shift, axis=1)[:, c0:c1], r0, shift, c0


def find_sun(img):
    """The compact peak: brightest texel, then its connected region while luminance > 1 % of the peak.
    Returns (row, col, radius_px) with a luminance-weighted centre and radius = sqrt(area / pi)."""
    h, w = img.shape[:2]
    lum = luminance(img)
    pr, pc = np.unravel_index(int(np.argmax(lum)), lum.shape)
    peak = float(lum[pr, pc])
    half = 8
    while True:
        crop, r0, shift, c0 = _window(lum, pr, pc, half)
        ci = int(pr) - r0; cj = w // 2 - c0
        sub = crop > 0.01 * peak
        region = np.zeros_like(sub); region[ci, cj] = True
        while True:  # 4-connected flood fill by dilation
            grown = region.copy()
            grown[1:] |= region[:-1]; grown[:-1] |= region[1:]; grown[:, 1:] |= region[:, :-1]; grown[:, :-1] |= region[:, 1:]
            grown &= sub
            if (grown == region).all():
                break
            region = grown
        edge = region[:, 0].any() or region[:, -1].any() or (r0 > 0 and region[0].any()) or (r0 + crop.shape[0] < h and region[-1].any())
        if not edge or half >= max(h, w):
            break
        half *= 2
    rr, jj = np.nonzero(region)
    weight = crop[rr, jj]
    row = r0 + float((rr * weight).sum() / weight.sum())
    col = (c0 + float((jj * weight).sum() / weight.sum()) - shift) % w
    return row, col, math.sqrt(len(rr) / math.pi)


def _offsets(crop, ci, cj):
    yy, xx = np.mgrid[0:crop.shape[0], 0:crop.shape[1]]
    dy = yy - ci; dx = xx - cj
    return dx, dy, np.hypot(dx, dy)


def removal_radius(img, row, col, disc_radius, flat=2.5, limit=240):
    """Radius (px) that covers the sun's aureole, not just the 1 % disc. Grows from the disc radius until the median
    luminance of the 1-texel annulus at r is within `flat` x the median of the 2r..4r ring, i.e. the glow has fallen to
    the sky's own level. Never below the disc radius."""
    w = img.shape[1]
    half = int(min(4 * limit, img.shape[0]) // 1)
    crop, r0, shift, c0 = _window(luminance(img), row, col, half)
    _, _, d = _offsets(crop, row - r0, (col + shift) % w - c0)
    r = max(disc_radius, 1.0)
    while r < limit:
        here = np.median(crop[(d >= r) & (d < r + 1)])
        ring = np.median(crop[(d >= 2 * r) & (d <= 4 * r)])
        if here <= flat * ring:
            break
        r += 1.0
    return float(r)


def inpaint_disc(img, row, col, radius):
    """Replace the texels within 1.5 x radius by the surrounding sky: a harmonic (Laplace) fill whose boundary is the
    original sky just outside that circle. Each interior texel ends up a distance-weighted average of the whole
    boundary, i.e. an azimuthal average that follows the sky's gradient and leaves no step or halo at the edge.
    `radius` is meant to cover the sun's aureole (see removal_radius), so the boundary is already plain sky."""
    out = np.array(img, np.float32, copy=True)
    w = out.shape[1]
    reach = 1.5 * radius
    crop, r0, shift, c0 = _window(out, row, col, int(math.ceil(reach)) + 3)
    _, _, d = _offsets(crop, row - r0, (col + shift) % w - c0)
    inside = d < reach
    work = crop.astype(np.float64)
    work[inside] = work[~inside & (d < reach + 2)].mean(axis=0)  # start from the boundary mean
    yy, xx = np.mgrid[0:crop.shape[0], 0:crop.shape[1]]
    n = max(crop.shape)
    omega = 2.0 / (1.0 + math.sin(math.pi / n))
    for _ in range(int(6 * n) + 50):  # red-black SOR; the crop's border is outside the disc and never changes
        for colour in (0, 1):
            free = (inside & ((yy + xx) % 2 == colour))[..., None]
            avg = 0.25 * (np.roll(work, 1, 0) + np.roll(work, -1, 0) + np.roll(work, 1, 1) + np.roll(work, -1, 1))
            work = np.where(free, work + omega * (avg - work), work)
    crop[:] = work.astype(np.float32)
    rolled = np.roll(out[r0:r0 + crop.shape[0]], shift, axis=1)
    rolled[:, c0:c0 + crop.shape[1]] = crop
    out[r0:r0 + crop.shape[0]] = np.roll(rolled, -shift, axis=1)
    return out


def residual_peak_ratio(img, row, col, radius):
    """Largest luminance inside 1.5 x radius over the median luminance of the 2..4 x radius ring."""
    w = img.shape[1]
    crop, r0, shift, c0 = _window(luminance(img), row, col, int(math.ceil(4 * radius)) + 2)
    _, _, d = _offsets(crop, row - r0, (col + shift) % w - c0)
    return float(crop[d <= 1.5 * radius].max() / np.median(crop[(d >= 2 * radius) & (d <= 4 * radius)]))


def rotate_azimuth(img, delta_cols):
    """Content moves right by delta_cols columns (azimuth + delta * 2pi / W)."""
    return np.roll(img, int(delta_cols), axis=1)


def horizontal_irradiance(img):
    """Integral of luminance * cos(theta) over the upper hemisphere (y > 0), rows weighted by solid angle."""
    h, w = img.shape[:2]
    theta = (np.arange(h) + 0.5) * math.pi / h
    wt = np.where(theta < math.pi / 2, np.cos(theta) * np.sin(theta), 0) * (math.pi / h) * (2 * math.pi / w)
    return float((luminance(img).sum(axis=1) * wt).sum())


def solid_angle_radius_deg(row, radius_px, h, w):
    omega = math.pi * radius_px ** 2 * (math.pi / h) * (2 * math.pi / w) * math.sin((row + 0.5) * math.pi / h)
    return math.degrees(math.sqrt(omega / math.pi))


def prepare(img, sun_dir, sun_strength, sky_ratio):
    """Returns (prepared image, report dict, found disc (row, col, radius_px))."""
    h, w = img.shape[:2]
    sun_dir = np.asarray(sun_dir, np.float64); sun_dir = sun_dir / np.linalg.norm(sun_dir)
    row, col, disc = find_sun(img)
    radius = removal_radius(img, row, col, disc)
    clean = inpaint_disc(img, row, col, radius)
    original = rowcol_to_dir(row, col, h, w)
    target = -sun_dir  # the bench sun direction is the travel direction; the sun sits opposite
    phi_o = math.atan2(original[2], original[0]); phi_t = math.atan2(target[2], target[0])
    delta = int(round((phi_t - phi_o) / (2 * math.pi) * w)) % w
    if delta > w // 2:
        delta -= w
    sun_irradiance = sun_strength * max(0.0, -sun_dir[1])
    scale = sky_ratio * sun_irradiance / horizontal_irradiance(clean)
    report = {
        "sun_dir_original": [round(float(x), 6) for x in original],
        "sun_pixel_original": [round(row, 2), round(col, 2)],
        "rotation_deg": delta * 360.0 / w,
        "rotation_cols": delta,
        "scale": scale,
        "disc_radius_px": round(disc, 3),
        "disc_radius_deg": solid_angle_radius_deg(row, disc, h, w),
        "removal_radius_px": round(radius, 3),
        "removal_radius_deg": solid_angle_radius_deg(row, radius, h, w),
        "residual_peak_ratio": residual_peak_ratio(clean, row, col, radius),
        "sun_dir_bench": [float(x) for x in sun_dir],
        "sun_strength": sun_strength, "sky_ratio": sky_ratio,
        "sun_horizontal_irradiance": sun_irradiance,
        "sky_horizontal_irradiance_after": horizontal_irradiance(clean) * scale,
    }
    return (rotate_azimuth(clean, delta) * scale).astype(np.float32), report, (row, col, disc)


def _vec3(text):
    v = [float(x) for x in text.split(",")]
    if len(v) != 3:
        raise argparse.ArgumentTypeError("expected x,y,z")
    return v


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input"); ap.add_argument("output")
    ap.add_argument("--sun-dir", type=_vec3, default=[0.45, -0.77, 0.45])
    ap.add_argument("--sun-strength", type=float, default=1500.0)
    ap.add_argument("--sky-ratio", type=float, default=0.12)
    ap.add_argument("--preview", help="PNG: before | after, tone mapped, the found disc circled on before")
    args = ap.parse_args(argv)
    img = read_hdr(args.input)
    out, report, disc = prepare(img, args.sun_dir, args.sun_strength, args.sky_ratio)
    write_hdr(args.output, out)
    with open(os.path.splitext(args.output)[0] + ".json", "w") as f:
        json.dump(report, f, indent=2)
    if args.preview:
        from hdri_preview import write_preview
        write_preview(args.preview, img, out, disc, report)
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
