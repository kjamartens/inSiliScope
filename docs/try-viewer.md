---
hide:
  - navigation
  - toc
---

# Viewer

The inSiliScope core as WebAssembly in your browser. Pan and zoom through the cell field, tilt for a 3D impression,
then render a short SMLM, widefield or brightfield movie of the dyes under the view centre and save it as a 16-bit TIFF (identical
to the `insiliscope_cli` output with the same settings). **[Open it full screen](../viewer/index.html)** for more room.

<div class="isc-viewer">
  <div class="isc-viewer-loading">Loading the viewer (about 1 MB of WebAssembly)…</div>
  <iframe src="../viewer/index.html" title="inSiliScope cell field viewer" loading="eager"
          onload="this.previousElementSibling.style.display='none'" allow="fullscreen"></iframe>
</div>

Notes:

- The options are grouped the way a microscope is set up: display, sample (cell field, cell shape, nucleus,
  cytoplasm, packing, microtubules), sample preparation (labelling and fluorophore, refractive index), microscope
  (illumination, objective & PSF, camera, image formation) and acquisition. Groups fold open and closed; every option
  has a unit and an (i) that explains it (hover or Tab to it).
- The ☰ menu switches between **Default** options (the common ones) and **Advanced** (all of them), and between three
  looks: Compact, Focus (one group open at a time, highlighted; the default) and Light. The choices are remembered in your browser.
  Sliders also follow the mouse wheel, one step per notch.
- **Microtubule label** picks the dye and its mode; a pick loads the dye's fields, the mode's labelling and its light
  preset (lasers, dichroic, filter under **Light path**, with the spectra plotted). **Dye slots** hold edited copies of
  library dyes. Presets set several advanced options at once: **Cell shape** (footprint and outline), **Cell look** (cytoplasm height profile; the default is "Rounded") and **BF quality**. The options a preset drives are
  indented under it; editing one shows "Custom".
- Movies use the same PSF as the Micro-Manager adapter: scalar Gibson-Lanni + Zernike aberrations by default (the
  "PSF model" and "Aberrations" selects under Objective & PSF; Gaussian is the fast option). The kernel of the current
  PSF settings is computed in the background as soon as the page is ready and whenever those settings change, so a
  movie usually finds it ready.
  The viewer uses a 3 um kernel half width (the adapter: 7 um) to save browser memory; `--psf-kernel-half-width-nm 3000`
  reproduces a viewer movie with the CLI.
- Widefield movies use WebGPU when your browser exposes a hardware adapter; the movie's info line says `GPU` or `CPU`.
  Everything falls back to the CPU on any GPU failure.
- Brightfield movies run on the CPU, spread over the browser's workers (the condenser source points are shared out;
  the movie's info line says how many workers took part); the **BF quality** slider trades speed for precision (1: thin
  object, 6 condenser points ... 4: 0.25 um slices, 48 points). At 256 px the default level 3 takes well under a second
  once the cells are built (the first movie at a new place also builds the cells); repeating a movie is instant, and
  SMLM and widefield movies also reuse the cells of the previous movie.
- The **x-z** checkbox (View, on by default) adds a side view along the bottom of the map: the cytoplasm, nuclei,
  microtubules and dyes of the view's y range seen along y, at equal x and z scale.
- Cells are packed on fixed blocks exactly as in the Micro-Manager adapter, so the view shows what the adapter images
  at the same position. The packed cell positions are remembered in your browser (a few hundred KB of local storage,
  one entry per seed and cell settings), so a reload shows the same field without packing it again.
- URL flags for testing: `?nw` (core on the main thread), `?2d` (force Canvas 2D), `?wfgpu=any` (allow software WebGPU).
- The original JS prototype the core was ported from is at [`viewer/prototype/`](../viewer/prototype/index.html); it is
  the reference implementation that the golden vectors are frozen from.
