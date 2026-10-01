#!/usr/bin/env bash
# The port phase in one command (web/lab/README.md): after porting a prototype change to core/, run every
# build and test that the iteration phase skipped. Stops at the first failure.
#   bash tools/port_check.sh            # native via Ninja (Linux/macOS)
#   ISC_NATIVE=msvc bash tools/port_check.sh   # native via the msvc preset (Windows, Git Bash)
# WASM runs when emsdk is found (~/emsdk or $EMSDK); freeze golden with Node 24 (CLAUDE.md).
set -euo pipefail
cd "$(dirname "$0")/.."
step() { printf '\n== %s\n' "$*"; }

step "golden vectors: re-freeze from web/prototype"
node tests/parity/golden.mjs --freeze
git --no-pager diff --stat -- spec/golden || true

step "lab check"
node web/lab/check.mjs

step "native build + ctest"
if [ "${ISC_NATIVE:-}" = msvc ]; then
  cmake --preset msvc && cmake --build --preset msvc && ctest --test-dir build/msvc -C Release --output-on-failure
else
  cmake -S . -B build/native -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build/native &&
    ctest --test-dir build/native --output-on-failure
fi

EMSDK_ENV="${EMSDK:-$HOME/emsdk}/emsdk_env.sh"
if [ -f "$EMSDK_ENV" ]; then
  step "WASM build + ctest + viewer module + webSMLM block"
  # shellcheck disable=SC1090
  source "$EMSDK_ENV" >/dev/null
  cmake --preset wasm && cmake --build --preset wasm && ctest --test-dir build/wasm --output-on-failure
  node tools/embed_web_module.mjs
  node tools/make_cellfield_block.mjs && node tests/block/check_cellfield_block.mjs
else
  step "WASM skipped (no $EMSDK_ENV); CI's core-wasm job runs it"
fi

step "JS parity report (build/parity/report.md)"
node tests/parity/run.mjs

if [ -f PORT_PENDING.md ]; then
  printf '\nAll checks passed. Delete PORT_PENDING.md in the port commit (CI port-gate blocks the merge until then).\n'
else
  printf '\nAll checks passed.\n'
fi
