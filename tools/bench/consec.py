"""consec.py <refDir> <dir> <first> <last>: consecutive-frame MAD (luma, 0..255, whole frame) in <dir>,
and PSNR of each frame of <dir> against the same frame of <refDir>."""
import sys
from PIL import Image
import numpy as np
ref, d, a, b = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
def lum(p):
    img = np.asarray(Image.open(p).convert("RGB")).astype(np.float64)
    return 0.2126*img[...,0]+0.7152*img[...,1]+0.0722*img[...,2]
prev = None; mads = []; psnrs = []
for f in range(a, b+1):
    try: cur = lum(f"{d}/dump_{f}.bmp")
    except Exception as e: print(f"{f}: missing"); prev=None; continue
    try:
        r = lum(f"{ref}/dump_{f}.bmp"); mse = np.mean((cur-r)**2); psnr = 10*np.log10(255**2/max(mse,1e-9)); psnrs.append(psnr)
    except Exception: psnr = float('nan')
    if prev is not None:
        m = np.mean(np.abs(cur-prev)); mads.append(m)
        print(f"{f-1}->{f}: MAD {m:.3f}   PSNR(vs ref) {psnr:.2f}")
    prev = cur
if mads:
    mads = np.array(mads); print(f"consecutive MAD mean {mads.mean():.3f}  std {mads.std():.3f}  alternation {np.mean(np.abs(np.diff(mads))):.3f}")
if psnrs: print(f"PSNR mean {np.mean(psnrs):.2f}")
