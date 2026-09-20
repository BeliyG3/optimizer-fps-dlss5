"""Reports source files that outgrew the size the project works in.

The rule is the user's and applies to every project: 250-300 lines is the signal to look at a file's
structure, 500 lines is where it stops being reviewable. It used to live only in the instructions, so
it survived exactly as long as someone remembered it. Now the build runs this.

    python check-file-size.py [root ...] [--limit 300] [--hard 500] [--quiet]

Exit code 1 when a file is past the hard limit, 0 otherwise (the soft limit only prints).
Vendored and generated code is skipped: it is not ours to split.
"""
import argparse
import pathlib
import sys

SUFFIXES = {".cpp", ".h", ".hpp", ".hlsl", ".hlsli", ".py", ".cs"}
SKIP_PARTS = {"sdk", "external", "third_party", "out", "dist", "dist-release", "bin", "obj",
              "detours", "imgui", ".git", "run_fork", "run_addon", "run_auto"}
# Code that came from elsewhere and is not ours to split: NVIDIA's optical-flow headers and the
# frame-generation tree lifted whole from the previous fork.
SKIP_DIRS = {"hybrid_nvof", "nvofapi", "nvof", "jfo_hybrid"}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("roots", nargs="*", default=["."])
    parser.add_argument("--limit", type=int, default=300)
    parser.add_argument("--hard", type=int, default=500)
    parser.add_argument("--quiet", action="store_true")
    args = parser.parse_args()

    over = []
    for root in args.roots:
        for path in pathlib.Path(root).rglob("*"):
            if path.suffix.lower() not in SUFFIXES:
                continue
            parts = {part.lower() for part in path.parts}
            if (SKIP_PARTS | SKIP_DIRS) & parts:
                continue
            try:
                lines = sum(1 for _ in path.open(encoding="utf-8", errors="ignore"))
            except OSError:
                continue
            if lines > args.limit:
                over.append((lines, path))

    over.sort(reverse=True)
    hard = [entry for entry in over if entry[0] > args.hard]
    if over and not args.quiet:
        print(f"[size] {len(over)} file(s) over {args.limit} lines"
              f"{f', {len(hard)} over the hard limit of {args.hard}' if hard else ''}:")
        for lines, path in over:
            print(f"[size] {lines:5d}  {path}{'   HARD' if lines > args.hard else ''}")
    return 1 if hard else 0


if __name__ == "__main__":
    sys.exit(main())
