#!/usr/bin/env bash
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"

PROTON_FILES="${PROTON_FILES:-$HOME/.local/share/Steam/compatibilitytools.d/Proton-FS2024/files}"
XWIN="$HOME/.xwin"
CLANGXX="clang++"
if [[ -x "$HOME/.local/llvm/bin/clang++" ]]; then
  CLANGXX="$HOME/.local/llvm/bin/clang++"
fi

[[ -f "$REPO/build/shaders/nr_resolve_scaled_dxil.h" ]] || { echo "run ./build.sh first"; exit 2; }

"$CLANGXX" -std=c++20 -target x86_64-pc-windows-msvc -fuse-ld=lld -O2 -D_XM_NO_INTRINSICS_ \
  -isystem "$XWIN/crt/include" -isystem "$XWIN/sdk/include/ucrt" \
  -isystem "$XWIN/sdk/include/um" -isystem "$XWIN/sdk/include/shared" \
  -L "$XWIN/crt/lib/x86_64" -L "$XWIN/sdk/lib/um/x86_64" -L "$XWIN/sdk/lib/ucrt/x86_64" \
  -o "$HERE/compose_test.exe" "$HERE/compose_test.cpp"

install -m 0644 "$PROTON_FILES/lib/wine/vkd3d-proton/x86_64-windows/d3d12.dll" \
  "$PROTON_FILES/lib/wine/vkd3d-proton/x86_64-windows/d3d12core.dll" \
  "$PROTON_FILES/lib/wine/dxvk/x86_64-windows/dxgi.dll" "$HERE/"

export WINEPREFIX="${WINEPREFIX:-$HERE/prefix}"
export WINEDLLOVERRIDES="d3d12,d3d12core,dxgi=n;winemenubuilder.exe=d"
export WINEDEBUG="${WINEDEBUG:--all}"
export VKD3D_FILTER_DEVICE_NAME="${VKD3D_FILTER_DEVICE_NAME:-NVIDIA}"
export DXVK_FILTER_DEVICE_NAME="${DXVK_FILTER_DEVICE_NAME:-NVIDIA}"

cd "$HERE"
wine ./compose_test.exe
