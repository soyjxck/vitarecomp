#!/usr/bin/env bash
# Install the vitasdk toolchain (once) and build every sample under samples/.
#
# Runs inside a Linux shell, WSL included:
#   wsl -d <distro> -- bash /mnt/<drive>/vitarecomp/tools/wsl-vitasdk.sh [sample...]
#
# The toolchain goes to $VITASDK (default ~/vitasdk) through vdpm's bootstrap,
# which downloads a prebuilt release; no root is needed. Each sample's
# eboot.bin, VPK and symbol-bearing ELF are copied to fixtures/<sample>/,
# which is gitignored like every other module.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
export VITASDK="${VITASDK:-$HOME/vitasdk}"
export PATH="$VITASDK/bin:$PATH"

if [ ! -x "$VITASDK/bin/arm-vita-eabi-gcc" ]; then
    echo "== installing vitasdk into $VITASDK"
    tmp="$(mktemp -d)"
    git clone --quiet --depth 1 https://github.com/vitasdk/vdpm.git "$tmp/vdpm"
    (cd "$tmp/vdpm" && ./bootstrap-vitasdk.sh)
    rm -rf "$tmp"
fi
echo "toolchain: $(arm-vita-eabi-gcc --version | head -1)"

samples=("$@")
if [ ${#samples[@]} -eq 0 ]; then
    for d in "$root"/samples/*/; do samples+=("$(basename "$d")"); done
fi

for s in "${samples[@]}"; do
    src="$root/samples/$s"
    build="$root/build-sample/$s"
    [ -f "$src/CMakeLists.txt" ] || { echo "no sample at $src" >&2; exit 1; }
    echo "== $s"
    cmake -S "$src" -B "$build" \
          -DCMAKE_TOOLCHAIN_FILE="$VITASDK/share/vita.toolchain.cmake" \
          -DCMAKE_BUILD_TYPE=Release >/dev/null
    cmake --build "$build" | grep -E 'error|warning|Built target' || true
    mkdir -p "$root/fixtures/$s"
    cp "$build/eboot.bin" "$root/fixtures/$s/eboot.bin"
    cp "$build/$s.vpk"    "$root/fixtures/$s/$s.vpk" 2>/dev/null || true
    cp "$build/$s"        "$root/fixtures/$s/$s.elf"      # the linked ELF, for symbols
    # Names and a listing, for vita_host --syms and for reading beside the
    # emitted C.
    arm-vita-eabi-nm -n "$build/$s" > "$root/fixtures/$s/$s.sym"
    arm-vita-eabi-objdump -d --no-show-raw-insn "$build/$s" > "$root/fixtures/$s/$s.dis"
    echo "fixture: fixtures/$s/eboot.bin ($(wc -c < "$root/fixtures/$s/eboot.bin") bytes), $s.sym, $s.dis"
done
