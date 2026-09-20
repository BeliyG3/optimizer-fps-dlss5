"""async_age.py RUN_DIR [--first 201 --last 219 --eval-offset 0]

Reads errflick.py output saved as metrics.txt and a DebugAsyncLog=1 ReShade.log.
For a fixed-layout run without resets, separates error growth on carried frames
from error-map change on adoption frames. Values inherit errflick's 0.001 rounding.
Verify the frame/evaluate offset from the frame-10 temporal activation and the
last evaluate before using this tool; the procedural bench used here has offset 0.
"""

import argparse
import re
import statistics
from pathlib import Path


def read_metrics(path):
    pattern = re.compile(
        r"(\d+): err centre ([\d.]+) periphery ([\d.]+) "
        r"\| error change centre ([\d.]+|nan) periphery ([\d.]+|nan)"
    )
    rows = {}
    for line in path.read_text().splitlines():
        match = pattern.match(line)
        if match:
            rows[int(match[1])] = tuple(float(v) for v in match.groups()[1:])
    return rows


def read_ages(path):
    pattern = re.compile(r"\[async\] eval (\d+): .* sinceKick \d+ age (\d+) signalPending")
    rows = {}
    for line in path.read_text(errors="replace").splitlines():
        match = pattern.search(line)
        if match:
            evaluation = int(match[1])
            if evaluation in rows:
                raise ValueError("Multiple async features share an evaluate; use a single-feature run")
            rows[evaluation] = int(match[2])
    return rows


def analyze(metrics, ages, first, last, offset):
    growth, adoption = [], []
    print("frame\tshown_age\terror_c\terror_p\tchange_c\tchange_p")
    for frame in range(first, last + 1):
        # AsyncBody logs before adopting and increments age after presenting. Thus the
        # next evaluate reports age 1 precisely when this frame adopted a new pass.
        age = ages[frame + offset + 1] - 1
        if age < 0:
            raise ValueError("Reset or first-pass startup in the measurement window")
        values = metrics[frame]
        print(frame, age, *values, sep="\t")
        if age == 0:
            adoption.append(values[2:])
        else:
            growth.append(tuple(values[i] - metrics[frame - 1][i] for i in (0, 1)))
    for label, rows in (("carried error growth", growth), ("adoption error-map change", adoption)):
        if not rows:
            raise ValueError(f"No samples for {label}")
        centre, periphery = (statistics.mean(c) for c in zip(*rows))
        print(f"{label}: centre {centre:.4f} periphery {periphery:.4f} ({len(rows)} frames)")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("run_dir", type=Path)
    parser.add_argument("--first", type=int, default=201)
    parser.add_argument("--last", type=int, default=219)
    parser.add_argument("--eval-offset", type=int, default=0)
    args = parser.parse_args()
    if args.first > args.last:
        parser.error("first must not exceed last")
    analyze(read_metrics(args.run_dir / "metrics.txt"), read_ages(args.run_dir / "ReShade.log"),
            args.first, args.last, args.eval_offset)


if __name__ == "__main__":
    main()
