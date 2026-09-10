"""compare_dumps.py <dirA> <dirB> <frame> [--zone cx cy cw ch] [--ref]

Compares dump_<frame>.bmp from two bench runs (full-size 3840x2160 BMPs written by pw_bench --dump):
  MAD (0..255) inside the centre zone and outside it, mean luma per region, PSNR, and an
  "edge doubling" score: the correlation of the two images' gradient magnitude maps (1 = same
  edges, lower = displaced/doubled edges). Zone defaults to the 80/80 band.
Frames whose files are missing are skipped. Also compares B[frame-1] vs B[frame] when present."""
import sys
from PIL import Image
import numpy as np

args = [a for a in sys.argv[1:] if not a.startswith("--")]
a_dir, b_dir, frame = args[0], args[1], int(args[2])
zone = None
if "--zone" in sys.argv:
    i = sys.argv.index("--zone")
    zone = tuple(int(v) for v in sys.argv[i + 1:i + 5])


def load(d, f):
    return np.asarray(Image.open(f"{d}/dump_{f}.bmp").convert("RGB")).astype(np.float64)


def lum(img):
    return 0.2126 * img[..., 0] + 0.7152 * img[..., 1] + 0.0722 * img[..., 2]


def grad(l):
    gx = np.abs(np.diff(l, axis=1))[:-1, :]
    gy = np.abs(np.diff(l, axis=0))[:, :-1]
    return np.sqrt(gx * gx + gy * gy)


def stats(x, y, label):
    H, W = x.shape[:2]
    if zone is None:
        cw, ch = int(W * 0.8) & ~1, int(H * 0.8) & ~1
        cx, cy = (W - cw) // 2, (H - ch) // 2
    else:
        cx, cy, cw, ch = zone
    d = np.abs(x - y).mean(axis=2)
    mask = np.zeros(d.shape, bool)
    mask[cy:cy + ch, cx:cx + cw] = True
    lx, ly = lum(x), lum(y)
    mse = ((lx - ly) ** 2).mean()
    psnr = 10 * np.log10(255.0 ** 2 / mse) if mse > 0 else 99.0
    gx_, gy_ = grad(lx), grad(ly)
    m = mask[:-1, :-1]
    def corr(a, b):
        a = a - a.mean(); b = b - b.mean()
        den = np.sqrt((a * a).sum() * (b * b).sum())
        return (a * b).sum() / den if den > 0 else 0.0
    print(f"{label}: MAD centre {d[mask].mean():.3f} periphery {d[~mask].mean():.3f} | PSNR {psnr:.2f} dB | "
          f"edge corr centre {corr(gx_[m], gy_[m]):.4f} periphery {corr(gx_[~m], gy_[~m]):.4f} | "
          f"luma A {lx.mean():.2f} B {ly.mean():.2f}")


try:
    A = load(a_dir, frame)
    B = load(b_dir, frame)
except FileNotFoundError as e:
    print("missing:", e)
    sys.exit(1)
stats(A, B, f"A[{frame}] vs B[{frame}]")
try:
    Bp = load(b_dir, frame - 1)
    stats(Bp, B, f"B[{frame-1}] vs B[{frame}]")
except FileNotFoundError:
    pass
