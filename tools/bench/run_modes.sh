#!/bin/bash
# run_modes.sh <label> <temporal M,N> <extra ini key=value ...> : runs the bench, moves the dumps into run/<label>
# BENCH_RUN: the bench runtime folder (pw_bench.exe, Reshade64 as dxgi.dll, renodx, our add-on, ReShade.ini). Not in git.
O="${BENCH_RUN:-$(cd "$(dirname "$0")" && pwd -W 2>/dev/null || pwd)/run}"
label=$1; temporal=$2; shift 2; set -- "$@" Mode=2 CrashGuard=0
rm -f "$O/optimizer-fps-dlss5.session"   # the bench ends with TerminateProcess; the marker would trip the crash guard
# renodx must have NR on, otherwise feature 18 is never created and every mode yields the same frame
sed -i "s/^NeuralUplift=0/NeuralUplift=1/" "$O/ReShade.ini"
python - "$O/ReShade.ini" "$@" <<'PY'
import sys, io, re
p = sys.argv[1]; s = io.open(p, encoding="utf-8", newline="").read()
nl = "\r\n" if "\r\n" in s else "\n"
head, sec, rest = re.split(r"(\[PeripheralWarp\]" + nl + r")", s, maxsplit=1)[0], None, None
m = re.search(r"\[PeripheralWarp\]" + nl + r"(.*?)(?=" + nl + r"\[|\Z)", s, re.S)
body = m.group(1)
for kv in sys.argv[2:]:
    k, v = kv.split("=", 1)
    if re.search(r"^" + re.escape(k) + r"=.*$", body, re.M): body = re.sub(r"^" + re.escape(k) + r"=.*$", k + "=" + v, body, flags=re.M)
    else: body = body.rstrip(nl) + nl + k + "=" + v
s = s[:m.start(1)] + body + s[m.end(1):]
io.open(p, "w", encoding="utf-8", newline="").write(s)
PY
cd "$O" && rm -f dump_*.bmp && ./pw_bench.exe ${BENCH_FRAMES:-232} --temporal $temporal --fps-cap 60 ${BENCH_EXTRA} --dump ${BENCH_DUMPS:-200,201,202,203,204,205,206,207,208,209,210,211,212,213,214,215,216,217,218,219} 2>&1 | grep -E "frametime|MAD|average|passes|\[fail\]|error" | head -8
mkdir -p "$O/$label" && rm -f "$O/$label"/dump_*.bmp && mv "$O"/dump_*.bmp "$O/$label/" 2>/dev/null
grep -E "temporal|async|forced|stopped" "$O/ReShade.log" | tail -3 | cut -c1-200
