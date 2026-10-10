#!/usr/bin/env bash
# End to end, headless: the fbdemo sample from SELF to pixels.
#
#   SELF -> extract -> emit -> compile with the runtime -> run the host with
#   scripted input -> compare the frame it drew against the known one.
#
# The sample is built from samples/fbdemo (MIT) and its SELF is tracked in
# samples/fbdemo/prebuilt/, so this needs no vitasdk toolchain: just the
# toolkit build, a C compiler, and the NID database (tools/fetch-deps.sh).
#
# usage: tools/e2e.sh [toolkit-build-dir]   (default: build)
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
build="${1:-$root/build}"
arm="$build/tools/armrecomp/armrecomp"
[ -x "$arm" ] || arm="$arm.exe"
[ -x "$arm" ] || arm="$build/tools/armrecomp/Release/armrecomp.exe"
[ -x "$arm" ] || { echo "e2e: armrecomp not found under $build; build first" >&2; exit 1; }
db="$root/external/vita-headers/db/360"
[ -d "$db" ] || { echo "e2e: NID database missing; run tools/fetch-deps.sh" >&2; exit 1; }

work="$root/build-e2e"
mkdir -p "$work/recomp"

echo "== extract"
"$arm" extract "$root/samples/fbdemo/prebuilt/eboot.bin" "$work/fbdemo.elf" | tail -3
echo "== emit"
"$arm" emit "$work/fbdemo.elf" "$work/recomp/recomp_funcs.c" 0 "$db" | sed -n '/^  functions/,/^$/p'

echo "== build host"
cmake -S "$root" -B "$work" -DCMAKE_BUILD_TYPE=Release -DVITARECOMP_RECOMP_DIR="$work/recomp" >/dev/null
cmake --build "$work" --config Release --target vita_host | tail -1
host="$work/host/vita_host"
[ -x "$host" ] || host="$host.exe"
[ -x "$host" ] || host="$work/host/Release/vita_host.exe"

echo "== run"
"$host" "$work/fbdemo.elf" --frames 240 \
        --press RIGHT@10-70 --press DOWN@30-60 --press START@200 \
        --dump "$work/frame.ppm@120"

# The frame is deterministic: a gradient whose blue channel is the frame
# counter, and the square where 60 frames of RIGHT and 30 of DOWN put it.
expected="115c519b351b744c71187361eadd4e772e14c4d00025f6a5d51d769a34b3ef1d"
actual="$(sha256sum "$work/frame.ppm" | cut -d' ' -f1)"
if [ "$actual" != "$expected" ]; then
    echo "e2e: frame differs: $actual (expected $expected)" >&2
    exit 1
fi
echo "e2e: ok (frame matches)"
