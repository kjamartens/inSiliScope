# insiliscope

One C++ world model of a field of cells (cell body, nucleus, cytoplasm, microtubules, dye sites)
for three consumers: the Micro-Manager inSiliScope device adapter (native), a browser viewer (WASM)
and webSMLM (a CI-generated WASM block). Every cell is a pure function of `(seed, address)`, so any
window of the infinite field can be generated on its own, in any order, on any platform, with
identical results.

Status: **M0 (feasibility) and M1 (repo restructure) done**; see [PLAN.md](PLAN.md) for the plan and
checklist. RNG and cell packing are ported and bit-identical in JS, native and WASM
([spec/m0-feasibility.md](spec/m0-feasibility.md)).

## inSiliScope (Micro-Manager device adapter)

A synthetic SMLM (Single-Molecule Localization Microscopy) camera device
adapter for [Micro-Manager](https://micro-manager.org/). It generates
blinking-fluorophore movies -- point-spread functions rendered on a
pixel grid, with realistic camera noise -- that resolve into a chosen pattern
over many frames. Useful for demoing or testing SMLM analysis pipelines
inside Micro-Manager without real hardware.

- **Live** (compute-as-you-go, default): continuously simulates frames on a
  background thread, with density, intensity, pattern, and noise parameters
  adjustable in real time while streaming.
- **Precomputed stack**: generates a fixed-length, reproducible movie (given
  a `SimType_RandomSeed`) up front, then serves frames from it during Snap/Live/
  sequence acquisition. Good for benchmarking analysis pipelines against a
  known ground truth.

Emitter density, photon emission rate, ON-lifetime, and background are all
expressed as physical rates (per second) and automatically scale with
whatever the standard MM `Exposure` is set to.

Built-in patterns: `Circle`, `Lines`, `Grid`, `Random`, `Spiral`, `Star`,
`Heart`, `ResolutionTarget`, and `CustomPoints` (loaded from a CSV file of
normalized `x,y` coordinates via the `SimType_CustomPointsFile` property). `Circle`,
`Spiral`, `Star`, and `Heart` are each rendered as concentric double-line
outlines whose gap shrinks step to step (500 down to 10 nm), and
`ResolutionTarget` lays the same spacing sequence out as a 3x3 chart --
together, built-in resolution tests for the current PSF/pixel-size/density
settings. See
[Simulation/SMLMPatterns.h](adapter/inSiliScope/Simulation/SMLMPatterns.h)
to add more.

See [docs/BUILD_AND_USAGE.md](docs/BUILD_AND_USAGE.md) for full adapter build and
usage instructions (Windows/Visual Studio 2022).

## Layout

```text
core/                      world model, C++17, no MMDevice/GPU/OS deps; C ABI in core/include/insiliscope/
adapter/inSiliScope/       the MM device adapter (MSBuild, builds mmgr_dal_inSiliScope.dll)
  Simulation/              the SMLM render engine (PSF, camera noise, GPU path) -- no MMDevice dependency
web/                       cell-field viewer (the JS prototype; moves onto the WASM core in M3)
spec/                      algorithm notes, port spec, golden vectors, reports
tests/                     golden-vector and parity harness (native + WASM)
tools/                     gen_jsmath.py, adapter_pixel_hash.py, test_insiliscope.py, psf_parity_check/
third_party/mmCoreAndDevices/  git submodule: Micro-Manager's MMDevice SDK + build scripts
```

## Build and test

```sh
cmake --preset msvc && cmake --build --preset msvc          # core, native (MSVC 2022 x64)
source ~/emsdk/emsdk_env.sh                                  # Emscripten (pinned: see .github/)
cmake --preset wasm && cmake --build --preset wasm           # core, WASM
node tests/parity/run.mjs                                    # parity vs the JS prototype
```

## License

BSD, see [LICENSE](LICENSE).
