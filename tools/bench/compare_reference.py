"""Compare reference dumps byte-for-byte, or allow an explicit positive RGB MAD."""
import argparse
import pathlib
import sys
import numpy as np
from PIL import Image
def pixels(path):
    with Image.open(path) as image: return np.asarray(image.convert("RGB"),dtype=np.float32)
def compare_pair(reference,candidate):
    if reference.read_bytes()==candidate.read_bytes():
        return dict(status="identical",mad=0.0,max=0.0,share=0.0,shape_a=None,shape_b=None)
    a,b=pixels(reference),pixels(candidate)
    if a.shape!=b.shape:
        return dict(status="size",mad=float("inf"),max=float("inf"),share=1.0,shape_a=a.shape,shape_b=b.shape)
    diff=np.abs(a-b)
    return dict(status="differs",mad=float(diff.mean()),max=float(diff.max()),share=float((diff.max(axis=2)>0).mean()),shape_a=a.shape,shape_b=b.shape)
def report(label,result,limit):
    if result["status"]=="identical": print(f"identical {label}"); return 0
    if result["status"]=="size": print(f"SIZE {label}: {result['shape_a']} vs {result['shape_b']}"); return 2
    passed=limit is not None and limit>0 and result["mad"]<=limit
    print(f"{'within' if passed else 'DIFFERS'} {label}: MAD {result['mad']:.4f} max {result['max']:.0f} pixels {result['share']*100:.3f} %")
    return 0 if passed else 1
def pair(a,b,limit):
    if not a.is_file() or not b.is_file(): print(f"MISSING {a} or {b}"); return 2
    try: return report(str(a),compare_pair(a,b),limit)
    except OSError as e: print(f"UNREADABLE {a}: {e}"); return 2
def compare_dirs(reference_dir,candidate_dir,pattern,mad_limit):
    reference_dir,candidate_dir=pathlib.Path(reference_dir),pathlib.Path(candidate_dir)
    files=sorted(p for p in reference_dir.rglob(pattern) if p.is_file())
    if not files: print(f"MISSING reference files: {reference_dir}/{pattern}"); return 2
    return max(pair(p,candidate_dir/p.relative_to(reference_dir),mad_limit) for p in files)
def main(argv=None):
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("reference",type=pathlib.Path,nargs="?"); p.add_argument("candidate",type=pathlib.Path,nargs="?")
    p.add_argument("--pattern",default="*.bmp"); p.add_argument("--mad-limit",type=float,default=None)
    p.add_argument("--pair",nargs=2,type=pathlib.Path)
    a=p.parse_args(argv)
    if a.mad_limit is not None and (not np.isfinite(a.mad_limit) or a.mad_limit<0): p.error("MAD limit must be finite and nonnegative")
    if a.pair: return pair(*a.pair,a.mad_limit)
    if a.reference is None or a.candidate is None: p.error("REFERENCE_DIR and CANDIDATE_DIR are required")
    return compare_dirs(a.reference,a.candidate,a.pattern,a.mad_limit)
if __name__=="__main__": sys.exit(main())
