"""lowflick.py <refDir> <dir> <first> <last> [block]: low-frequency flicker. Error map e_t = frame - ref (luma),
box-filtered by `block` (default 16); prints the mean |e_t - e_{t-1}| of the low-pass (tone pulsation between
frames) and the mean |e_t| low-pass, whole frame."""
import sys
from PIL import Image
import numpy as np
ref, d, a, b = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
block = int(sys.argv[5]) if len(sys.argv) > 5 else 16
def lum(p):
    img = np.asarray(Image.open(p).convert("RGB")).astype(np.float64)
    return 0.2126*img[...,0]+0.7152*img[...,1]+0.0722*img[...,2]
def low(x):
    H, W = x.shape; H2, W2 = H//block*block, W//block*block
    return x[:H2,:W2].reshape(H2//block, block, W2//block, block).mean(axis=(1,3))
prev = None; ch = []; errs = []
for f in range(a, b+1):
    try: e = low(lum(f"{d}/dump_{f}.bmp") - lum(f"{ref}/dump_{f}.bmp"))
    except Exception: prev = None; continue
    errs.append(np.mean(np.abs(e)))
    if prev is not None: ch.append(np.mean(np.abs(e - prev)))
    prev = e
print(f"low-pass ({block}px) error mean {np.mean(errs):.3f} | low-pass error change between frames mean {np.mean(ch):.3f} max {np.max(ch):.3f}")
