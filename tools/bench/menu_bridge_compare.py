"""Pixel checks of the menu-mode bridge spike (menu_bridge.ps1 cases, spike 0c).

Every add-on dump OUT/<case>_menu_dump_<n>.bmp (the back buffer after the write-back) is compared with the
bench's own dump of the same run and frame, OUT/<case>_dump_<n>.bmp (--dump-back: the back buffer before
Present, i.e. the frame the host drew). In-menu / after-exit comes from <case>_menu_events.log.
  marker: in a menu the 64x64 square at (16,16) is red and every other pixel equals the bench's frame;
          after an exit the whole frame equals it.
  none:   every dump equals the bench's frame (forced failure: the frame untouched).
  model:  after an exit the whole frame equals the bench's; in a menu PSNR against the as-is frame is reported
          (the model changed it), and with --nr-ref CASE the distance of both to that case's in-game NR frame.
usage: menu_bridge_compare.py OUT CASE {marker,none,model} [--nr-ref CASE]
"""
import argparse
import os
import sys

import numpy as np
from PIL import Image


def load(path):
    return np.asarray(Image.open(path).convert('RGB')).astype(np.int16) if os.path.isfile(path) else None


def psnr(a, b):
    mse = np.mean((a.astype(np.float64) - b.astype(np.float64)) ** 2)
    return 99.0 if mse == 0 else 10.0 * np.log10(255.0 ** 2 / mse)


def menu_runs(log):
    runs, dumps, opened = [], [], None
    for line in open(log, encoding='utf-8'):
        f = line.split()
        if not f:
            continue
        if f[0] == 'enter':
            opened = int(f[1])
        elif f[0] in ('exit', 'release', 'resize', 'stop') and opened is not None:
            runs.append((opened, int(f[1])))
            opened = None
        elif f[0] == 'dump':
            dumps.append(int(f[1]))
    if opened is not None:
        runs.append((opened, 1 << 62))
    return runs, dumps


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('out')
    p.add_argument('case')
    p.add_argument('expect', choices=['marker', 'none', 'model'])
    p.add_argument('--nr-ref')
    a = p.parse_args()
    runs, dumps = menu_runs(os.path.join(a.out, a.case + '_menu_events.log'))
    ok, rows = True, {'menu': [], 'after': []}
    for n in dumps:
        menu = any(s <= n < e for s, e in runs)
        mine = load(os.path.join(a.out, '%s_menu_dump_%d.bmp' % (a.case, n)))
        host = load(os.path.join(a.out, '%s_dump_%d.bmp' % (a.case, n)))
        if mine is None or host is None:
            print('MISSING frame %d' % n)
            ok = False
            continue
        diff = np.any(mine != host, axis=2)
        if a.expect == 'marker' and menu:
            square = mine[16:80, 16:80]
            red = bool(np.all(square[..., 0] >= 250) and np.all(square[..., 1:] <= 5))
            outside = diff.copy()
            outside[16:80, 16:80] = False
            good = red and not outside.any()
            rows['menu'].append((n, 'red' if red else 'NOT RED', int(outside.sum())))
        elif a.expect == 'model' and menu:
            db = psnr(mine, host)
            ref = load(os.path.join(a.out, '%s_dump_%d.bmp' % (a.nr_ref, n))) if a.nr_ref else None
            extra = (psnr(mine, ref), psnr(host, ref)) if ref is not None else ()
            good = db < 60.0  # the model changed the frame
            rows['menu'].append((n, round(db, 2)) + tuple(round(x, 2) for x in extra))
        else:
            good = not diff.any()
            rows['menu' if menu else 'after'].append((n, int(diff.sum())))
        ok = ok and good
        if not good:
            print('FAIL frame %d (%s)' % (n, 'menu' if menu else 'after exit'))
    for kind in ('menu', 'after'):
        print('%s %s: %s' % (a.case, kind, rows[kind]))
    if a.expect == 'model' and rows['menu']:
        col = np.array([r[1] for r in rows['menu']])
        line = 'model vs as-is PSNR avg %.2f min %.2f max %.2f' % (col.mean(), col.min(), col.max())
        if a.nr_ref and len(rows['menu'][0]) > 2:
            m, h = np.array([r[2] for r in rows['menu']]), np.array([r[3] for r in rows['menu']])
            line += '; vs in-game NR (%s): model %.2f, as-is %.2f (avg dB)' % (a.nr_ref, m.mean(), h.mean())
        print(line)
    print('%s: %s (%d in menu, %d after exit)' % (a.case, 'OK' if ok else 'FAIL', len(rows['menu']), len(rows['after'])))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
