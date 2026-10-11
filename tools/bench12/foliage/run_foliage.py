"""Phase 1 runner of the foliage frame-carry diagnosis (FOLIAGE_DIAG.md).

  python run_foliage.py stage
      Copies this checkout's builds into <root>\\rt_foliage: pw_bench12.exe, its NGX shim and shaders\\bin\\*.cso,
      the add-on payload from out\\build\\x64 (Copy-CorePayload, as deploy_addon.ps1 does), ReShade 6.8 and the NGX
      DLLs from run_nrhost (this checkout's, else bench_paths.json "nrhost"). ReShade.ini is copied only if absent.
  python run_foliage.py run <view> <case> <repeat> [--frames 176] [--first 48] [--neighbours K] [--out DIR]
      Runs one cell under <root>\\bench.lock and moves dump_*.png, motion_*.f32, depth_*.f32 and ofps_acc_*.bin into
      <out>\\<view>\\<case>\\r<repeat>\\ with meta.json, the ini, ReShade.log and the bench's stdout/stderr.
      A previous folder of that cell, and stale dump files in the runtime, move to <out>\\old\\<timestamp>\\.
      --neighbours K adds DebugTemporalMarkerNeighbours=K (rules 3/4's 5x5 threshold) and names the case <case>n<K>.
  python run_foliage.py lab [--rule R] [--neighbours K] [--runs 1] [--out DIR] [--scenarios S --cadences C]
      The lab runs (path traced, temporal_quality.py: follow/forward N = 0, 4, 8 and yaw N = 0, 8, PNG dumps) under
      bench.lock with DebugTemporalMarkerRule=R; outputs move to <out>\\baseline\\lab_*\\ for R = 0 (the phase 1
      baselines) and to <out>\\rules\\r<R>[n<K>]\\lab_*\\ otherwise; an older folder moves to <out>\\old\\<timestamp>\\.
      --scenarios/--cadences run only those, in one folder lab_<scenarios> (e.g. --scenarios forward --cadences 4).
<root> and the scene folder come from tools\\bench12\\bench_paths.json (bench_paths.py).
"""
import argparse, glob, hashlib, json, os, shutil, subprocess, sys, time

import bench_paths

HERE = os.path.dirname(os.path.abspath(__file__))
BENCH = os.path.dirname(HERE)                                   # tools\bench12
REPO = os.path.dirname(os.path.dirname(BENCH))
ROOT = bench_paths.ROOT or ""      # bench_paths.json; every runner's main() calls bench_paths.require_root() first
RT = os.path.join(ROOT, "rt_foliage")
OUT = os.path.join(ROOT, r"out\foliage")
TMP = os.path.join(ROOT, "tmp")
LOCK = os.path.join(ROOT, "bench.lock")
ASSETS = bench_paths.ASSETS
TIMEOUT_S = 900

# run_accept5.py's BASE; every case sets its own keys on top. TemporalModelMarker=1 (classic, 2026.10.2) keeps the
# phase 1 cells and the DebugTemporalMarkerRule cases comparable, TemporalChainRule=0 (classic, phase 3) every earlier
# cell; a key set to None is removed from the ini.
BASE = dict(Mode="2", ColorFilter="2", CenterX="80", CenterY="80", WorkX="90", WorkY="90", GlobalScale="100",
            Flags="1", TemporalMode="0", TemporalEvery="2", ModelPasses="1", SpreadPasses="1", MenuMode="0",
            DebugMenuDump="0", DebugMenuPass="0", DebugTiming="1", CrashGuard="0", DebugLayer="0", DebugWarpPath="0",
            TemporalModelMarker="1", TemporalChainRule="0")
DEBUG_KEYS = ("DebugTemporalVis", "DebugTemporalNoModelMotion", "DebugTemporalDumpAcc", "DebugTemporalMarkerRule",
              "DebugTemporalMarkerNeighbours")   # never leak between cases
T1 = dict(TemporalMode="1", TemporalEvery="8")
CASES = {   # case: (ini keys, bench flags)
    "t0": (dict(TemporalMode="0"), ["--nr", "native"]),
    "t1": (T1, ["--nr", "native"]),
    "t1vis": (dict(T1, DebugTemporalVis="1"), ["--nr", "native", "--tonemap", "none", "--bloom", "0"]),
    "t1nomm": (dict(T1, DebugTemporalNoModelMotion="1"), ["--nr", "native"]),
    "t1acc": (dict(T1, DebugTemporalDumpAcc="1"), ["--nr", "native", "--dump-raw", "motion"]),
    "gt8": ({}, ["--nr", "off", "--motion-span", "8", "--dump-raw", "motion"]),
    "gtdepth": ({}, ["--nr", "off", "--dump-raw", "depth"]),          # dumps from --first - 8: the ghost band needs t-8
    "mask": ({}, ["--nr", "off", "--view", "vegmask"]),
    # DebugTemporalMarkerRule 0..4 (core PwTMarkerReason): t1r0 must equal t1 byte for byte
    **{f"t1r{r}": (dict(T1, DebugTemporalMarkerRule=str(r)), ["--nr", "native"]) for r in range(5)},
    # the shipped default: TemporalModelMarker absent = 2 (narrow), must equal t1r4n8 byte for byte
    "t1def": (dict(T1, TemporalModelMarker=None), ["--nr", "native"]),
}
HELD = ["--cam-pos", "18.3,1.7,-2.5", "--cam-target", "21.0,0.3,-9.5"]
# pan: --camera yaw ignores --cam-pos/--cam-target (camera.h: the override replaces the pose), so the slow pan from
# the held position is the snap camera's in-place turn, one linear 30 degree turn over the 176 frames, wind on.
PAN = ["--camera", "snap", "--snap-deg", "30", "--snap-frames", "176", "--snap-hold", "0"]
COMMON = ["--gltf", rf"{ASSETS}\lab_scene_garden.glb", "--hdri", rf"{ASSETS}\autumn_field_puresky_nosun.hdr",
          "--render", "raster", "--upscaler", "none", "--render-scale", "1", "--width", "1920", "--height", "1080",
          "--vsync", "0", "--dump-format", "png", "--sun-dir", "0.45,-0.77,0.45", "--sun-strength", "1500",
          "--haze", "0.008", "--fov", "62", "--exposure", "0.007", "--auto-exposure", "0", "--csm", "--ao", "clean",
          "--probes", "--lamp-tubes", "--lamp-shadows", "--raster-sky-ambient", "--fxaa"]
# the lab path traced as in docs/dev/temporal-validation.md: RR, render 960x540 for 1920x1080
LAB = ["--gltf", rf"{ASSETS}\lab_scene.glb", "--upscaler", "rr", "--render-scale", "0.5", "--width", "1920", "--height",
       "1080", "--vsync", "0", "--dump-format", "png", "--sun-dir", "0.45,-0.77,0.45", "--sun-strength", "1500",
       "--exposure", "0.22", "--haze", "0.008", "--fov", "62"]
# lab raster as RASTER.md's lab checks; the later flags override LAB's --upscaler/--render-scale in the bench's parser
LAB_RASTER = LAB + ["--render", "raster", "--upscaler", "none", "--render-scale", "1", "--csm", "--ao", "game", "--probes",
                    "--lamp-tubes", "--lamp-shadows", "--fxaa"]
# ghost guard, static camera: Olivia's white suit crosses right to left 1.7 m from a low camera looking up at the dark
# upper wall (big screens, a lamp tube) about 6 m away; --anim-start puts her crossing into the dumps 48-175
GHOST = ["--camera", "static", "--cam-pos", "1.3,0.12,9.0", "--cam-target", "-4.6,5.0,9.0", "--anim-start", "2.36",
         "--anim-playing", "1"]
# second ghost view: Chad (teal scrubs) walks the right bay lane past the camera, 3.2 m away, in front of the flat east
# wall 6.3 m away, eye level, crossing left to right in the dumps 48-175
GHOST_CHAD = ["--camera", "static", "--cam-pos", "3.6,1.5,10.1", "--cam-target", "9.9,1.3,10.1", "--anim-playing", "1"]
# third ghost view, dark figure on a bright panel: Chad walks away down the bay lane and passes in front of the lit white
# roller-shutter door on the east wall (about 7 m away), crossing it obliquely in the dumps 48-175
GHOST_DOOR = ["--camera", "static", "--cam-pos", "5.2,1.3,-3.0", "--cam-target", "8.3,1.2,-9.3", "--anim-start", "16.5",
              "--anim-playing", "1"]
VIEWS = {"held": COMMON + ["--camera", "static"] + HELD + ["--wind"], "pan": COMMON + PAN + HELD + ["--wind"],
         "combo": COMMON + ["--camera", "combo", "--wind"],      # the ghost guard's garden_walkers (window 952-1079)
         # baseline: the held pose in the yard without foliage (the later --gltf wins in the bench's parser)
         "yard": COMMON + ["--camera", "static"] + HELD + ["--wind", "--gltf", rf"{ASSETS}\lab_scene_outdoor.glb"],
         "lab_combo": LAB + ["--camera", "combo"],                # the ghost guard's lab view: the hero walking
         "lab_ghost": LAB + GHOST, "lab_ghost_raster": LAB_RASTER + GHOST,
         "lab_ghost_chad": LAB + GHOST_CHAD, "lab_ghost_chad_raster": LAB_RASTER + GHOST_CHAD,
         "lab_ghost_door": LAB + GHOST_DOOR, "lab_ghost_door_raster": LAB_RASTER + GHOST_DOOR}
OUTPUTS = ("dump_*.png", "motion_*.f32", "depth_*.f32", "ofps_acc_*.bin")


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def set_ini(path, keys, unset):
    """run_accept5.py's set_ini: the [OptimizerFPS] keys sorted, the cache path on W:."""
    lines = open(path, encoding="utf-8-sig").read().splitlines()
    start = lines.index("[OptimizerFPS]")
    end = start + 1
    while end < len(lines) and not lines[end].startswith("["):
        end += 1
    section = {l.split("=", 1)[0]: l for l in lines[start + 1:end] if "=" in l}
    for key, value in keys.items():
        section[key] = f"{key}={value}"
    for key in unset:
        section.pop(key, None)
    body = [section[k] for k in sorted(section)]
    lines = lines[:start + 1] + body + [""] + lines[end:]
    lines = [f"IntermediateCachePath={TMP}" if l.startswith("IntermediateCachePath=") else l for l in lines]
    open(path, "w", encoding="utf-8").write("\n".join(lines) + "\n")


def stage(rt=RT):
    """Stages this checkout's bench and add-on into rt (run_motion.py passes its own runtime folder)."""
    nrhost = os.path.join(BENCH, "run_nrhost")
    nrhost = nrhost if os.path.isdir(nrhost) else bench_paths.NRHOST
    os.makedirs(os.path.join(rt, r"shaders\bin"), exist_ok=True)
    for name in ("pw_bench12.exe", "nvngx.dll_pwbench12.dll"):
        shutil.copy2(os.path.join(BENCH, name), rt)
    for cso in glob.glob(os.path.join(BENCH, r"shaders\bin\*.cso")):
        shutil.copy2(cso, os.path.join(rt, r"shaders\bin"))
    for name in ("dxgi.dll", "nvngx_dlss.dll", "nvngx_dlssd.dll", "nvngx_dlssnr.dll"):
        shutil.copy2(os.path.join(nrhost, name), rt)
    if not os.path.exists(os.path.join(rt, "ReShade.ini")):
        shutil.copy2(os.path.join(nrhost, "ReShade.ini"), rt)
    build = os.path.join(REPO, r"out\build\x64")
    payload = os.path.join(REPO, r"tools\Runtime-Payload.ps1")
    subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-Command",
                    f". '{payload}'; Copy-CorePayload '{build}' '{rt}'"], check=True)
    for name in ("pw_bench12.exe", "optimizer-fps-dlss5.addon64", "optimizer-fps-dlss5-core.dll", "dxgi.dll"):
        print(f"{name}: {sha256(os.path.join(rt, name))[:16]}")
    print(f"staged {rt} (ReShade/NGX from {nrhost})")


def take_lock(label):
    waited = 0
    while True:
        try:
            fd = os.open(LOCK, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
            os.write(fd, f"{label} {time.strftime('%Y-%m-%d %H:%M:%S')} pid {os.getpid()}\n".encode())
            os.close(fd)
            return waited
        except FileExistsError:
            print(f"bench.lock held, waiting ({waited} s)", flush=True)
            time.sleep(30)
            waited += 30


def move_all(src_dir, patterns, dest):
    moved = 0
    for p in patterns:
        for f in glob.glob(os.path.join(src_dir, p)):
            os.makedirs(dest, exist_ok=True)
            shutil.move(f, os.path.join(dest, os.path.basename(f)))
            moved += 1
    return moved


def run(view, case, repeat, frames, first, out, neighbours=0):
    keys, flags = CASES[case]
    keys = dict(BASE, **keys)
    if neighbours:
        keys["DebugTemporalMarkerNeighbours"] = str(neighbours)
        case = f"{case}n{neighbours}"
    unset = [k for k in DEBUG_KEYS if k not in keys] + [k for k, v in keys.items() if v is None]
    keys = {k: v for k, v in keys.items() if v is not None}
    cell = os.path.join(out, view, case, f"r{repeat}")
    exe, ini = os.path.join(RT, "pw_bench12.exe"), os.path.join(RT, "ReShade.ini")
    first = first - 8 if case == "gtdepth" else first
    argv = [exe, str(frames)] + VIEWS[view] + flags + ["--dump", ",".join(map(str, range(first, frames)))]
    os.makedirs(TMP, exist_ok=True)
    waited = take_lock(f"foliage run_foliage.py {view} {case} r{repeat}")
    try:
        stamp = time.strftime("%Y%m%d-%H%M%S")
        old = os.path.join(out, "old", stamp)
        if os.path.exists(cell):
            os.makedirs(os.path.join(old, view, case), exist_ok=True)
            shutil.move(cell, os.path.join(old, view, case, f"r{repeat}"))
        if move_all(RT, OUTPUTS, os.path.join(old, "stale_rt")):
            print(f"stale outputs in {RT} moved to {old}\\stale_rt")
        os.makedirs(cell)
        set_ini(ini, keys, unset)
        t0 = time.time()
        with open(os.path.join(cell, "bench.out.txt"), "w") as so, open(os.path.join(cell, "bench.err.txt"), "w") as se:
            proc = subprocess.Popen(argv, cwd=RT, stdout=so, stderr=se, env=dict(os.environ, TEMP=TMP, TMP=TMP),
                                    creationflags=0x08000000)
            try:
                code = proc.wait(TIMEOUT_S)
            except subprocess.TimeoutExpired:
                proc.kill()                                     # only the PID this script started
                code = "timeout"
        wall = round(time.time() - t0, 1)
        counts = {p: len(glob.glob(os.path.join(RT, p))) for p in OUTPUTS}
        move_all(RT, OUTPUTS, cell)
        shutil.copy2(ini, os.path.join(cell, "ReShade.ini"))
        if os.path.exists(os.path.join(RT, "ReShade.log")):
            shutil.copy2(os.path.join(RT, "ReShade.log"), os.path.join(cell, "ReShade.log"))
        meta = dict(view=view, case=case, repeat=repeat, frames=frames, dumps=[first, frames - 1], argv=argv,
                    ini=keys, unset=unset, exe_sha256=sha256(exe),
                    addon_sha256=sha256(os.path.join(RT, "optimizer-fps-dlss5.addon64")),
                    core_sha256=sha256(os.path.join(RT, "optimizer-fps-dlss5-core.dll")),
                    time=time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(t0)), wall_s=wall, pid=proc.pid,
                    exit=code, lock_wait_s=waited, files=counts)
        json.dump(meta, open(os.path.join(cell, "meta.json"), "w"), indent=1)
    finally:
        os.remove(LOCK)
    print(json.dumps({k: meta[k] for k in ("view", "case", "repeat", "exit", "wall_s", "files")}))
    return 0 if code == 0 else 1


def lab(rule, runs, out, neighbours=0, scenarios=None, cadences=None):
    tq = os.path.join(BENCH, "temporal_quality.py")
    glb = os.path.join(ASSETS, "lab_scene.glb")
    tag = f"r{rule}" + (f"n{neighbours}" if neighbours else "")
    dest = os.path.join(out, "baseline" if tag == "r0" else os.path.join("rules", tag))
    keys = dict(BASE, DebugTemporalMarkerRule=str(rule)) if rule else dict(BASE)
    if neighbours:
        keys["DebugTemporalMarkerNeighbours"] = str(neighbours)
    take_lock(f"foliage run_foliage.py lab rule {rule}")
    code = 0
    try:
        set_ini(os.path.join(RT, "ReShade.ini"), keys, [k for k in DEBUG_KEYS if k not in keys])
        plan = (("lab_fwdfollow", "follow,forward", "0,4,8"), ("lab_yaw", "yaw", "0,8"))
        if scenarios:
            plan = ((f"lab_{scenarios.replace(',', '')}", scenarios, cadences or "0,4,8"),)
        for label, scen, cad in plan:
            for d in (os.path.join(RT, label), os.path.join(dest, label)):
                if os.path.exists(d):
                    shutil.move(d, os.path.join(out, "old", time.strftime("%Y%m%d-%H%M%S"), f"{tag}_{label}"))
            code = subprocess.call([sys.executable, tq, label, "--runtime", RT, "--scenarios", scen, "--cadences", cad,
                                    "--runs", str(runs), "--dump-format", "png", "--extra", f"--gltf {glb} --nr native"],
                                   cwd=BENCH)
            os.makedirs(dest, exist_ok=True)
            shutil.move(os.path.join(RT, label), os.path.join(dest, label))
            print(label, "exit", code, flush=True)
            if code:
                break
        set_ini(os.path.join(RT, "ReShade.ini"), BASE, DEBUG_KEYS)    # no rule left for the next run
    finally:
        os.remove(LOCK)
    return code


def main():
    bench_paths.require_root()
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("stage")
    lb = sub.add_parser("lab")
    lb.add_argument("--rule", type=int, default=0, choices=range(5))
    lb.add_argument("--neighbours", type=int, default=0, choices=range(26))
    lb.add_argument("--runs", type=int, default=1)
    lb.add_argument("--out", default=OUT)
    lb.add_argument("--scenarios")
    lb.add_argument("--cadences")
    r = sub.add_parser("run")
    r.add_argument("view", choices=sorted(VIEWS))
    r.add_argument("case", choices=list(CASES))
    r.add_argument("repeat", type=int, choices=(1, 2))
    r.add_argument("--frames", type=int, default=176)
    r.add_argument("--first", type=int, default=48)
    r.add_argument("--neighbours", type=int, default=0, choices=range(26))
    r.add_argument("--out", default=OUT)
    a = ap.parse_args()
    if a.cmd == "stage":
        stage()
        return 0
    if a.cmd == "lab":
        return lab(a.rule, a.runs, a.out, a.neighbours, a.scenarios, a.cadences)
    return run(a.view, a.case, a.repeat, a.frames, a.first, a.out, a.neighbours)


if __name__ == "__main__":
    sys.exit(main())
