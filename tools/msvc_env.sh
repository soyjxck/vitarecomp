#!/usr/bin/env bash
# Import the MSVC x64 toolchain environment into Git Bash on Windows so the
# cmake/ninja scripts here (build_tools.sh, recompile.sh, the game build) run
# the same way as on macOS/Linux.
# usage: source tools/msvc_env.sh
vswhere="/c/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe"
vs=$("$vswhere" -latest -products '*' -property installationPath 2>/dev/null | tr -d '\r')
if [ -z "$vs" ]; then
    echo "msvc_env: Visual Studio not found (vswhere)" >&2
    return 1 2>/dev/null || exit 1
fi
# cmd.exe cannot be handed a quoted path through the MSYS argv conversion, so
# go through a throwaway batch file.
bat="$(mktemp --suffix=.bat)"
printf 'call "%s\\VC\\Auxiliary\\Build\\vcvars64.bat" >nul 2>&1 && set\r\n' "$(cygpath -w "$vs")" > "$bat"
while IFS='=' read -r name value; do
    value=${value%$'\r'}
    case "$name" in
        PATH) export PATH="$(cygpath -up "$value")" ;;
        INCLUDE|LIB|LIBPATH|VCINSTALLDIR|VSINSTALLDIR|VCToolsInstallDir|VCToolsVersion|\
        WindowsSdkDir|WindowsSDKVersion|WindowsSdkBinPath|WindowsLibPath|UCRTVersion|UniversalCRTSdkDir)
            export "$name=$value" ;;
    esac
done < <(cmd //c "$(cygpath -w "$bat")")
rm -f "$bat"
export CC=cl CXX=cl
command -v cl >/dev/null || { echo "msvc_env: cl.exe not on PATH after vcvars64" >&2; return 1 2>/dev/null || exit 1; }
