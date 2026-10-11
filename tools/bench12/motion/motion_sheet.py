"""PNG sheet of one view for review: offsets 1 / 4 / 7 of one cycle x four runs (default T0 / R0 / R1 / R2), plus a
row of error heat maps against T0 (the last three runs at offset 7; R0 at the cycle's full pass). Needs the view's
metrics.json (phase, frames).

Usage: python motion_sheet.py <view> [--root DIR] [--out DIR] [--cycle C] [--gain G] [--cols T0,R0,C2,C1]
"""
import argparse, json, os, sys

import numpy as np
from PIL import Image, ImageDraw, ImageFont

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "foliage"))
import bench_paths                                         # noqa: E402

ROOT = bench_paths.out("motion")
COLS = ("T0", "R0", "R1", "R2")
TILE = (960, 540)
LABEL = 44


def heat(err, gain):
    """|luma error| (0..255) -> black, red, yellow, white at err = 64 / gain."""
    x = np.clip(err * gain / 64.0, 0, 1)
    rgb = np.stack([np.clip(3 * x, 0, 1), np.clip(3 * x - 1, 0, 1), np.clip(3 * x - 2, 0, 1)], -1)
    return Image.fromarray((rgb * 255).astype(np.uint8))


def load(view_dir, run, f):
    p = os.path.join(view_dir, run, f"dump_{f}.png")
    return Image.open(p).convert("RGB") if os.path.exists(p) else None


def luma(img):
    a = np.asarray(img, np.float64)
    return a @ np.array([0.2126, 0.7152, 0.0722])


def sheet(view_dir, cycle=None, gain=1.0, cols=COLS):
    m = json.load(open(os.path.join(view_dir, "metrics.json")))
    p, (first, last, _) = m["phase"], m["frames"]
    cycles = list(range((first - p) // 8, (last - p) // 8 + 1))
    c = cycles[len(cycles) // 2] if cycle is None else cycle
    base = p + 8 * c
    font = ImageFont.truetype("arial.ttf", 30)
    out = Image.new("RGB", (TILE[0] * 4, (TILE[1] + LABEL) * 4), (24, 24, 24))
    d = ImageDraw.Draw(out)

    def put(img, col, row, text):
        x, y = col * TILE[0], row * (TILE[1] + LABEL)
        d.text((x + 10, y + 6), text, fill=(235, 235, 235), font=font)
        if img is not None:
            out.paste(img.resize(TILE, Image.Resampling.BILINEAR), (x, y + LABEL))

    for row, o in enumerate((1, 4, 7)):
        for col, r in enumerate(cols):
            put(load(view_dir, r, base + o), col, row, f"{m['view']}  {r}  offset {o}  (frame {base + o})")
    t0_7, t0_0 = load(view_dir, "T0", base + 7), load(view_dir, "T0", base)
    for col, (r, f) in enumerate([(c, base + 7) for c in cols[1:]] + [("R0", base)]):
        img, ref = load(view_dir, r, f), (t0_7 if f == base + 7 else t0_0)
        h = heat(np.abs(luma(img) - luma(ref)), gain) if img is not None and ref is not None else None
        put(h, col, 3, f"|{r} - T0| luma, offset {f - base}, x{gain:g} (white = {64 / gain:.0f}/255)")
    return out, base


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("view")
    ap.add_argument("--root", default=ROOT, required=ROOT is None)
    ap.add_argument("--out", help="default <root>\\sheets")
    ap.add_argument("--cycle", type=int)
    ap.add_argument("--gain", type=float, default=1.0)
    ap.add_argument("--cols", default=",".join(COLS), help="four runs, the first the reference (default T0,R0,R1,R2)")
    a = ap.parse_args()
    cols = tuple(a.cols.split(","))
    if len(cols) != 4:
        ap.error("--cols takes four runs")
    img, base = sheet(os.path.join(a.root, a.view), a.cycle, a.gain, cols)
    a.out = a.out or os.path.join(a.root, "sheets")
    os.makedirs(a.out, exist_ok=True)
    path = os.path.join(a.out, f"{a.view}_cycle{base}" + ("" if cols == COLS else "_" + "_".join(cols)) + ".png")
    img.save(path)
    print(path)


if __name__ == "__main__":
    main()
