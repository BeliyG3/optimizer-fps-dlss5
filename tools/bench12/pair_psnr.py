"""PSNR of two runs' dumps, file by file (menu-mode spike 0b-2a: own block against host block).

A and B are path prefixes: every file A<suffix>.bmp is compared with B<suffix>.bmp, e.g.
  pair_psnr.py OUT/pp_own_menu_dump_ OUT/pp_ref_menu_dump_   (the add-on's dumps of two runs)
  pair_psnr.py OUT/pp_own_dump_ OUT/pp_ref_dump_                  (bench dumps)
  pair_psnr.py ROOT/params_own/dump_ ROOT/off/dump_               (check_plan8_public_optiscaler.py cases)
Both sides must have the same suffixes. Exit 0 when complete and every pair reaches --min-db (default 50).
"""
import argparse
import glob
import os
import sys

import numpy as np
from PIL import Image


def psnr(a, b):
    mse = np.mean((a.astype(np.float64) - b.astype(np.float64)) ** 2)
    return float('inf') if mse == 0 else 10.0 * np.log10(255.0 ** 2 / mse)


def suffixes(prefix):
    return {path[len(prefix):] for path in glob.glob(glob.escape(prefix) + '*.bmp')}


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('a')
    parser.add_argument('b')
    parser.add_argument('--min-db', type=float, default=50.0)
    args = parser.parse_args()
    left, right = suffixes(args.a), suffixes(args.b)
    only = sorted(left ^ right)
    for suffix in only:
        print('MISSING %s' % ((args.b if suffix in left else args.a) + suffix))
    values = []
    key = lambda s: (len(s), s)  # numeric order for the index suffixes
    for suffix in sorted(left & right, key=key):
        with Image.open(args.a + suffix) as x, Image.open(args.b + suffix) as y:
            a, b = np.asarray(x.convert('RGB')), np.asarray(y.convert('RGB'))
        value = psnr(a, b) if a.shape == b.shape else float('-inf')
        values.append(value)
        print('%-24s %s' % (os.path.splitext(suffix)[0], 'identical' if value == float('inf') else '%.2f dB' % value))
    if not values:
        print('no pairs')
        return 2
    finite = [v for v in values if v != float('inf')]
    print('pairs %d, identical %d, min %s, mean of the differing %s; missing %d' % (
        len(values), len(values) - len(finite), 'inf' if not finite else '%.2f dB' % min(finite),
        '-' if not finite else '%.2f dB' % float(np.mean(finite)), len(only)))
    ok = not only and min(values) >= args.min_db
    print('%s (>= %.1f dB on every pair, no missing pair)' % ('PASS' if ok else 'FAIL', args.min_db))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
