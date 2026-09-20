"""Compare captured RGB8 frames, retaining per-run data and median/range summaries."""
import argparse
import json
from pathlib import Path
import statistics

import numpy as np
from PIL import Image


def pixels(path):
    with Image.open(path) as image:
        return np.asarray(image.convert("RGB"), dtype=np.float32)


def compare(case, reference):
    errors, temporal_errors, maximum = [], [], 0.0
    previous, previous_reference = None, None
    for frame in sorted(case.glob("dump_*.bmp"), key=lambda p: int(p.stem.split("_")[1])):
        current, target = pixels(frame), pixels(reference / frame.name)
        errors.append(float(np.abs(current - target).mean()))
        maximum = max(maximum, float(np.abs(current - target).max()))
        if previous is not None:
            temporal_errors.append(float(np.abs((current - previous) - (target - previous_reference)).mean()))
        previous, previous_reference = current, target
    if not errors:
        raise ValueError(f"No captured frames: {case}")
    return {"mad_rgb8": statistics.mean(errors), "temporal_mad_rgb8": statistics.mean(temporal_errors),
            "max_rgb8": maximum, "frames": len(errors)}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("directory", type=Path)
    parser.add_argument("--compare-build", type=Path)
    args = parser.parse_args()
    records = json.loads((args.directory / "runs.json").read_text())
    groups = {}
    for record in records:
        scenario, cadence, run = record["scenario"], record["cadence"], record["run"]
        case = args.directory / f"{scenario}_n{cadence}_{run}"
        reference = ((args.compare_build / f"{scenario}_n{cadence}_1") if args.compare_build
                     else args.directory / f"{scenario}_n0_{run}")
        record.update(compare(case, reference))
        groups.setdefault(f"{scenario}_n{cadence}", []).append(record)
    summary = {}
    for group, runs in groups.items():
        summary[group] = {}
        for key in ("gpu_ms", "mad_rgb8", "temporal_mad_rgb8", "max_rgb8"):
            values = [r[key] for r in runs]
            summary[group][key] = dict(median=statistics.median(values), minimum=min(values), maximum=max(values))
    result = dict(reference=str(args.compare_build or "same-run TemporalMode=0"), runs=records, summary=summary)
    name = "comparison.json" if args.compare_build else "metrics.json"
    (args.directory / name).write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    main()
