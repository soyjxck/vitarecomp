#!/usr/bin/env bash
# Install the vitasdk toolchain (once) and build samples/fbdemo with it.
#
# Runs inside a Linux shell, WSL included:
#   wsl -d <distro> -- bash /mnt/f/psvitarecomp/tools/wsl-vitasdk.sh
#
# The toolchain goes to $VITASDK (default ~/vitasdk) through vdpm's bootstrap,
# which downloads a prebuilt release; no root is needed. The built eboot.bin is
# copied to fixtures/fbdemo/, which is gitignored like every other module.
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

build="$root/build-sample"
cmake -S "$root/samples/fbdemo" -B "$build" \
      -DCMAKE_TOOLCHAIN_FILE="$VITASDK/share/vita.toolchain.cmake" \
      -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$build"

mkdir -p "$root/fixtures/fbdemo"
cp "$build/eboot.bin" "$root/fixtures/fbdemo/eboot.bin"
cp "$build/fbdemo.vpk" "$root/fixtures/fbdemo/fbdemo.vpk" 2>/dev/null || true
cp "$build/fbdemo" "$root/fixtures/fbdemo/fbdemo.elf"     # the linked ELF, for symbols
echo "fixture: $root/fixtures/fbdemo/eboot.bin ($(wc -c < "$root/fixtures/fbdemo/eboot.bin") bytes)"
