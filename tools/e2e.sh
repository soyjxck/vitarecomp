#!/usr/bin/env bash
# End to end, headless: the samples from SELF to pixels.
#
#   SELF -> extract -> emit -> compile with the runtime -> run the host with
#   scripted input -> compare the frame it drew against the known one.
#
# The samples are built from samples/* (MIT) and their SELFs are tracked in
# samples/*/prebuilt/, so this needs no vitasdk toolchain: just the toolkit
# build, a C compiler, and the NID database (tools/fetch-deps.sh).
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

# run <sample> <expected frame sha256> <dump vblank> [host args...]
run() {
    local s="$1" expected="$2" at="$3"; shift 3
    local work="$root/build-e2e/$s"
    mkdir -p "$work/recomp"

    echo "== $s: extract"
    "$arm" extract "$root/samples/$s/prebuilt/eboot.bin" "$work/$s.elf" | tail -2
    echo "== $s: emit"
    "$arm" emit "$work/$s.elf" "$work/recomp/recomp_funcs.c" 0 "$db" | sed -n '/^  functions/,/^  trapped/p'

    echo "== $s: build host"
    cmake -S "$root" -B "$work" -DCMAKE_BUILD_TYPE=Release -DVITARECOMP_RECOMP_DIR="$work/recomp" >/dev/null
    cmake --build "$work" --config Release --target vita_host | tail -1
    local host="$work/host/vita_host"
    [ -x "$host" ] || host="$host.exe"
    [ -x "$host" ] || host="$work/host/Release/vita_host.exe"

    echo "== $s: run"
    "$host" "$work/$s.elf" "$@" --dump "$work/frame.ppm@$at" > "$work/stdout.txt"

    local actual
    actual="$(sha256sum "$work/frame.ppm" | cut -d' ' -f1)"
    if [ "$actual" != "$expected" ]; then
        echo "e2e: $s: frame differs: $actual (expected $expected)" >&2
        exit 1
    fi
    echo "== $s: frame matches"
}

# fbdemo at vblank 120: a gradient whose blue channel is the frame counter,
# and the square where sixty frames of RIGHT and thirty of DOWN put it.
run fbdemo 115c519b351b744c71187361eadd4e772e14c4d00025f6a5d51d769a34b3ef1d 120 \
    --frames 240 --press RIGHT@10-70 --press DOWN@30-60 --press START@200

# hello at vblank 30: a console line through sceClibPrintf, snprintf and
# malloc on screen ("FRAME 29  BUTTONS 00000000", the heap size, the malloc
# address).
run hello a29782c6ca2501e306aa5e4a5c59e9b906b48a8215586d3fe3cee127d8f50566 30 \
    --frames 90 --press START@60
grep -q "hello from sceClibPrintf, frame 0, heap 4194304" "$root/build-e2e/hello/stdout.txt" \
    || { echo "e2e: hello: the console line did not reach stdout" >&2; exit 1; }
echo "== hello: stdout matches"

# threads at vblank 5: two workers and the main thread waiting for each other
# by spinning, with no kernel wait anywhere. Four green squares (both started,
# both passed the gate, the counter is exact, the lock is free) and the
# counter as a full-width bar. It is drawn at all only if a thread that spins
# gives way to the one it waits for.
run threads c078b260713119f03b6432f0021d2388ca5b7b8671241add52e467b7922ceaa8 5 \
    --frames 30 --press START@20

echo "e2e: ok"
