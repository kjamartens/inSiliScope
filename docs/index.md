# inSiliScope

**A deterministic in-silico microscope.** inSiliScope generates an endless field of synthetic cells (cell body,
nucleus, cytoplasm, microtubules, fluorophore sites) and images it the way a microscope would: as a
single-molecule localisation (SMLM) movie or as a widefield image, with a physical PSF and camera noise model.

One C++ world model, three consumers:

| Consumer | What it is | Where |
|---|---|---|
| **Viewer** | Pan/zoom/tilt through the field in your browser, render SMLM and widefield movies, save TIFFs (WASM, WebGPU for widefield) | [Open the viewer](try-viewer.md) |
| **Micro-Manager adapter** | `Camera`, `XYStage` and `ZStage` devices (`mmgr_dal_inSiliScope.dll`): move the stage, change focus, run live or precomputed acquisitions against a sample that never changes | [Quickstart](quickstart.md) |
| **webSMLM block** | The same core as a single generated JS file for [webSMLM](https://github.com/kjamartens/webSMLM) | [Quickstart](quickstart.md) |

## Why it exists

Testing localisation and analysis software needs data where the truth is known, and testing acquisition software
needs a sample that behaves like a sample: the same cell is in the same place every time the stage returns to it,
bleaching persists, focus matters. inSiliScope provides both from one source of truth.

## Key properties

- **Address-based determinism.** Every cell, microtubule and dye is a pure function of `(seed, address)`. Any window
  of the infinite field can be generated alone, in any order, on any platform, with identical results. See
  [World model](physics/world-model.md).
- **Bit-exact across targets.** The RNG and the geometry match the original JS prototype bit for bit (native, WASM, JS),
  guarded by golden vectors in CI.
- **Two modalities.** *SuperRes*: blinking dyes with a vectorial PSF (Richards-Wolf, Gibson-Lanni, Zernike aberrations,
  double helix). *WideField*: every labelled dye at once, 3D PSF convolution, photobleaching in physical units.
- **Realistic camera.** sCMOS and EMCCD noise chains with per-pixel maps, background, haze and illumination profiles.
- **Built to extend.** Targets, photophysics and modalities are separate layers; see [Extending](extending.md).

## Reading guide

- New here: [Gallery](gallery/index.md), then [Quickstart](quickstart.md).
- Want the models: [Physics](physics/world-model.md) (world model, structures, photophysics, optics, camera, validation).
- Want to add something: [Extending](extending.md) and the [Roadmap](roadmap.md).
- Timings: [Benchmarks](benchmarks.md).

!!! note "Status"
    Pre-1.0. The numbers are reproducible for a given version and seed (`world_version` is recorded in outputs once
    introduced; see the roadmap). The Direct3D 11 widefield path is checked on software and Wine adapters; reports from
    real GPUs are welcome.

## License

The source is BSD-3-Clause. The Micro-Manager DLL embeds EPFL's PSFGenerator (GPL-3.0), so the distributed DLL is
GPL-3.0 as a whole; the viewer and the webSMLM block are BSD-3-Clause. See the repository's `LICENSE` and
`THIRD_PARTY_NOTICES.md`.
