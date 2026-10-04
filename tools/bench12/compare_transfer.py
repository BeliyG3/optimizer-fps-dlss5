"""Detail transfer on bench12: each case against full-resolution NR on the same frames."""
import argparse
import re
from pathlib import Path

import numpy as np
from PIL import Image

CASES = ["uni50_f1", "uni50_f2", "uni50_f3", "uni70_f1", "uni70_f3", "uni85_f1", "uni85_f3",
         "per_f1", "per_f3", "uni50_f3_cl", "uni50_f3_t1", "per_f1_r10", "per_f3_r10"]


def load(path):
    return np.asarray(Image.open(path).convert("RGB"), dtype=np.float64)


def psnr(a, b):
    mse = np.mean((a - b) ** 2)
    return 99.0 if mse == 0 else 10.0 * np.log10(255.0 ** 2 / mse)


def laplacian(img):
    g = img.mean(axis=2)
    return np.abs(4 * g[1:-1, 1:-1] - g[:-2, 1:-1] - g[2:, 1:-1] - g[1:-1, :-2] - g[1:-1, 2:])


def last_float(path, pattern):
    if not path.exists():
        return float("nan")
    values = re.findall(pattern, path.read_text(errors="replace"))
    return float(values[-1]) if values else float("nan")


def post_ms(stdout):
    """GPU frame time minus path tracing: everything after the render, NR and our passes included."""
    m = re.findall(r"gpu frame time: avg ([0-9.]+) ms \(path trace ([0-9.]+) ms\)",
                   stdout.read_text(errors="replace")) if stdout.exists() else []
    return float(m[-1][0]) - float(m[-1][1]) if m else float("nan")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("out", type=Path, help="capture root (reference_dumps -Out, or the plan-8 output root)")
    parser.add_argument("--frames", default="120,239")
    parser.add_argument("--reference", default="off_t0_time")
    parser.add_argument("--depth-case", default="depthview", help="case whose dumps are the depth view ('' = none)")
    parser.add_argument("--depth-step", type=float, default=2.0,
                        help="depth-view levels between neighbours that count as a silhouette")
    parser.add_argument("--cases", nargs="*", default=CASES)
    parser.add_argument("--flat", action="store_true",
                        help="plan-8 layout: <root>/<case>/dump_<frame>.bmp, ReShade.log, stdout.txt")
    args = parser.parse_args()
    frames = args.frames.split(",")

    def dump(case, frame):
        return args.out / case / f"dump_{frame}.bmp" if args.flat else args.out / f"{case}_dump_{frame}.bmp"

    def log(case):
        return args.out / case / "ReShade.log" if args.flat else args.out / f"{case}_ReShade.log"

    def stdout(case):
        return args.out / case / "stdout.txt" if args.flat else args.out / f"{case}.txt"

    def silhouettes(frame):
        if not args.depth_case or not dump(args.depth_case, frame).exists():
            return None
        # bench12's depth view spans only ~13 of 256 levels (240..253), one level being banding on the
        # floor: a silhouette is a step of at least --depth-step levels between neighbours.
        d = load(dump(args.depth_case, frame)).mean(axis=2)
        dx = np.abs(np.diff(d, axis=1)) >= args.depth_step
        dy = np.abs(np.diff(d, axis=0)) >= args.depth_step
        mask = np.zeros(d.shape, dtype=bool)
        mask[:, 1:] |= dx
        mask[:, :-1] |= dx
        mask[1:, :] |= dy
        mask[:-1, :] |= dy
        # Every pixel within 2 px (5x5 square) of a depth step; padded with False, so nothing wraps
        # across opposite image edges.
        r = 2
        padded = np.pad(mask, r, constant_values=False)
        h, w = mask.shape
        mask = np.logical_or.reduce([padded[r + dy:r + dy + h, r + dx:r + dx + w]
                                     for dy in range(-r, r + 1) for dx in range(-r, r + 1)])
        return mask[1:-1, 1:-1]

    print(f"{'case':16} {'post ms':>8} {'model ms':>9} {'PSNR dB':>8} {'detail':>7} {'silh err':>9}")
    print(f"{args.reference:16} {post_ms(stdout(args.reference)):8.2f} "
          f"{last_float(log(args.reference), r'model time: ([0-9.]+) ms'):9.2f} {'ref':>8} {'1.000':>7} {'-':>9}")
    for case in args.cases:
        rows = []
        for f in frames:
            ref, got = load(dump(args.reference, f)), load(dump(case, f))
            lr, lg = laplacian(ref), laplacian(got)
            err = np.abs(got - ref).mean(axis=2)[1:-1, 1:-1]
            mask = silhouettes(f)
            silh = float(err[mask].mean()) if mask is not None and mask.any() else float("nan")
            rows.append((psnr(ref, got), lg.mean() / max(lr.mean(), 1e-9), silh))
        columns = [np.array([r[i] for r in rows]) for i in range(3)]
        p, d, s = (float(np.nanmean(c)) if np.isfinite(c).any() else float("nan") for c in columns)
        print(f"{case:16} {post_ms(stdout(case)):8.2f} "
              f"{last_float(log(case), r'model time: ([0-9.]+) ms'):9.2f} {p:8.2f} {d:7.3f} {s:9.2f}")


if __name__ == "__main__":
    main()
