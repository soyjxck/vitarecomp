#!/usr/bin/env bash
# Fetch the run-time data the toolkit loads but does not vendor:
#   external/vita-headers   the MIT NID database (vitasdk), db/<firmware>/*.yml
#   fixtures/vitashell      VitaShell's eboot.bin, a homebrew SELF with plaintext
#                           zlib segments: a key-free input for info/extract/cover
# Both directories are gitignored. Re-running updates in place.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"

mkdir -p "$root/external" "$root/fixtures/vitashell"

if [ -d "$root/external/vita-headers/.git" ]; then
    git -C "$root/external/vita-headers" pull --ff-only --quiet
else
    git clone --quiet --depth 1 https://github.com/vitasdk/vita-headers.git "$root/external/vita-headers"
fi
echo "nid db: $(ls "$root/external/vita-headers/db" | tr '\n' ' ')"

vpk="$root/fixtures/vitashell/VitaShell.vpk"
url="https://github.com/TheOfficialFloW/VitaShell/releases/download/v2.02/VitaShell.vpk"
if [ ! -f "$vpk" ]; then
    curl -sSfL -o "$vpk" "$url"
fi
# A .vpk is a zip; only eboot.bin is needed.
python -I -c "import sys, zipfile; zipfile.ZipFile(sys.argv[1]).extract('eboot.bin', sys.argv[2])" \
    "$vpk" "$root/fixtures/vitashell"
echo "fixture: $root/fixtures/vitashell/eboot.bin ($(wc -c < "$root/fixtures/vitashell/eboot.bin") bytes)"
