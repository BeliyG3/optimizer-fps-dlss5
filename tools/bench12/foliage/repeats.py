"""Identity and repeat checks of foliage cells (phase 2, Task 9c).

  python repeats.py same <dir A> <dir B>
      Byte identity of the dump_*.png (and ofps_acc_*.bin) files the two cell folders share: identical/total, the names
      that differ, and files only one side has. Exit 1 unless every file is identical and both sides have the same set.
  python repeats.py lab <reference dir> <dir>...
      The lab metric of the phase 2 reports (rules\\eval_*.json): PSNR, MAD and temporal MAD of each dir's dumps against
      the reference's (an N = 0 run, the frames it has), and which of the dirs are byte-identical to the first one.
  python repeats.py garden <t0 dir> <mask dir> <dir>...
      PSNR and MAD of each dir's dumps against the t0 cell's over all the frames they share, per region of the vegmask
      cell (foliage, rest: foliage_metrics.regions). For runs without a full-pass cadence (the background mode).
"""
import hashlib, os, sys

import numpy as np
from PIL import Image


def files(d):
    return sorted(f for f in os.listdir(d) if f.startswith(("dump_", "ofps_acc_")))


def digest(path):
    return hashlib.sha256(open(path, "rb").read()).digest()


def same(a, b):
    fa, fb = files(a), files(b)
    shared = sorted(set(fa) & set(fb))
    differ = [f for f in shared if digest(os.path.join(a, f)) != digest(os.path.join(b, f))]
    only = sorted(set(fa) ^ set(fb))
    print(f"byte-identical {len(shared) - len(differ)}/{len(shared)}" + (f"; differ {differ[:8]}" if differ else "") +
          (f"; on one side only {only[:8]}" if only else ""))
    return 0 if shared and not differ and not only else 1


def img(path):
    return np.asarray(Image.open(path).convert("RGB"), np.float64)


def lab(ref, dirs):
    frames = [f for f in files(ref) if f.startswith("dump_")]
    first = [digest(os.path.join(dirs[0], f)) for f in frames]
    for d in dirs:
        mse, mad, tmad, prev = [], [], [], None
        for f in frames:
            a, b = img(os.path.join(d, f)), img(os.path.join(ref, f))
            mse.append(np.mean((a - b) ** 2))
            mad.append(np.mean(np.abs(a - b)))
            if prev is not None:
                tmad.append(np.mean(np.abs((a - prev[0]) - (b - prev[1]))))
            prev = (a, b)
        identical = [digest(os.path.join(d, f)) for f in frames] == first
        print(f"{d}: PSNR {10 * np.log10(255 ** 2 / np.mean(mse)):.3f} dB, MAD {np.mean(mad):.4f}, tMAD {np.mean(tmad):.4f}, "
              f"byte-identical to the first: {identical}")
    return 0


def garden(t0, mask, dirs):
    from foliage_metrics import classes, psnr, regions
    frames = [f for f in files(t0) if f.startswith("dump_") and os.path.exists(os.path.join(mask, f))]
    for d in dirs:
        acc = {r: [0.0, 0.0, 0] for r in ("foliage", "rest")}      # squared and absolute error sums, values
        for f in frames:
            regs = regions(classes(mask, int(f[5:-4])))
            diff = img(os.path.join(d, f)) - img(os.path.join(t0, f))
            for r, a in acc.items():
                e = diff[regs[r]]
                a[0] += float(np.sum(e ** 2))
                a[1] += float(np.sum(np.abs(e)))
                a[2] += e.size
        print(f"{d}: {len(frames)} frames; " + "; ".join(
            f"{r} PSNR {psnr(a[0] / a[2]):.3f} dB, MAD {a[1] / a[2]:.4f}" for r, a in acc.items()))
    return 0


if __name__ == "__main__":
    if len(sys.argv) >= 4 and sys.argv[1] == "same":
        sys.exit(same(sys.argv[2], sys.argv[3]))
    if len(sys.argv) >= 4 and sys.argv[1] == "lab":
        sys.exit(lab(sys.argv[2], sys.argv[3:]))
    if len(sys.argv) >= 5 and sys.argv[1] == "garden":
        sys.exit(garden(sys.argv[2], sys.argv[3], sys.argv[4:]))
    sys.exit(__doc__)
