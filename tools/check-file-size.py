"""Reports source files that outgrew the size the project works in.

The rule is the user's and applies to every project: 250-300 lines is the signal to look at a file's
structure, 500 lines is where it stops being reviewable. It used to live only in the instructions, so
it survived exactly as long as someone remembered it. Now the build runs this.

    python check-file-size.py [root ...] [--limit 300] [--hard 500] [--changed] [--quiet]
    python check-file-size.py --owned-fork path/to/OptiScaler/optimizer_fps/fg

Exit code 1 when a file reaches the hard limit, 0 otherwise (the soft limit only prints).
Vendored and generated code is skipped: it is not ours to split.
"""
import argparse
import os
import pathlib
import subprocess
import sys

SUFFIXES = {".cpp", ".h", ".hpp", ".inl", ".hlsl", ".hlsli", ".py", ".cs",
            ".ps1", ".psm1", ".cmd", ".bat", ".sh"}
SKIP_PARTS = {"external", "third_party", "out", "dist", "dist-release", "bin", "obj",
              "detours", "imgui", ".git", "run_fork", "run_addon", "run_auto"}
# Code that came from elsewhere and is not ours to split: NVIDIA's optical-flow headers and the
# frame-generation tree lifted whole from the previous fork.
SKIP_DIRS = {"hybrid_nvof", "nvofapi", "nvof", "jfo_hybrid"}


def changed_files() -> list[pathlib.Path]:
    root = pathlib.Path(subprocess.check_output(
        ["git", "rev-parse", "--show-toplevel"], text=True).strip())
    status = subprocess.check_output(
        ["git", "status", "--porcelain=v1", "-z", "--untracked-files=all"], cwd=root)
    entries = status.split(b"\0")
    paths = []
    index = 0
    while index < len(entries) and entries[index]:
        entry = entries[index]
        code = entry[:2]
        # With -z, Git reports the destination first and the old path second.
        paths.append(root / entry[3:].decode("utf-8", errors="surrogateescape"))
        index += 2 if b"R" in code or b"C" in code else 1
    return paths


def source_files(roots: list[str], changed: bool):
    if changed:
        yield from changed_files()
        return
    for root in roots:
        path = pathlib.Path(root)
        if path.is_file():
            yield path
        elif path.is_dir():
            # os.walk skips unreadable directories and dangling links (local bench captures link to
            # removed temporary folders), where Path.rglob raises.
            for folder, _dirs, names in os.walk(path, onerror=lambda _error: None):
                for name in names:
                    yield pathlib.Path(folder) / name


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("roots", nargs="*", default=["."])
    parser.add_argument("--limit", type=int, default=300)
    parser.add_argument("--hard", type=int, default=500)
    parser.add_argument("--changed", action="store_true")
    parser.add_argument("--owned-fork", type=pathlib.Path,
                        help="check hand-written sources under the fork's optimizer_fps tree")
    parser.add_argument("--max-line", type=int, default=500)
    parser.add_argument("--quiet", action="store_true")
    args = parser.parse_args()

    if args.owned_fork is not None:
        if args.changed or args.roots != ["."]:
            parser.error("--owned-fork takes one directory and cannot be combined with roots or --changed")
        fork_root = args.owned_fork.resolve()
        parts = [part.lower() for part in fork_root.parts]
        if not fork_root.is_dir() or not any(
            parts[index:index + 2] == ["optiscaler", "optimizer_fps"]
            for index in range(len(parts) - 1)
        ):
            parser.error("--owned-fork must point within OptiScaler/optimizer_fps")
        args.roots = [str(fork_root)]

    over = []
    long_lines = []
    for path in source_files(args.roots, args.changed):
        if path.suffix.lower() not in SUFFIXES or not path.is_file():
            continue
        parts = {part.lower() for part in path.parts}
        skip_dirs = SKIP_DIRS - {"hybrid_nvof", "jfo_hybrid"} if args.owned_fork else SKIP_DIRS
        if (SKIP_PARTS | skip_dirs) & parts or (args.owned_fork and path.name == "NvofDenseCS.h"):
            continue
        try:
            with path.open(encoding="utf-8", errors="replace") as stream:
                lengths = [len(line.rstrip("\r\n")) for line in stream]
        except OSError:
            continue
        lines = len(lengths)
        if lines > args.limit or lines >= args.hard:
            over.append((lines, path))
        if lengths and max(lengths) > args.max_line:
            long_lines.append((max(lengths), path))

    over.sort(reverse=True)
    hard = [entry for entry in over if entry[0] >= args.hard]
    if over and not args.quiet:
        print(f"[size] {len(over)} file(s) over {args.limit} lines"
              f"{f', {len(hard)} at or above the hard limit of {args.hard}' if hard else ''}:")
        for lines, path in over:
            print(f"[size] {lines:5d}  {path}{'   HARD' if lines >= args.hard else ''}")
    if long_lines and not args.quiet:
        for length, path in sorted(long_lines, reverse=True):
            print(f"[size] {length:5d} char line  {path}   LONG LINE")
    return 1 if hard or long_lines else 0


if __name__ == "__main__":
    sys.exit(main())
