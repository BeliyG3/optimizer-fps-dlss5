"""PNG maps for review (phase 3 step 1): where a chain-rule candidate changes flicker and the ghost band.

  python motion_maps.py flicker <view> <run> [--root DIR] [--out DIR]
      The M2 flicker map on foliage: per pixel the mean |e_t - e_t-1| of the luma error against T0 over the measured
      frames (metrics.json), counted where the pixel is foliage on frame t (the vegmask; every pixel on a view without
      one, e.g. lab_forward_raster), for R0 and <run>, and their
      difference (green: <run> flickers less, magenta: more). Non-foliage pixels show T0's luma, dimmed.
  python motion_maps.py band <view> <run> [--root DIR] [--out DIR] [--cycle C]
      Crops of the disocclusion band (the cycle's union, motion_band.py; default: the cycle with the largest union):
      offsets 0 / 4 / 7 x T0 / R0 / <run> / |R0 - T0| / |<run> - T0| (heat, white = 16/255), the band outlined.
  python motion_maps.py restart <view> <run>_raw [--root DIR] [--out DIR] [--cycle C]
      Step 1c: where the model's chain and the carry chain (OFAC v3) restart over one cycle's offsets 2-7, the level
      floor (motion_split.floor_mask on R1_raw's depth) outlined; restart shares by region and offset over the whole
      window -> restarts_<run>.json in the view folder.
"""
import argparse, json, os, sys

import numpy as np
from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "foliage"))
from foliage_metrics import classes, luma, regions, rgb    # noqa: E402
from ghost_metrics import resize_nearest                   # noqa: E402
from motion_band import PREV, TRUTH, raw_fields            # noqa: E402
from motion_sheet import ROOT, heat                        # noqa: E402

LABEL = 44


def font():
    return ImageFont.truetype("arial.ttf", 28)


def flicker_map(view_dir, run, frames, mask):
    """-> {R0, run: per-pixel mean |de| on foliage (NaN where never foliage)}, T0 luma of the last frame."""
    sums, count, prev, t0 = {}, None, {}, None
    for f in range(frames[0], frames[1] + 1):
        t0 = luma(rgb(os.path.join(view_dir, "T0"), f))
        fol = regions(classes(mask, f))["foliage"] if mask else np.ones(t0.shape, bool)   # no vegmask: whole frame
        count = np.zeros(fol.shape) if count is None else count
        for r in ("R0", run):
            e = luma(rgb(os.path.join(view_dir, r), f)) - t0
            if r in prev:
                sums[r] = sums.get(r, np.zeros(fol.shape)) + np.where(fol, np.abs(e - prev[r]), 0)
            prev[r] = e
        if f > frames[0]:
            count += fol
    with np.errstate(invalid="ignore", divide="ignore"):
        return {r: np.where(count > 0, s / count, np.nan) for r, s in sums.items()}, t0


def flicker_sheet(view_dir, run):
    m = json.load(open(os.path.join(view_dir, "metrics.json")))
    maps, t0 = flicker_map(view_dir, run, m["frames"], m["mask"])
    reg = "foliage" if m["mask"] else "whole"
    back = np.repeat((np.clip(t0, 0, 255) * 0.35)[..., None], 3, -1).astype(np.uint8)
    tiles = []
    for r in ("R0", run):
        h = np.asarray(heat(np.nan_to_num(maps[r]), 4.0))         # white = 16/255 of mean |de|
        tiles.append((np.where(np.isnan(maps[r])[..., None], back, h), f"{m['view']} {r}: mean |de| on {reg}, white = 16/255"))
    d = maps["R0"] - maps[run]                                     # > 0: the candidate flickers less
    x = np.clip(np.nan_to_num(d) / 8.0, -1, 1)[..., None]
    div = np.where(x > 0, x * np.array([0, 255, 0]), -x * np.array([255, 0, 255])).astype(np.uint8)
    tiles.append((np.where(np.isnan(d)[..., None], back, div), f"R0 - {run}: green = {run} less flicker (8/255 full)"))
    fl = lambda r: m["m2"][r][reg]["flicker"]
    title = (f"{m['view']} {reg} M2 (frames {m['frames'][0]}-{m['frames'][1]}): R0 {fl('R0'):.3f}, {run} {fl(run):.3f} "
             f"({100 * (fl(run) - fl('R0')) / fl('R0'):+.1f} %)")
    return grid([tiles], title, (960, 540))


def band_sheet(view_dir, run, cycle=None):
    m = json.load(open(os.path.join(view_dir, "metrics.json")))
    p, (first, last, _) = m["phase"], m["frames"]
    truth, prev = os.path.join(view_dir, TRUTH), os.path.join(view_dir, PREV)
    unions = {}
    for c in range((first - p) // 8, (last - p) // 8 + 1):
        for o in range(8):
            raw = raw_fields(truth, prev, p + 8 * c + o, o or 8)
            if raw is not None:
                unions[c] = raw[2] if c not in unions else unions[c] | raw[2]
    c = max(unions, key=lambda k: unions[k].mean()) if cycle is None else cycle
    base = p + 8 * c
    t0 = rgb(os.path.join(view_dir, "T0"), base)
    u = resize_nearest(unions[c], t0.shape[:2])
    ys, xs = np.nonzero(u)
    pad = 24
    y0, y1 = max(ys.min() - pad, 0), min(ys.max() + pad, u.shape[0])
    x0, x1 = max(xs.min() - pad, 0), min(xs.max() + pad, u.shape[1])
    edge = u ^ np.pad(u[1:-1, 1:-1] & u[:-2, 1:-1] & u[2:, 1:-1] & u[1:-1, :-2] & u[1:-1, 2:], 1)
    rows = []
    for o in (0, 4, 7):
        img = {r: rgb(os.path.join(view_dir, r), base + o) for r in ("T0", "R0", run)}
        row = []
        for r in ("T0", "R0", run):
            a = img[r].copy()
            a[edge] = (0, 255, 255)
            row.append((a.astype(np.uint8)[y0:y1, x0:x1], f"{r} offset {o}"))
        for r in ("R0", run):
            e = np.abs(luma(img[r]) - luma(img["T0"]))
            row.append((np.asarray(heat(e, 4.0))[y0:y1, x0:x1], f"|{r} - T0| offset {o}, white = 16/255"))
        rows.append(row)
    band = lambda r: (m["m1"][r]["band"]["0"]["psnr"], np.mean([m["m1"][r]["band"][str(k)]["psnr"] for k in range(1, 8)]))
    title = (f"{m['view']} band crop, cycle {c} (frames {base}-{base + 7}), band PSNR offset 0 / 1-7: "
             f"R0 {band('R0')[0]:.2f} / {band('R0')[1]:.2f}, {run} {band(run)[0]:.2f} / {band(run)[1]:.2f}; cyan = band edge")
    h = 360
    w = int(round((x1 - x0) * h / (y1 - y0)))
    if w > 720:                                                    # keep the aspect ratio inside 720 x 360
        w, h = 720, int(round((y1 - y0) * 720 / (x1 - x0)))
    return grid(rows, title, (w, h)), base


def restart_sheet(view_dir, run, cycle=None):
    """Step 1c: where a <run>_raw (OFAC v3) restarts its chains, offsets 2-7. Restart = |chain - this frame's vector| <
    0.05 px where |true offset - vector| > 1 px (motion_metrics.chain_stats), for the model's chain (rule 0) and the carry
    chain. -> (sheet of one cycle, stats over the window: per region floor / other and offset, the restart shares)."""
    from f16io import read_ofac, read_ofdp
    from foliage_metrics import gt_motion
    from ghost_metrics import view_depth
    from motion_ofac import ofac_heads
    from motion_split import floor_mask
    m = json.load(open(os.path.join(view_dir, "metrics.json")))
    p, rd, truth, notes = m["phase"], os.path.join(view_dir, run), os.path.join(view_dir, TRUTH), set()
    evs = [e for e in sorted(ofac_heads(rd)) if (e - p) % 8 >= 2 and os.path.exists(os.path.join(rd, f"motion_{e}.f32"))
           and os.path.exists(os.path.join(truth, f"motion_{e}.f32"))]
    c = cycle if cycle is not None else max({(e - p) // 8 for e in evs}, key=lambda k: sum((e - p) // 8 == k for e in evs))
    stats, pix, floor0 = {}, None, None
    norm = lambda v: np.hypot(v[..., 0], v[..., 1])
    for e in evs:
        _, _, chain, _, carry = read_ofac(os.path.join(rd, f"ofps_acc_{e}.bin"), with_carry=True)
        carry = chain if carry is None else carry
        g, mv = gt_motion(truth, e, chain.shape[:2], notes), gt_motion(rd, e, chain.shape[:2], notes)
        det = (norm(g - mv) > 1) & np.isfinite(chain).all(-1) & np.isfinite(carry).all(-1)
        rm, rc = det & (norm(chain - mv) < 0.05), det & (norm(carry - mv) < 0.05)
        fl = floor_mask(resize_nearest(view_depth(read_ofdp(os.path.join(truth, f"depth_{e}.f32"))[1]), chain.shape[:2]))
        for reg, msk in (("floor", fl), ("other", ~fl)):
            s = stats.setdefault(reg, {}).setdefault((e - p) % 8, np.zeros(3))
            s += [np.count_nonzero(det & msk), np.count_nonzero(rm & msk), np.count_nonzero(rc & msk)]
        if (e - p) // 8 == c:
            pix = np.zeros(chain.shape[:2] + (3,)) if pix is None else pix
            pix += np.stack([det, rm, rc], -1)
            floor0 = fl if floor0 is None else floor0
    out = {reg: {str(k): {"detectable": int(v[0]), "model": v[1] / v[0] if v[0] else None, "carry": v[2] / v[0] if v[0] else None}
                 for k, v in sorted(ks.items())} for reg, ks in stats.items()}
    for reg, ks in stats.items():
        tot = sum(ks.values())
        out[reg]["all"] = {"detectable": int(tot[0]), "model": tot[1] / tot[0], "carry": tot[2] / tot[0]}
    n = np.maximum(pix[..., 0], 1)
    fm, fc = pix[..., 1] / n, pix[..., 2] / n                       # share of the cycle's detectable offsets restarted
    img = np.zeros(fm.shape + (3,))
    img += np.minimum(fm, fc)[..., None] * np.array([255, 230, 0])    # yellow: both chains restart
    img += np.clip(fm - fc, 0, 1)[..., None] * np.array([60, 120, 255])   # blue: the model's chain only (rule 0)
    img += np.clip(fc - fm, 0, 1)[..., None] * np.array([255, 40, 40])    # red: the carry chain only
    img[pix[..., 0] == 0] = (48, 48, 48)                               # grey: no restart detectable
    t0 = rgb(os.path.join(view_dir, "T0"), p + 8 * c + 7)
    shape = t0.shape[:2]
    img = resize_nearest(img, shape)
    edge = resize_nearest(floor0 ^ np.pad(floor0[1:-1, 1:-1] & floor0[:-2, 1:-1] & floor0[2:, 1:-1] & floor0[1:-1, :-2]
                                          & floor0[1:-1, 2:], 1), shape)
    img[edge] = (0, 255, 255)
    dim = (t0 * 0.6).astype(np.uint8)
    dim[edge] = (0, 255, 255)
    f = lambda reg, key: 100 * out[reg]["all"][key]
    title = (f"{m['view']} {run}: restarts, cycle {c} offsets 2-7 (frames {p + 8 * c + 2}-{p + 8 * c + 7}); floor (cyan) "
             f"model {f('floor', 'model'):.1f} % / carry {f('floor', 'carry'):.1f} %, other {f('other', 'model'):.1f} / "
             f"{f('other', 'carry'):.1f} % of detectable px (window)")
    tiles = [(dim, f"T0 frame {p + 8 * c + 7}, floor outlined"),
             (img, "yellow both, blue model only (rule 0), red carry only; grey: none detectable")]
    return grid([tiles], title, (960, 540)), dict(out, cycle=c, notes=sorted(notes))


def grid(rows, title, tile):
    f = font()
    out = Image.new("RGB", (tile[0] * max(len(r) for r in rows), LABEL + (tile[1] + LABEL) * len(rows)), (24, 24, 24))
    d = ImageDraw.Draw(out)
    d.text((10, 6), title, fill=(235, 235, 235), font=f)
    for i, row in enumerate(rows):
        for j, (a, text) in enumerate(row):
            x, y = j * tile[0], LABEL + i * (tile[1] + LABEL)
            d.text((x + 10, y + 6), text, fill=(235, 235, 235), font=f)
            out.paste(Image.fromarray(np.asarray(a, np.uint8)).resize(tile, Image.Resampling.BILINEAR), (x, y + LABEL))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("kind", choices=("flicker", "band", "restart"))
    ap.add_argument("view")
    ap.add_argument("run")
    ap.add_argument("--root", default=ROOT, required=ROOT is None)
    ap.add_argument("--out", help="default <root>\\sheets_step1")
    ap.add_argument("--cycle", type=int)
    a = ap.parse_args()
    view_dir = os.path.join(a.root, a.view)
    a.out = a.out or os.path.join(a.root, "sheets_step1")
    os.makedirs(a.out, exist_ok=True)
    if a.kind == "flicker":
        img, path = flicker_sheet(view_dir, a.run), os.path.join(a.out, f"{a.view}_flicker_R0_vs_{a.run}.png")
    elif a.kind == "restart":
        img, stats = restart_sheet(view_dir, a.run, a.cycle)
        path = os.path.join(a.out, f"{a.view}_{a.run}_restarts_cycle{stats['cycle']}.png")
        json.dump(stats, open(os.path.join(view_dir, f"restarts_{a.run}.json"), "w"), indent=1)
        print(json.dumps({r: stats[r]["all"] for r in ("floor", "other")}))
    else:
        img, base = band_sheet(view_dir, a.run, a.cycle)
        path = os.path.join(a.out, f"{a.view}_band_cycle{base}_R0_vs_{a.run}.png")
    img.save(path)
    print(path)


if __name__ == "__main__":
    main()
