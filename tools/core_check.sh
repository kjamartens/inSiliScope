#!/usr/bin/env bash
# The core's builds and tests in one command (run before committing a core or engine change): native build +
# ctest, then (when emsdk is found) the WASM build + ctest and the viewer module. Stops at the first failure.
#   bash tools/core_check.sh            # native via Ninja (Linux/macOS)
#   ISC_NATIVE=msvc bash tools/core_check.sh   # native via the msvc preset (Windows, Git Bash)
# WASM runs when emsdk is found (~/emsdk or $EMSDK).
set -euo pipefail
cd "$(dirname "$0")/.."
step() { printf '\n== %s\n' "$*"; }

step "native build + ctest"
if [ "${ISC_NATIVE:-}" = msvc ]; then
  cmake --preset msvc && cmake --build --preset msvc && ctest --test-dir build/msvc -C Release -j 8 --output-on-failure
else
  cmake -S . -B build/native -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build/native &&
    ctest --test-dir build/native -j 8 --output-on-failure
fi

EMSDK_ENV="${EMSDK:-$HOME/emsdk}/emsdk_env.sh"
if [ -f "$EMSDK_ENV" ]; then
  step "WASM build + ctest + viewer module"
  # shellcheck disable=SC1090
  source "$EMSDK_ENV" >/dev/null
  cmake --preset wasm && cmake --build --preset wasm && ctest --test-dir build/wasm -j 8 --output-on-failure
  node tools/embed_web_module.mjs
else
  step "WASM skipped (no $EMSDK_ENV); CI's core-wasm job runs it"
fi

printf '\nAll checks passed.\n'
