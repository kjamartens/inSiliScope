# inSiliScope

**A deterministic in-silico microscope.** inSiliScope generates an endless synthetic sample and images it the way a
microscope would: as a single-molecule localisation (SMLM) movie or as a widefield image, with a physical PSF and
camera noise model. Two [samples](samples/index.md) can be mounted: a field of cells (cell body, nucleus, cytoplasm,
microtubules, nuclear pores, fluorophore sites; the microtubules and the pores labelled in two colours) and DNA-origami
nanorulers (rods with three spots of docking strands 80 nm apart).

<div class="isc-launch" markdown>
[Open the viewer :material-open-in-new:](../){ .md-button .md-button--primary target="_blank" }

The live microscope in your browser; the [Quickstart](quickstart.md) shows what to try.
</div>

<!-- hero:start (the release's 3D overview, tools/build_hero.mjs; tools/build_site.sh keeps this or the 2x2 below) -->
<figure class="isc-hero"><img src="gallery/overview_3d.gif" alt="The cell field in 3D in the viewer: the camera box goes in from a cell's edge to its nucleus, the focus rises through it and comes down, and the box goes back; its live widefield and SMLM (DNA-PAINT) frames beside it, single molecules blinking" loading="lazy"></figure>

The cell field in the viewer, tilted and in perspective, and what its live microscope sees in the dashed camera box. More
in the [Gallery](gallery/index.md).
<!-- hero:end -->

<!-- overview:start (removed by tools/build_site.sh when no gallery release provides the images) -->
<div class="isc-overview">
  <figure><img src="gallery/overview_map.png" alt="A field of synthetic cells; the box is the camera's view" loading="lazy"><figcaption>A field of synthetic cells; the box is the camera's view</figcaption></figure>
  <figure><img src="gallery/overview_structures.png" alt="The structures in the box: cell, nucleus, microtubules and nuclear pores" loading="lazy"><figcaption>The structures in the box: cell, nucleus, microtubules and nuclear pores</figcaption></figure>
  <figure><img src="gallery/overview_wf.webp" alt="Widefield movie of the box (Gibson-Lanni + Zernike PSF)" loading="lazy"><figcaption>Widefield movie of the box (Gibson-Lanni + Zernike PSF)</figcaption></figure>
  <figure><img src="gallery/overview_sr.webp" alt="SMLM movie of the box" loading="lazy"><figcaption>SMLM movie of the box</figcaption></figure>
</div>

One spot of the field: where it is, what is there, and how a widefield and an SMLM camera see it. More in the
[Gallery](gallery/index.md).
<!-- overview:end -->

One C++ world model, three consumers:

| Consumer | What it is | Where |
|---|---|---|
| **Viewer** | Pan/zoom/tilt/turn through the field in your browser, a live microscope (bleaching where you look, the Micro-Manager config presets) that records TIFF stacks (WASM); cell animations exported as MP4/WebM/GIF | [Open the viewer](../){ target="_blank" }, [how to use it](quickstart.md) |
| **Micro-Manager adapter** | A hub (`mmgr_dal_inSiliScope.dll`) with one device per part of the microscope (camera, stages, objective, filters, lasers, lamp, sample): move the stage, change focus, run live acquisitions, sequences and z stacks against a sample that never changes | [Micro-Manager](micro-manager.md) |
| **webSMLM block** | The same core as a single generated JS file for [webSMLM](https://github.com/HohlbeinLab/webSMLM) | [Quickstart](quickstart.md) |

## Why it exists

Testing localisation and analysis software needs data where the truth is known, and testing acquisition software
needs a sample that behaves like a sample: the same cell is in the same place every time the stage returns to it,
bleaching persists, focus matters. inSiliScope provides both from one source of truth.

## Key properties

- **Address-based determinism.** Every cell, microtubule and dye is a pure function of `(seed, address)`. Any window
  of the infinite field can be generated alone, in any order, on any platform, with identical results. See
  [World model](samples/cell-field/world-model.md).
- **Bit-exact across targets.** The RNG and the geometry match the original JS prototype bit for bit (native, WASM, JS),
  guarded by golden vectors in CI.
- **Dyes and a light path.** Each structure has its own label: a dye of the library (FPbase spectra; AF647, mEos3.2, ATTO 655,
  mEGFP, ...) in a mode: *dSTORM*, *PALM* (with the pre-converted state), *DNA-PAINT* (with the free imager's background)
  or *WideField* (every dye at once, bleaching by its photon budget). Lasers, dichroic, emission filter and the camera's
  QE curve decide what is excited and detected, and at which wavelength (the PSF follows). See
  [Dyes and light path](physics/dyes-and-light-path.md).
- **Two modalities.** *Fluorescence* with a diffraction PSF (a scalar Gibson-Lanni model with Zernike aberration
  modes, double helix) or a Gaussian. *BrightField* (exploration): transmitted light through
  the cells' refractive index, partially coherent multislice wave optics, a speed/precision quality setting.
- **Realistic camera.** sCMOS and EMCCD noise chains with per-pixel maps, background and illumination profiles.
- **More than one sample.** The geometry of a sample is separate from the photophysics and the imaging: the cell
  field and the [nanorulers](samples/nanorulers/index.md) share the dyes, the light path, the PSF and the camera.
  Switch in the viewer's Sample select, Micro-Manager's `SampleHolder` or the cli's `--specimen`; see
  [Samples](samples/index.md). [Fiducial beads](samples/fiducials.md) can be added to either, for drift correction.
- **Built to extend.** Samples, targets, photophysics and modalities are separate layers; see
  [Extending](extending.md).

## Reading guide

- New here: the [Quickstart](quickstart.md) (a tour of the viewer), then the [Gallery](gallery/index.md).
- Want the models: [Samples](samples/index.md) (the cell field's world model and structures, the nanorulers) and
  [Physics](physics/dyes-and-light-path.md) (dyes and light path, photophysics, optics, camera, validation).
- Want to add something: [Extending](extending.md) and the [Roadmap](roadmap.md).
- Timings: [Benchmarks](benchmarks.md).

!!! note "Status"
    Pre-1.0. The numbers are reproducible for a given version and seed (`world_version` is recorded in outputs once
    introduced; see the roadmap). The Direct3D 11 widefield path is checked on software and Wine adapters; reports from
    real GPUs are welcome.

## License

The source is BSD-3-Clause. The distributed Micro-Manager DLL is GPL-3.0; the viewer and the webSMLM block are
BSD-3-Clause. See the repository's `LICENSE` and
`THIRD_PARTY_NOTICES.md`.
