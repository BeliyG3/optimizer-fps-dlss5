"""Step 1b: where the flicker of a view without a vegmask sits (lab_forward_raster's forward flight).

Usage: python motion_split.py <view> <run> [<run> ...] [--root DIR]
  -> <root>\\<view>\\split.json and markdown tables on stdout. R0 is always included.
M2 as in motion_metrics.py: per consecutive pair (t-1, t) the mean |e_t - e_t-1| of the luma error against T0, over the
measured frames of metrics.json. Split by the offset of t (0 = the step from a carried offset 7 onto the full pass) and
by region of frame t. Regions are exclusive, assigned in this order, at the colour dumps' size:
  band   the cycle's union of disocclusion bands (motion_band.py: R1_raw + prevdepth);
  border within BORDER px of the frame edge;
  edge   a view-depth jump above EDGE_REL to a 4-neighbour (R1_raw's depth of t), dilated by DILATE px;
  floor  the horizontal plane: depth within FLOOR_REL of z = 1 / (a y + b), fitted on the row medians of the bottom
         fifth (a level camera without roll sees a floor of constant depth per row);
  near   view depth below NEAR_M metres;
  rest   everything else.
contrib[region][offset] = the region's sum of |de| over the frame's pixel count: the regions add up to the whole M2.
"""
import argparse, json, os, sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "foliage"))
from f16io import read_ofdp                                 # noqa: E402
from foliage_metrics import luma, rgb                       # noqa: E402
from ghost_metrics import resize_nearest, view_depth        # noqa: E402

from motion_band import TRUTH, frame_fields, unpack         # noqa: E402, F401
from motion_metrics import ROOT, discover                   # noqa: E402

N = 8
REGIONS = ("band", "border", "edge", "floor", "near", "rest")
BORDER, EDGE_REL, DILATE, FLOOR_REL, NEAR_M = 48, 0.10, 3, 0.03, 3.0


def dilate(m, r):
    for _ in range(r):
        m = m | np.pad(m[1:], ((0, 1), (0, 0))) | np.pad(m[:-1], ((1, 0), (0, 0))) | \
            np.pad(m[:, 1:], ((0, 0), (0, 1))) | np.pad(m[:, :-1], ((0, 0), (1, 0)))
    return m


def depth_edges(z):
    """True where the view depth jumps by more than EDGE_REL to a 4-neighbour (both sides marked)."""
    e = np.zeros(z.shape, bool)
    for a, b, sa, sb in ((z[1:], z[:-1], np.s_[1:], np.s_[:-1]), (z[:, 1:], z[:, :-1], np.s_[:, 1:], np.s_[:, :-1])):
        j = np.abs(a - b) / np.maximum(np.minimum(a, b), 1e-3) > EDGE_REL
        e[sa] |= j
        e[sb] |= j
    return e


def floor_mask(z):
    """The level floor: 1 / z linear in the row (fit on the bottom fifth's row medians), y below the horizon."""
    h = z.shape[0]
    ys = np.arange(int(0.8 * h), h)
    inv = 1.0 / np.median(z[ys], axis=1)
    a, b = np.polyfit(ys, inv, 1)
    if a <= 0:                                              # no floor rising towards the bottom: none
        return np.zeros(z.shape, bool)
    y = np.arange(h)[:, None]
    model = 1.0 / np.maximum(a * y + b, 1e-6)
    return (a * y + b > 0) & (np.abs(z - model) / z < FLOOR_REL)


def regions_of(z, union, shape):
    """Exclusive region masks of one frame at the colour size `shape`."""
    z = resize_nearest(z, shape)
    # a copy: resize_nearest hands back the cycle's union itself at equal size, and `taken |=` must not grow it
    taken = resize_nearest(union, shape).copy() if union is not None else np.zeros(shape, bool)
    out = {"band": taken.copy()}
    border = np.zeros(shape, bool)
    border[:BORDER], border[-BORDER:], border[:, :BORDER], border[:, -BORDER:] = True, True, True, True
    for name, m in (("border", border), ("edge", dilate(depth_edges(z), DILATE)), ("floor", floor_mask(z)),
                    ("near", z < NEAR_M)):
        out[name] = m & ~taken
        taken |= m
    out["rest"] = ~taken
    return out


def split(view_dir, runs_wanted):
    m = json.load(open(os.path.join(view_dir, "metrics.json")))
    runs, phase = discover(view_dir), m["phase"]
    names = ["R0"] + [r for r in runs_wanted if r != "R0"]
    frames = list(range(m["frames"][0], m["frames"][1] + 1))
    notes = set()
    fields = frame_fields(runs, phase, frames, notes)
    acc = {r: {} for r in names}                            # run -> region -> offset -> [sum of per-pair means, contrib, pairs]
    sq = {r: {} for r in names}                             # run -> region -> offset -> [SSE of the luma error, pixels]
    share = {}
    prev = {}
    for f in frames:
        t0 = luma(rgb(runs["T0"], f))
        o, c = (f - phase) % N, (f - phase) // N
        z = view_depth(read_ofdp(os.path.join(runs[TRUTH], f"depth_{f}.f32"))[1])
        regs = regions_of(z, fields["union"].get(c) if fields else None, t0.shape)
        for g, msk in regs.items():
            share.setdefault(g, []).append(float(msk.mean()))
        for r in names:
            e = luma(rgb(runs[r], f)) - t0
            for g, msk in list(regs.items()) + [("whole", None)]:
                v = e if msk is None else e[msk]
                s = sq[r].setdefault(g, {}).setdefault(o, [0.0, 0])
                s[0] += float(np.sum(v.astype(np.float64) ** 2))
                s[1] += v.size
            if r in prev and prev[r][0] == f - 1:
                d = np.abs(e - prev[r][1])
                for g, msk in list(regs.items()) + [("whole", None)]:
                    v = d if msk is None else d[msk]
                    s = acc[r].setdefault(g, {}).setdefault(o, [0.0, 0.0, 0])
                    s[0] += float(v.mean()) if v.size else 0.0
                    s[1] += float(v.sum()) / d.size
                    s[2] += 1
            prev[r] = (f, e)
    res = {"view": m["view"], "frames": m["frames"], "phase": phase, "share": {g: float(np.mean(v)) for g, v in share.items()},
           "params": dict(border=BORDER, edge_rel=EDGE_REL, dilate=DILATE, floor_rel=FLOOR_REL, near_m=NEAR_M),
           "notes": sorted(notes), "m2": {}, "contrib": {}, "psnr": {}}
    for r in names:
        res["psnr"][r] = {g: {o: float(10 * np.log10(255.0 ** 2 / (s[0] / s[1]))) for o, s in sorted(v.items()) if s[1]}
                          for g, v in sq[r].items()}                    # luma PSNR against T0 per region and offset
        res["m2"][r] = {g: {o: s[0] / s[2] for o, s in sorted(v.items())} for g, v in acc[r].items()}
        res["contrib"][r] = {g: {o: s[1] / s[2] for o, s in sorted(v.items())} for g, v in acc[r].items()}
    return res


def mean(d):
    return float(np.mean(list(d.values()))) if d else float("nan")


def tables(res):
    names = list(res["m2"])
    out = ["region | share | " + " | ".join(f"M2 {r}" for r in names) + " | " +
           " | ".join(f"d contrib {r}" for r in names[1:])]
    for g in REGIONS + ("whole",):
        c0 = mean(res["contrib"]["R0"].get(g, {}))
        out.append(f"{g} | {res['share'].get(g, 1.0):.3f} | " +
                   " | ".join(f"{mean(res['m2'][r].get(g, {})):.3f}" for r in names) + " | " +
                   " | ".join(f"{mean(res['contrib'][r].get(g, {})) - c0:+.4f}" for r in names[1:]))
    out.append("")
    out.append("offset | " + " | ".join(f"whole {r}" for r in names) + " | " +
               " | ".join(f"d {g} {r}" for r in names[1:] for g in REGIONS))
    for o in range(N):
        w = [res["m2"][r]["whole"].get(o, float("nan")) for r in names]
        dc = [res["contrib"][r][g].get(o, 0.0) - res["contrib"]["R0"][g].get(o, 0.0) for r in names[1:] for g in REGIONS]
        out.append(f"{o} | " + " | ".join(f"{x:.3f}" for x in w) + " | " + " | ".join(f"{x:+.4f}" for x in dc))
    out.append("")
    p = res["psnr"]
    p17 = lambda r, g: float(np.mean([p[r][g][o] for o in range(1, N) if o in p[r].get(g, {})]))
    out.append("region | luma PSNR R0 0 / 1-7 | " + " | ".join(f"d {r} 0 / 1-7 / 7" for r in names[1:]))
    for g in REGIONS + ("whole",):
        out.append(f"{g} | {p['R0'][g][0]:.2f} / {p17('R0', g):.2f} | " +
                   " | ".join(f"{p[r][g][0] - p['R0'][g][0]:+.2f} / {p17(r, g) - p17('R0', g):+.2f} / "
                              f"{p[r][g][7] - p['R0'][g][7]:+.2f}" for r in names[1:]))
    return "\n".join(out)


COLOURS = {"band": (255, 0, 255), "border": (110, 110, 110), "edge": (255, 255, 0), "floor": (0, 160, 255),
           "near": (255, 90, 0), "rest": (0, 0, 0)}


def region_png(view_dir, frame, path):
    """T0 of `frame` with the regions tinted (legend in the corner): what the split measures."""
    from PIL import Image, ImageDraw, ImageFont
    m = json.load(open(os.path.join(view_dir, "metrics.json")))
    runs, phase = discover(view_dir), m["phase"]
    c = (frame - phase) // N
    fields = frame_fields(runs, phase, [f for f in range(phase + N * c, phase + N * c + N)], set())
    z = view_depth(read_ofdp(os.path.join(runs[TRUTH], f"depth_{frame}.f32"))[1])
    img = rgb(runs["T0"], frame)
    regs = regions_of(z, fields["union"].get(c) if fields else None, img.shape[:2])
    for g, msk in regs.items():
        img[msk] = img[msk] * 0.45 + np.array(COLOURS[g]) * 0.55
    out = Image.fromarray(img.astype(np.uint8))
    d, font = ImageDraw.Draw(out), ImageFont.truetype("arial.ttf", 30)
    d.text((60, 60), f"{m['view']} frame {frame} (offset {(frame - phase) % N}): split regions", fill=(255, 255, 255), font=font)
    for i, g in enumerate(REGIONS):
        d.rectangle((60, 110 + 40 * i, 90, 140 + 40 * i), fill=COLOURS[g], outline=(255, 255, 255))
        d.text((100, 108 + 40 * i), f"{g} {regs[g].mean() * 100:.1f} %", fill=(255, 255, 255), font=font)
    out.save(path)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("view")
    ap.add_argument("runs", nargs="+")
    ap.add_argument("--root", default=ROOT, required=ROOT is None)
    ap.add_argument("--png", nargs=2, metavar=("FRAME", "PATH"), help="also save the region map of FRAME to PATH")
    a = ap.parse_args()
    view_dir = os.path.join(a.root, a.view)
    if a.png:
        region_png(view_dir, int(a.png[0]), a.png[1])
    res = split(view_dir, a.runs)
    json.dump(res, open(os.path.join(view_dir, "split.json"), "w"), indent=1)
    print(tables(res))


if __name__ == "__main__":
    main()
