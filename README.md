# inSiliScope

[![CI](https://github.com/kjamartens/inSiliScope/actions/workflows/ci.yml/badge.svg)](https://github.com/kjamartens/inSiliScope/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/kjamartens/inSiliScope?include_prereleases)](https://github.com/kjamartens/inSiliScope/releases)
[![License: BSD-3-Clause](https://img.shields.io/badge/license-BSD--3--Clause-blue.svg)](LICENSE)

**A deterministic in-silico microscope.** inSiliScope generates an endless field of synthetic cells (cell body,
nucleus, cytoplasm, microtubules, dye sites) and images it as a single-molecule localisation (SMLM) movie or as a
widefield image, with a physical PSF and camera noise model. Every cell is a pure function of `(seed, address)`, so
any window of the field can be generated on its own, in any order, on any platform, with identical results.

- **Live viewer** (browser, WebAssembly): pan through the field, render SMLM/widefield movies, save TIFFs.
- **Micro-Manager adapter**: `Camera`, `XYStage`, `ZStage` devices; live or precomputed acquisitions against a sample that never changes.
- **webSMLM block**: the same core as one generated JS file.

Project site, docs, gallery and benchmarks: **https://kjamartens.github.io/inSiliScope/**

## Get it

| I want to... | Do this |
|---|---|
| try it | open the [viewer](https://kjamartens.github.io/inSiliScope/viewer/) |
| use it in Micro-Manager | download `mmgr_dal_inSiliScope.dll` from the [latest release](https://github.com/kjamartens/inSiliScope/releases), copy it to your Micro-Manager folder (Windows x64), add the module `inSiliScope` |
| script it | build `insiliscope_cli` (below) and write TIFF movies headlessly |

## What it models

- **World**: jittered-grid cell placement with relaxation packing, wobbly cell outlines, a 3D nucleus, a cytoplasm height
  field, 3D microtubules anchored at the nucleus, a 13_3 protofilament dye lattice with antibody/nanobody linkers.
- **SuperRes**: blinking dyes (bleaching and persistent/DNA-PAINT-like populations), diffraction PSFs (scalar
  Gibson-Lanni with Zernike aberrations by default, double helix; Richards-Wolf and Gibson-Lanni via PSFGenerator), sub-pixel placement, sCMOS/EMCCD noise, background, drift.
- **WideField**: all labelled dyes at once, 3D PSF convolution by FFT, photobleaching in physical units (extinction, QY,
  photon budget), world-anchored bleach memory, hardware z stacks, GPU path (WebGPU / Direct3D 11).

Documentation of the models: [Physics](https://kjamartens.github.io/inSiliScope/physics/world-model/). The algorithm notes (the *why* of each design decision) are in [spec/ALGORITHM.md](spec/ALGORITHM.md).

## Build

```sh
# core + CLI, native (Linux/macOS)
cmake -S . -B build/native -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/native
ctest --test-dir build/native
build/native/cli/insiliscope_cli --out movie.tif --frames 200 --size 128   # --help for all options

# Windows (MSVC)
cmake --preset msvc && cmake --build --preset msvc && ctest --test-dir build/msvc -C Release

# WASM (Emscripten 6.0.10) and the viewer module
cmake --preset wasm && cmake --build --preset wasm && node tools/embed_web_module.mjs
```

The Micro-Manager adapter is MSBuild-only and needs the `third_party/mmCoreAndDevices` submodule and a locally built
`third_party/SMLMPsfEmbedded.jar`; see [CLAUDE.md](CLAUDE.md) and [docs/dev/BUILD_AND_USAGE.md](docs/dev/BUILD_AND_USAGE.md).
Tagged releases build everything in CI ([.github/workflows/release.yml](.github/workflows/release.yml)).

## Repository layout

```text
core/                      world model, C++17, no MMDevice/GPU/OS deps; C ABI in core/include/insiliscope/
adapter/inSiliScope/       the Micro-Manager device adapter (MSBuild) and Simulation/, the render engine
cli/                       insiliscope_cli (headless TIFF movies) and the viewer's WASM target
web/                       the viewer (WASM core); web/prototype/ is the JS reference implementation
spec/                      algorithm notes, port spec, frozen golden vectors
tests/ tools/              parity and golden harness, GPU checks, adapter smoke tests, release tooling
docs/  gallery/            the project site (mkdocs) and the manifest rendered into the gallery
```

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) and [docs/extending.md](docs/extending.md). Ideas: [docs/roadmap.md](docs/roadmap.md).

## Citing

See [CITATION.cff](CITATION.cff).

## License

This project's source is BSD-3-Clause ([LICENSE](LICENSE)). The Micro-Manager DLL embeds EPFL's PSFGenerator
(GPL-3.0), so the distributed DLL is GPL-3.0 as a whole ([LICENSE-GPL-3.0.txt](LICENSE-GPL-3.0.txt)). The viewer and the
webSMLM block contain no GPL code and are BSD-3-Clause. Third-party components and credits:
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). inSiliScope builds on
[webSMLM](https://github.com/kjamartens/webSMLM) (MIT) and its camera and PSF models.
