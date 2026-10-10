#!/usr/bin/env bash
# Bring a title's modules, as Vita3K decrypted them, into fixtures/ and
# measure them.
#
# Vita3K's "Other > Decrypt All SELF" (right-click a title in its app list)
# writes plaintext SELFs for an installed title under its cache directory:
#   <Vita3K dir>/cache/decrypted_selfs/<TITLEID>/eboot.bin, *.self, sce_module/*.suprx
# Those are ordinary armrecomp inputs. This copies them to fixtures/<name>/,
# extracts each to an ELF, and prints the measurements that decide whether
# the title is a bring-up target: static or relocatable, NEON density, the
# import table resolved against the NID database.
#
# usage: tools/import-vita3k.sh <decrypted-title-dir> <name> [build-dir]
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
src="$1"; name="$2"; build="${3:-$root/build}"
arm="$build/tools/armrecomp/armrecomp"
[ -x "$arm" ] || arm="$arm.exe"
[ -x "$arm" ] || { echo "import: armrecomp not found under $build; build first" >&2; exit 1; }
db="$root/external/vita-headers/db/360"

dst="$root/fixtures/$name"
mkdir -p "$dst"
find "$src" -type f \( -iname 'eboot.bin' -o -iname '*.self' -o -iname '*.suprx' \) | while read -r f; do
    rel="${f#$src/}"
    mkdir -p "$dst/$(dirname "$rel")"
    cp "$f" "$dst/$rel"
    echo "== $rel"
    "$arm" info "$dst/$rel" | sed -n '/^ELF/,/^$/p' | head -4
    out="$dst/${rel%.*}.elf"
    if "$arm" extract "$dst/$rel" "$out" > /dev/null 2>&1; then
        "$arm" cover "$out" | grep -E 'thumb|arm |dominant' | head -3
        if [ -d "$db" ]; then
            "$arm" funcs "$out" "$db" | grep -E 'firmware libraries needed|^resolved'
        fi
    else
        echo "   (extract refused: segments still encrypted?)"
    fi
done
