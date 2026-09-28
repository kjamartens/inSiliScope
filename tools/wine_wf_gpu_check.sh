#!/usr/bin/env bash
# The adapter's WideField Direct3D 11 GPU host, checked on Linux: builds
# tests/d3d11/wf_gpu_d3d11_check.cpp with mingw-w64 and runs it under Wine on
# Mesa's lavapipe (software Vulkan) with Microsoft's HLSL compiler (Wine's own
# cannot compile ByteAddressBuffer code). Needs:
#   apt install g++-mingw-w64-x86-64-posix wine64 mesa-vulkan-drivers xvfb xauth
#   d3dcompiler_47.dll next to the exe: D3DCOMPILER=<path> (e.g. from the
#   PyQt5-Qt5 win_amd64 wheel: pip download PyQt5-Qt5 --platform win_amd64
#   --only-binary=:all: --no-deps, then unzip PyQt5/Qt5/bin/d3dcompiler_47.dll)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; S="$ROOT/adapter/inSiliScope/Simulation"
OUT="$ROOT/build/wine-d3d11"; mkdir -p "$OUT"
: "${D3DCOMPILER:?set D3DCOMPILER to a d3dcompiler_47.dll}"
cp "$D3DCOMPILER" "$OUT/d3dcompiler_47.dll"
x86_64-w64-mingw32-g++ -std=c++17 -O2 -DNOMINMAX -I"$S" -I"$ROOT/core/include" -I"$ROOT/third_party/jni" \
  -I"$ROOT/third_party/jni/win32" "$ROOT/tests/d3d11/wf_gpu_d3d11_check.cpp" "$S"/WidefieldGpuD3D11.cpp \
  "$S"/WidefieldRender.cpp "$S"/Fft2d.cpp "$S"/Illumination.cpp "$S"/SMLMNoise.cpp "$S"/CellFieldSource.cpp \
  "$S"/PsfGeneratorBridge.cpp "$S"/SMLMZernike.cpp \
  $(ls "$ROOT"/core/src/*.cpp | grep -v -e jsmath_std.cpp -e wasm_entry.cpp) \
  -o "$OUT/wf_gpu_d3d11_check.exe" -static -ld3d11 -ld3dcompiler -ldxgi
export WINEPREFIX="$OUT/prefix" WINEDEBUG=-all WINEDLLOVERRIDES="d3dcompiler_47=n"
WINE="$(command -v wine64 || echo /usr/lib/wine/wine64)"
"$WINE" reg add 'HKCU\Software\Wine\Direct3D' /v renderer /t REG_SZ /d vulkan /f >/dev/null 2>&1 || true
# The wineserver that command started has no display: stop it, so the run
# below starts one inside the virtual X server.
"$(command -v wineserver || echo /usr/lib/wine/wineserver)" -k 2>/dev/null || true
cd "$OUT" && xvfb-run -a "$WINE" wf_gpu_d3d11_check.exe 2>&1 | grep -v "X connection"
