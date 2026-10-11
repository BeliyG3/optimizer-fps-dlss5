"""Local folders of the bench scripts, read from tools\\bench12\\bench_paths.json (not tracked: copy
bench_paths.example.json next to it and fill it in).

  root    the bench folder: runtimes (rt_*), outputs (out\\foliage, out\\motion), tmp, cache and bench.lock
  assets  optional: the bench scenes (default: this checkout's tools\\bench\\assets)
  nrhost  optional: ReShade + NGX DLLs used when this checkout has no tools\\bench12\\run_nrhost
Without the file the metric scripts need --root and the runners stop with a message.
"""
import json, os

BENCH = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))             # tools\bench12
CONFIG = os.path.join(BENCH, "bench_paths.json")
_cfg = json.load(open(CONFIG, encoding="utf-8")) if os.path.exists(CONFIG) else {}
ROOT = _cfg.get("root")
ASSETS = _cfg.get("assets") or os.path.join(os.path.dirname(os.path.dirname(BENCH)), "tools", "bench", "assets")
NRHOST = _cfg.get("nrhost")


def out(name):
    """<root>\\out\\<name>, or None without a configured root (the caller's --root is then required)."""
    return os.path.join(ROOT, "out", name) if ROOT else None


def require_root():
    if not ROOT:
        raise SystemExit(f"bench root not set: copy {os.path.join(BENCH, 'bench_paths.example.json')} to {CONFIG}"
                         " and set \"root\"")
    return ROOT
