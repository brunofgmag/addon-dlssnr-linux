#!/usr/bin/env bash
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"

RENODX="${RENODX:-$HOME/projects/renodx}"
XWIN="$HOME/.xwin"
CLANGXX="clang++"
if [[ -x "$HOME/.local/llvm/bin/clang++" ]]; then
  CLANGXX="$HOME/.local/llvm/bin/clang++"
fi

[[ -f "$REPO/build/shaders/nr_resolve_scaled_dxil.h" ]] || { echo "run ./build.sh first"; exit 2; }

"$CLANGXX" -std=c++20 -target x86_64-pc-windows-msvc -fuse-ld=lld -O2 \
  -isystem "$XWIN/crt/include" -isystem "$XWIN/sdk/include/ucrt" \
  -isystem "$XWIN/sdk/include/um" -isystem "$XWIN/sdk/include/shared" \
  -I "$RENODX/external/DLSS/include" \
  -L "$XWIN/crt/lib/x86_64" -L "$XWIN/sdk/lib/um/x86_64" -L "$XWIN/sdk/lib/ucrt/x86_64" \
  -o "$HERE/follow_test.exe" "$HERE/follow_test.cpp"

export WINEPREFIX="${WINEPREFIX:-$REPO/test/compose/prefix}"
export WINEDEBUG="${WINEDEBUG:--all}"
export WINEDLLOVERRIDES="winemenubuilder.exe=d"
cd "$HERE"
wine ./follow_test.exe
