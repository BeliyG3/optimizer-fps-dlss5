"""A short run's dumps against the full run's (README "Short runs"): byte identity, PSNR per frame and per cadence offset.

  python short_compare.py FULL_CELL SHORT_CELL [--phase P] [--json FILE]

Frames in both cells are compared (RGB, 8 bit). Offsets are (frame - P) mod 8 (P: the view's R0 phase, default 1), each
from the mean MSE over its frames; "first8" is the same over the window's first 8 frames alone. "settled_from" is the
first frame from which every later frame reaches 50 dB (None: never). Exit 0.
"""
import argparse, hashlib, json, math, os, re, sys

import numpy as np
from PIL import Image

N = 8
SETTLED_DB = 50.0


def frames_of(d):
    return {int(m.group(1)): os.path.join(d, n) for n in os.listdir(d) if (m := re.fullmatch(r"dump_(\d+)\.png", n))}


def digest(path):
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


def mse(a, b):
    x = np.asarray(Image.open(a).convert("RGB"), np.float64)
    y = np.asarray(Image.open(b).convert("RGB"), np.float64)
    return float(np.mean((x - y) ** 2))


def db(m):
    return math.inf if m == 0 else 10 * math.log10(255.0 ** 2 / m)


def by_offset(errors, frames, phase):
    out = {}
    for k in range(N):
        sel = [errors[f] for f in frames if (f - phase) % N == k]
        if sel:
            out[k] = round(db(sum(sel) / len(sel)), 2)
    return out


def compare(full, short, phase=1):
    a, b = frames_of(full), frames_of(short)
    frames = sorted(set(a) & set(b))
    if not frames:
        raise ValueError(f"no common dump frames in {full} and {short}")
    same = {f for f in frames if digest(a[f]) == digest(b[f])}
    errors = {f: 0.0 if f in same else mse(a[f], b[f]) for f in frames}
    settled = next((f for i, f in enumerate(frames) if all(db(errors[g]) >= SETTLED_DB for g in frames[i:])), None)
    return {"full": full, "short": short, "frames": [frames[0], frames[-1], len(frames)], "identical": len(same),
            "psnr_all": round(db(sum(errors.values()) / len(frames)), 2), "offsets": by_offset(errors, frames, phase),
            "first8": by_offset(errors, frames[:N], phase), "settled_from": settled,
            "per_frame": {f: round(db(errors[f]), 2) for f in frames}}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("full")
    ap.add_argument("short")
    ap.add_argument("--phase", type=int, default=1)
    ap.add_argument("--json")
    a = ap.parse_args()
    res = compare(a.full, a.short, a.phase)
    text = json.dumps(res, indent=1, default=str)
    if a.json:
        open(a.json, "w").write(text)
    print(json.dumps({k: v for k, v in res.items() if k != "per_frame"}, default=str))
    return 0


if __name__ == "__main__":
    sys.exit(main())
