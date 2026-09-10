"""errflick.py <refDir> <dir> <first> <last>: per frame, the error against the reference (MAD luma) in the
centre band (80%) and the periphery, and the change of the error map between consecutive frames
(MAD of e_t - e_{t-1}) - the flicker component with the scene motion removed."""
import sys
from PIL import Image
import numpy as np
ref, d, a, b = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
def lum(p):
    img = np.asarray(Image.open(p).convert("RGB")).astype(np.float64)
    return 0.2126*img[...,0]+0.7152*img[...,1]+0.0722*img[...,2]
prevE = None; rows = []
for f in range(a, b+1):
    try: e = lum(f"{d}/dump_{f}.bmp") - lum(f"{ref}/dump_{f}.bmp")
    except Exception: prevE = None; continue
    H, W = e.shape; cw, ch = int(W*0.8), int(H*0.8); cx, cy = (W-cw)//2, (H-ch)//2
    mask = np.zeros_like(e, bool); mask[cy:cy+ch, cx:cx+cw] = True
    ec, ep = np.mean(np.abs(e[mask])), np.mean(np.abs(e[~mask]))
    fl = fc = fp = float('nan')
    if prevE is not None:
        de = e - prevE; fc, fp = np.mean(np.abs(de[mask])), np.mean(np.abs(de[~mask]))
    rows.append((f, ec, ep, fc, fp))
    print(f"{f}: err centre {ec:.3f} periphery {ep:.3f} | error change centre {fc:.3f} periphery {fp:.3f}")
    prevE = e
r = np.array(rows)[1:]
print(f"mean: err centre {r[:,1].mean():.3f} periphery {r[:,2].mean():.3f} | error change centre {np.nanmean(r[:,3]):.3f} (max {np.nanmax(r[:,3]):.3f}) periphery {np.nanmean(r[:,4]):.3f} (max {np.nanmax(r[:,4]):.3f})")
