#!/bin/bash
# run_cg.sh <tag>: cutegirl head, Blender camera, animation 3x: mouth open/close (frames 262-282), cheek puff (304-324), lip pucker (344-364); every / sync4 / sync8 / async; face-zone metrics.
S="$(cd "$(dirname "$0")" && pwd)"
R="${BENCH_RUN:-$S/run}"
G="${BENCH_GLTF:-$S/assets/external/cutegirl/cutegirl.glb}"   # external asset (Sketchfab, CC BY-NC-SA), not in git
tag=$1
export BENCH_FRAMES=365 BENCH_DUMPS="$(seq -s, 262 282),$(seq -s, 304 324),$(seq -s, 344 364)" BENCH_EXTRA="--gltf $G --camera blender --anim-speed 3"
for m in "every 0,2" "sync4 1,4" "sync8 1,8" "async 3,2"; do set -- $m; "$S/run_modes.sh" ${tag}_$1 $2 >/dev/null; done
Z="1100 700 1200 900"
for d in sync4 sync8 async; do
  for w in "mouth 262 282" "puff 304 324" "pucker 344 364"; do set -- $w
    printf "%-6s %-7s " $d $1; python "$S/zoneflick.py" "$R/${tag}_every" "$R/${tag}_$d" $2 $3 $Z | tr '\n' ' '; python "$S/zoneconsec.py" "$R/${tag}_$d" $2 $3 $Z | tail -1
  done
done
for w in "mouth 262 282" "puff 304 324" "pucker 344 364"; do set -- $w; printf "every  %-7s " $1; python "$S/zoneconsec.py" "$R/${tag}_every" $2 $3 $Z | tail -1; done
