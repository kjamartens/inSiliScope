#!/usr/bin/env bash
# Test-only Linux build of the inSiliScope adapter (the real one is MSBuild on
# Windows): builds build/adapter-linux/libmmgr_dal_inSiliScope.so.0 so the
# device, stages and cell field can be exercised headlessly with pymmcore(-plus)
# (device interface of the mmCoreAndDevices submodule; `pip install pymmcore-plus`).
# No JVM bridge (vectorial PSF models fall back to Gaussian) and no D3D11 GPU path.
#   tools/build_adapter_linux.sh && ADAPTER_DIR=build/adapter-linux python tools/test_cellfield_stage.py
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
A="$ROOT/adapter/inSiliScope"; MM="$ROOT/third_party/mmCoreAndDevices/MMDevice"; CORE="$ROOT/core"
[ -f "$MM/MMDevice.h" ] || { echo "run: git submodule update --init third_party/mmCoreAndDevices" >&2; exit 1; }
OUT="$ROOT/build/adapter-linux"; mkdir -p "$OUT"
${CXX:-g++} -std=c++17 -O2 -fPIC -shared -ffp-contract=off -fno-fast-math \
  -I"$MM" -I"$A" -I"$A/Simulation" -I"$CORE/include" \
  "$A"/*.cpp "$A"/Simulation/*.cpp \
  $(ls "$CORE"/src/*.cpp | grep -v -e jsmath_std.cpp -e wasm_entry.cpp) \
  "$MM"/DeviceUtils.cpp "$MM"/ImgBuffer.cpp "$MM"/MMDevice.cpp "$MM"/ModuleInterface.cpp "$MM"/Property.cpp \
  -o "$OUT/libmmgr_dal_inSiliScope.so.0" -lpthread
echo "built $OUT/libmmgr_dal_inSiliScope.so.0"
