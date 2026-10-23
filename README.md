# inSiliScope

[![CI](https://github.com/kjamartens/inSiliScope/actions/workflows/ci.yml/badge.svg)](https://github.com/kjamartens/inSiliScope/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/kjamartens/inSiliScope?include_prereleases)](https://github.com/kjamartens/inSiliScope/releases)
[![License: BSD-3-Clause](https://img.shields.io/badge/license-BSD--3--Clause-blue.svg)](LICENSE)

**A deterministic in-silico microscope.** inSiliScope generates an endless field of synthetic cells (cell body,
nucleus, cytoplasm, microtubules, nuclear pores, dye sites) -- or a field of DNA-origami nanorulers -- and images it as a
single-molecule localisation (SMLM) movie or as a widefield image, with a physical PSF and camera noise model. Every
cell and molecule is a pure function of `(seed, address)`, so any window of the field can be generated on its own, in
any order, on any platform, with identical results.

<p align="center"><a href="https://kjamartens.github.io/inSiliScope/"><img src="https://kjamartens.github.io/inSiliScope/docs/gallery/overview_3d.gif" width="720"
  alt="The cell field in 3D in the viewer: the camera box goes in from a cell's edge to its nucleus, the focus rises through it and comes down, and the box goes back; its live widefield and SMLM (DNA-PAINT) frames beside it, single molecules blinking"></a></p>

| A field of synthetic cells | What is in the box | Widefield camera | Single-molecule (SMLM) camera |
|:---:|:---:|:---:|:---:|
| <img src="https://kjamartens.github.io/inSiliScope/docs/gallery/overview_map.png" width="200" alt="A field of synthetic cells; the box is the camera's view"> | <img src="https://kjamartens.github.io/inSiliScope/docs/gallery/overview_structures.png" width="200" alt="Cell, nucleus, microtubules and nuclear pores in the box"> | <img src="https://kjamartens.github.io/inSiliScope/docs/gallery/overview_wf.webp" width="200" alt="Widefield movie of the box"> | <img src="https://kjamartens.github.io/inSiliScope/docs/gallery/overview_sr.webp" width="200" alt="SMLM movie of the box"> |

- **Live viewer** (browser, WebAssembly): pan, tilt and turn through the field in 3D, look through a live microscope
  (the adapter's live mode with the sample's illumination history, set up with its Micro-Manager config presets) and
  record TIFF stacks; make animations of a cell (orbits, layers building up, slices sweeping through) and export them
  as MP4, WebM or GIF.
- **Micro-Manager adapter**: a hub with one device per part of the microscope (camera, stages, objective, filters, lasers, lamp, sample, labels, renderer) at three levels of detail, and ready-made configurations; live acquisitions against a sample that never changes.
- **webSMLM block**: the same core as one generated JS file.

The viewer (live in your browser): **https://kjamartens.github.io/inSiliScope/** -- docs, gallery and benchmarks: **https://kjamartens.github.io/inSiliScope/docs/**

## Get it

| I want to... | Do this |
|---|---|
| try it | open the [viewer](https://kjamartens.github.io/inSiliScope/) ([a quick tour](https://kjamartens.github.io/inSiliScope/docs/quickstart/)) |
| use it in Micro-Manager | install a [Micro-Manager 2.0 nightly build](https://micro-manager.org/Micro-Manager_Nightly_Builds) (21 February 2026 or later: device interface 75), download `mmgr_dal_inSiliScope.dll` and `inSiliScope_Basic.cfg` from the [latest release](https://github.com/kjamartens/inSiliScope/releases), copy them to your Micro-Manager folder (Windows x64) and load the configuration ([getting started](https://kjamartens.github.io/inSiliScope/docs/micro-manager/)) |
| script it | build `insiliscope_cli` (below) and write TIFF movies headlessly |

## What it models

- **Samples**: the cell field or DNA-origami nanorulers (200 x 13 nm rods with three spots of docking strands 80 nm
  apart, bent in the coverslip plane as a worm-like chain), switched in the viewer, Micro-Manager's `SampleHolder` or the
  CLI's `--specimen` ([Samples](https://kjamartens.github.io/inSiliScope/docs/samples/)); optional fiducial beads on
  either (gold, TetraSpeck, FluoSpheres, nanodiamonds; spectra and brightness after Balinovic et al. 2019; on the
  coverslip, on the sample or in a gel; slow bleaching), for drift correction
  ([Fiducial beads](https://kjamartens.github.io/inSiliScope/docs/samples/fiducials/)).
- **Cell field**: jittered-grid cell placement with relaxation packing, wobbly cell outlines, a shaped 3D nucleus, a cytoplasm height
  field, 3D microtubules from the nucleus to the cell edge, a 13_3 protofilament dye lattice, nuclear pore complexes
  (Nup96, 64 sites) over the whole nuclear envelope; each target labelled on its own (two colours at once by default)
  through a probe model (antibodies, nanobody, SNAP/Halo, ...: binder, flexibility, linker).
- **Labels, dyes, light path**: a dye library (FPbase spectra, literature kinetics) in four label modes -- dSTORM
  (initial ON, blinking, intensity-scaled times), PALM (pre-converted state, 405 nm and primed activation), DNA-PAINT
  (imager binding, free-imager background) and WideField (all dyes at once, bleaching by photon budget); lasers,
  dichroics, emission filters and camera QE curves set excitation, detected fraction and each state's PSF wavelength
  ([Dyes and light path](https://kjamartens.github.io/inSiliScope/docs/physics/dyes-and-light-path/)).
- **Fluorescence imaging**: blinks splatted with a diffraction PSF (a scalar Gibson-Lanni model with Zernike
  aberration modes, double helix) or a Gaussian, continuous populations mean-field (3D PSF
  convolution by FFT, GPU path: WebGPU / Direct3D 11) or per dye, sub-pixel placement, sCMOS/EMCCD noise, background,
  drift, hardware z stacks.
- **BrightField** (exploration): transmitted light through the cells' refractive index (cytoplasm, nucleus,
  microtubules only -- nothing the world does not simulate), partially coherent Koehler illumination, multislice
  wave optics, one quality setting from fast to precise ([spec/BRIGHTFIELD.md](spec/BRIGHTFIELD.md)).

Documentation of the models: [Samples](https://kjamartens.github.io/inSiliScope/docs/samples/) and
[Physics](https://kjamartens.github.io/inSiliScope/docs/physics/dyes-and-light-path/). The algorithm notes (the *why* of each design decision) are in [spec/ALGORITHM.md](spec/ALGORITHM.md).

## Build

```sh
# core + CLI, native (Linux/macOS)
cmake -S . -B build/native -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/native
ctest --test-dir build/native -j 8
build/native/cli/insiliscope_cli --out movie.tif --frames 200 --size 128   # --help for all options

# Windows (MSVC)
cmake --preset msvc && cmake --build --preset msvc && ctest --test-dir build/msvc -C Release -j 8

# WASM (Emscripten 6.0.10) and the viewer module
cmake --preset wasm && cmake --build --preset wasm && node tools/embed_web_module.mjs
```

The Micro-Manager adapter is MSBuild-only and needs the `third_party/mmCoreAndDevices` submodule and a locally built
`third_party/SMLMPsfEmbedded.jar`; see [BUILDING.md](BUILDING.md).
Tagged releases build everything in CI ([.github/workflows/release.yml](.github/workflows/release.yml)).

## Repository layout

```text
core/                      world model, C++17, no MMDevice/GPU/OS deps; C ABI in core/include/insiliscope/
adapter/inSiliScope/       the Micro-Manager device adapter (MSBuild) and Simulation/, the render engine
cli/                       insiliscope_cli (headless TIFF movies) and the viewer's WASM target
web/                       the viewer (WASM core); web/prototype/ is the JS reference implementation
web/lab/                   iteration page: try prototype changes in JS (A/B vs main), port to C++ at merge
spec/                      algorithm notes, port spec, frozen golden vectors
tests/ tools/              parity and golden harness, GPU checks, adapter smoke tests, release tooling
docs/  gallery/            the project site (mkdocs) and the manifest rendered into the gallery
```

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) and [docs/extending.md](docs/extending.md). Ideas: [docs/roadmap.md](docs/roadmap.md).

## Citing

See [CITATION.cff](CITATION.cff).

## License

This project's source is BSD-3-Clause ([LICENSE](LICENSE)). The distributed Micro-Manager DLL is GPL-3.0
([LICENSE-GPL-3.0.txt](LICENSE-GPL-3.0.txt)). The viewer and the webSMLM block are BSD-3-Clause. Third-party components and credits:
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). inSiliScope builds on
[webSMLM](https://github.com/HohlbeinLab/webSMLM) (MIT) and its camera and PSF models.
