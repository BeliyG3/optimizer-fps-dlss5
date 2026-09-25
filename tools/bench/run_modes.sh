#!/bin/bash
# run_modes.sh <label> <temporal M,N> <extra ini key=value ...> : runs the bench, moves the dumps into run/<label>
# BENCH_RUN: the bench runtime folder (pw_bench.exe, Reshade64 as dxgi.dll, renodx, our add-on, ReShade.ini). Not in git.
O="${BENCH_RUN:-$(cd "$(dirname "$0")" && pwd -W 2>/dev/null || pwd)/run}"
label=$1; temporal=$2; shift 2; set -- "$@" Mode=2 CrashGuard=0 Flags=0
rm -f "$O/optimizer-fps-dlss5.session"   # the bench ends with TerminateProcess; the marker would trip the crash guard
# renodx must have NR on, otherwise feature 18 is never created and every mode yields the same frame
sed -i "s/^NeuralUplift=0/NeuralUplift=1/" "$O/ReShade.ini"
python - "$O/ReShade.ini" "$@" <<'PY'
import sys, io, re
p = sys.argv[1]; s = io.open(p, encoding="utf-8", newline="").read()
nl = "\r\n" if "\r\n" in s else "\n"
section = re.search(r"(?m)^\[OptimizerFPS\]\r?$", s)
if section is None:
    s += nl + "[OptimizerFPS]" + nl
    section = re.search(r"(?m)^\[OptimizerFPS\]\r?$", s)
body_start = s.index("\n", section.start()) + 1
next_section = re.search(r"(?m)^\[", s[body_start:])
body_end = body_start + next_section.start() if next_section else len(s)
lines = s[body_start:body_end].splitlines(keepends=True)
for kv in sys.argv[2:]:
    k, v = kv.split("=", 1)
    for index, line in enumerate(lines):
        if re.match(r"^" + re.escape(k) + r"=", line):
            lines[index] = k + "=" + v + (nl if line.endswith("\n") else "")
            break
    else:
        if lines and not lines[-1].endswith("\n"): lines[-1] += nl
        lines.append(k + "=" + v + nl)
s = s[:body_start] + "".join(lines) + s[body_end:]
io.open(p, "w", encoding="utf-8", newline="").write(s)
PY
cd "$O" || exit 1
rm -f dump_*.bmp
./pw_bench.exe ${BENCH_FRAMES:-232} --temporal $temporal --fps-cap 60 ${BENCH_EXTRA} --dump ${BENCH_DUMPS:-200,201,202,203,204,205,206,207,208,209,210,211,212,213,214,215,216,217,218,219} > bench_stdout.log 2>&1
bench_status=$?
grep -E "frametime|MAD|average|passes|\[fail\]|error|bench finished" bench_stdout.log | head -8
mkdir -p "$O/$label" && rm -f "$O/$label"/dump_*.bmp && mv "$O"/dump_*.bmp "$O/$label/" 2>/dev/null
grep -E "temporal|async|forced|stopped" "$O/ReShade.log" | tail -3 | cut -c1-200
exit "$bench_status"
