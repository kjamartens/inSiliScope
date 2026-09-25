# insilicell

One C++ world model of a field of cells (cell body, nucleus, cytoplasm, microtubules, dye sites)
for three consumers: the Micro-Manager demoCam device adapter (native), a browser viewer (WASM)
and webSMLM (a CI-generated WASM block). Every cell is a pure function of `(seed, address)`, so any
window of the infinite field can be generated on its own, in any order, on any platform, with
identical results.

Status: **M0 feasibility spike done** ([spec/m0-feasibility.md](spec/m0-feasibility.md)). RNG and
cell packing are ported and bit-identical in JS, native and WASM.

## Layout

```
core/     world model, C++17, no MMDevice/GPU/OS deps; C ABI in core/include/insilicell/
tests/    parity harness against the JS prototype (native + WASM)
tools/    gen_jsmath.py (V8 fdlibm -> core/src/jsmath_fdlibm.cpp)
spec/     algorithm notes and reports
```

## Build and test

```sh
cmake --preset msvc && cmake --build --preset msvc          # native (MSVC 2022 x64)
source ~/emsdk/emsdk_env.sh                                  # Emscripten (tested: 6.0.10)
cmake --preset wasm && cmake --build --preset wasm           # WASM
node tests/parity/run.mjs                                    # parity vs the JS prototype
```
