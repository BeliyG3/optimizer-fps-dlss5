"""CPU per menu present from a menu_events.log (menu-mode spike): the 't' lines carry, per menu present,
the whole present, the NGX evaluate recording and the pipeline's own share (ms); 'slowparts' lines split a
pipeline share of 1 ms or more into its parts.

usage: menu_cpu.py LOG [LOG...]
"""
import sys

import numpy as np


def stats(values):
    v = np.asarray(values)
    return 'avg %.3f  p95 %.3f  max %.3f  >=1ms %d' % (v.mean(), np.percentile(v, 95), v.max(), int((v >= 1.0).sum()))


def main():
    status = 0
    for path in sys.argv[1:]:
        rows, slow = [], []
        with open(path, encoding='utf-8') as f:
            for line in f:
                fields = line.split()
                if fields[:1] == ['t']:
                    rows.append([float(x) for x in fields[2:5]] + [int(fields[5])])
                elif fields[:1] == ['slowparts']:
                    slow.append(line.strip())
        if not rows:
            print('%s: no per-present lines' % path)
            status = 1
            continue
        total, evaluate, pipe, nth = zip(*rows)
        print('%s: %d menu presents' % (path, len(rows)))
        print('  whole present       %s' % stats(total))
        print('  evaluate recording  %s' % stats(evaluate))
        print('  pipeline (no eval)  %s' % stats(pipe))
        first = [p for p, n in zip(pipe, nth) if n == 1]
        rest = [p for p, n in zip(pipe, nth) if n != 1]
        if first and rest:
            print('  pipeline, 1st present of a run  %s' % stats(first))
            print('  pipeline, other presents        %s' % stats(rest))
        for line in slow:
            print('  ' + line)
    return status


if __name__ == '__main__':
    sys.exit(main())
