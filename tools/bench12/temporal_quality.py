"""Run deterministic add-on comparisons; never launches a game or downloads files."""
import argparse
import configparser
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("label")
    parser.add_argument("--runtime", type=Path, default=Path(__file__).parent / "run_addon")
    parser.add_argument("--runs", type=int, default=3)
    parser.add_argument("--frames", type=int, default=168)
    parser.add_argument("--mode", type=int, default=1)
    parser.add_argument("--scenarios", default="follow,forward")
    parser.add_argument("--cadences", default="0,4,8")
    # Extra bench switches for one-off comparisons, e.g. --extra "--depth linear-negative".
    parser.add_argument("--extra", default="")
    args = parser.parse_args()
    runtime = args.runtime.resolve()
    output = runtime / args.label
    output.mkdir(exist_ok=True)
    ini = runtime / "ReShade.ini"
    original = ini.read_bytes()
    config = configparser.ConfigParser(interpolation=None, strict=False)
    config.optionxform = str
    config.read_string(original.decode("utf-8-sig"))
    if not config.has_section("OptimizerFPS"):
        config.add_section("OptimizerFPS")
    section = config["OptimizerFPS"]
    section.update({"Mode": "2", "CrashGuard": "0", "TemporalShowZone": "0",
                    "TemporalSeparateZone": "0", "TemporalDebugLog": "1",
                    "TemporalResidualBlend": "0.6", "TemporalToneSmoothing": "24"})
    records = []
    try:
        for scenario in args.scenarios.split(","):
            for cadence in [int(value) for value in args.cadences.split(",")]:
                for run in range(args.runs):
                    case = output / f"{scenario}_n{cadence}_{run + 1}"
                    case.mkdir(exist_ok=True)
                    section["TemporalMode"] = str(args.mode if cadence else 0)
                    section["TemporalEvery"] = str(cadence or 4)
                    with ini.open("w", encoding="utf-8") as stream:
                        config.write(stream, space_around_delimiters=False)
                    shutil.copy2(ini, case / "ReShade.ini")
                    (runtime / "optimizer-fps-dlss5.session").unlink(missing_ok=True)
                    dumps = list(range(args.frames - 24, args.frames))
                    command = [str(runtime / "pw_bench12.exe"), str(args.frames),
                               "--gltf", "../../bench/assets/lab_scene.glb", "--upscaler", "rr",
                               "--camera", scenario, "--move-speed", "7", "--sun-dir", "0.45,-0.77,0.45",
                               "--sun-strength", "1500", "--exposure", "0.22", "--haze", "0.008",
                               "--fov", "62", *args.extra.split(), "--dump", ",".join(map(str, dumps))]
                    with (case / "stdout.txt").open("w", encoding="utf-8") as stream:
                        result = subprocess.run(command, cwd=runtime, stdout=stream, stderr=subprocess.STDOUT)
                    shutil.copy2(runtime / "ReShade.log", case / "ReShade.log")
                    for frame in dumps:
                        # Windows indexers can briefly deny deletion after a successful copy.
                        # The bench overwrites its working dumps on the next run.
                        shutil.copy2(runtime / f"dump_{frame}.bmp", case / f"dump_{frame}.bmp")
                    text = (case / "stdout.txt").read_text(encoding="utf-8")
                    match = re.search(r"gpu frame time: avg ([\d.]+) ms", text)
                    record = dict(scenario=scenario, cadence=cadence, run=run + 1,
                                  exit=result.returncode, device_ok="device ok" in text,
                                  gpu_ms=float(match[1]) if match else None, command=command)
                    records.append(record)
                    (output / "runs.json").write_text(json.dumps(records, indent=2), encoding="utf-8")
                    print(record, flush=True)
                    if result.returncode or not record["device_ok"]:
                        raise RuntimeError(f"Benchmark failed: {case}")
    finally:
        ini.write_bytes(original)
    files = [runtime / "optimizer-fps-dlss5.addon64", *sorted((runtime / "optimizer-fps-dlss5").glob("*.dxbc"))]
    (output / "binaries.json").write_text(json.dumps({p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in files}, indent=2))


if __name__ == "__main__":
    main()
