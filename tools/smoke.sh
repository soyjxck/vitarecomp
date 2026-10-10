#!/usr/bin/env bash
# Run the container stack on the VitaShell fixture: identify, extract, measure.
# Fails if any stage fails or if the extracted ELF is not the declared size.
# usage: tools/smoke.sh [build-dir]   (default: build)
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
build="${1:-$root/build}"
arm="$build/tools/armrecomp/armrecomp"
[ -x "$arm" ] || arm="$arm.exe"
[ -x "$arm" ] || { echo "smoke: armrecomp not found under $build; build first" >&2; exit 1; }

eboot="$root/fixtures/vitashell/eboot.bin"
[ -f "$eboot" ] || { echo "smoke: fixture missing; run tools/fetch-deps.sh" >&2; exit 1; }
elf="$root/fixtures/vitashell/vitashell.elf"

"$arm" info "$eboot"
"$arm" extract "$eboot" "$elf"
"$arm" cover "$elf"

if [ -d "$root/external/vita-headers/db/360" ]; then
    "$arm" funcs "$elf" "$root/external/vita-headers/db/360"
fi
echo "smoke: ok"
