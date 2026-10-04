"""PSNR tables of the menu-mode model spike (reference_dumps.ps1 cases mp_model*, mp_ref*, mp_off_*).

In-menu dumps (the add-on's menu_dump after its write-back) and the first five presents after each exit
are compared with the reference run's bench dumps at the same present index (bench frame == present
index). Optional baselines: the pause without menu mode ('off': the host's own frames, bench dumps).

usage: menu_psnr.py OUT MODEL REF [--off CASE] [--frozen CASE] [--criteria]
"""
import argparse
import os
import sys

import numpy as np
from PIL import Image


def psnr(a, b):
    mse = np.mean((a.astype(np.float64) - b.astype(np.float64)) ** 2)
    return 99.0 if mse == 0 else 10.0 * np.log10(255.0 ** 2 / mse)


def load(path):
    return np.asarray(Image.open(path).convert('RGB')) if os.path.isfile(path) else None


def parse_log(path):
    """Menu runs [(enter, end)] closed by exit/release/resize/stop, and the dumped present indices."""
    runs, dumps, open_at = [], [], None
    with open(path, encoding='utf-8') as f:
        for line in f:
            fields = line.split()
            if not fields:
                continue
            kind = fields[0]
            if kind == 'enter':
                open_at = int(fields[1])
            elif kind in ('exit', 'release', 'resize', 'stop') and open_at is not None:
                runs.append((open_at, int(fields[1])))
                open_at = None
            elif kind == 'dump':
                dumps.append(int(fields[1]))
    if open_at is not None:
        runs.append((open_at, 1 << 62))
    return runs, dumps


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('out')
    parser.add_argument('model')
    parser.add_argument('ref')
    parser.add_argument('--off')
    parser.add_argument('--frozen')
    parser.add_argument('--criteria', action='store_true', help='check the 0b-1b thresholds (static camera)')
    a = parser.parse_args()
    runs, dumps = parse_log(os.path.join(a.out, a.model + '_menu_events.log'))
    bench = lambda case, i: load(os.path.join(a.out, '%s_dump_%d.bmp' % (case, i))) if case else None
    menu = lambda i: load(os.path.join(a.out, '%s_menu_dump_%d.bmp' % (a.model, i)))
    inside, after, missing = [], [], 0
    for i in dumps:
        run = next((k for k, (s, e) in enumerate(runs) if s <= i < e), None)
        ref, mine = bench(a.ref, i), menu(i)
        others = {name: bench(getattr(a, name), i) for name in ('off', 'frozen')}
        absent = [n for n, img in (('ref', ref), ('model', mine), ('off', others['off']), ('frozen', others['frozen']))
                  if img is None and (n in ('ref', 'model') or getattr(a, n))]
        if absent:  # every pair must exist: a partial sample would pass as the whole run
            print('frame %d: MISSING %s' % (i, ', '.join(absent)))
            missing += 1
            continue
        row = {'index': i, 'model': psnr(mine, ref)}
        for name, other in others.items():
            row[name] = psnr(other, ref) if other is not None else None
        if run is not None:
            row['run'] = run + 1
            inside.append(row)
        else:
            prior = max((k for k, (s, e) in enumerate(runs) if e <= i), default=None)
            row['run'] = prior + 1 if prior is not None else 0
            row['pos'] = i - runs[prior][1] + 1 if prior is not None else 0
            after.append(row)
    fmt = lambda v: '   -  ' if v is None else '%6.2f' % v
    print('In-menu dumps (%s vs %s, dB); off = the host frame shown without menu mode, frozen = last NR output' % (a.model, a.ref))
    print(' run  frame   model    off  frozen')
    for r in inside:
        print('%4d %6d  %s %s %s' % (r['run'], r['index'], fmt(r['model']), fmt(r['off']), fmt(r['frozen'])))
    print('After exit (first five presents of each resumed host run)')
    print(' run  pos  frame   model    off  model-off')
    for r in after:
        delta = None if r['off'] is None else r['model'] - r['off']
        print('%4d %4d %6d  %s %s  %s' % (r['run'], r['pos'], r['index'], fmt(r['model']), fmt(r['off']), fmt(delta)))
    summary = lambda rows, key: (min(r[key] for r in rows), float(np.mean([r[key] for r in rows]))) if rows and all(r[key] is not None for r in rows) else (None, None)
    im, am = summary(inside, 'model'), summary(after, 'model')
    print('summary: in-menu model min %s mean %s; after-exit model min %s mean %s' % (fmt(im[0]), fmt(im[1]), fmt(am[0]), fmt(am[1])))
    if a.off:
        io, ao = summary(inside, 'off'), summary(after, 'off')
        worst = min((r['model'] - r['off'] for r in after if r['off'] is not None), default=None)
        print('summary: in-menu off min %s mean %s; after-exit off min %s mean %s; worst after-exit model-off %s' %
              (fmt(io[0]), fmt(io[1]), fmt(ao[0]), fmt(ao[1]), fmt(worst)))
    complete = missing == 0 and len(inside) == len(runs) and len(after) == 5 * len(runs)
    print('pairs: %d in-menu, %d after-exit over %d runs; %d missing -> %s' %
          (len(inside), len(after), len(runs), missing, 'complete' if complete else 'INCOMPLETE'))
    if a.criteria:
        worst = min((r['model'] - r['off'] for r in after if r['off'] is not None), default=None)
        checks = [('in-menu >= 35 dB', im[0] is not None and im[0] >= 35.0),
                  ('after-exit >= 35 dB', am[0] is not None and am[0] >= 35.0),
                  ('after-exit not more than 1 dB below off', worst is not None and worst >= -1.0),
                  ('10 in-menu and 50 after-exit dumps', len(inside) == 10 and len(after) == 50)]
        for name, ok in checks:
            print('criterion %-42s %s' % (name, 'PASS' if ok else 'FAIL'))
        if not all(ok for _, ok in checks):
            return 1
    return 0 if complete else 2  # a missing pair fails the comparison, criteria or not


if __name__ == '__main__':
    sys.exit(main())
