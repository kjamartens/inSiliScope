---
hide:
  - navigation
  - toc
---

# Viewer

The inSiliScope core as WebAssembly in your browser. Pan and zoom through the cell field, tilt and turn it for a 3D view,
then render a short SMLM, widefield or brightfield movie of the dyes under the view centre and save it as a 16-bit TIFF (identical
to the `insiliscope_cli` output with the same settings), or make an animation of a cell and export it as MP4, WebM or GIF. **[Open it full screen](../viewer/index.html)** for more room.

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
- Presets set several advanced options at once: **Fluorophore** (labelling and blinking: dSTORM-, PALM-, DNA-PAINT-like
  starting points), **Cell shape** (footprint and outline), **Cell look** (cytoplasm height profile; the default is "Rounded") and **BF quality**. The options a preset drives are
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
- **Tilt** (0 = straight down, 90 = from the side) and **Rotation** (about the vertical through the view centre) turn
  the view; Shift-drag on the map does both (sideways turns, up and down tilts). **Z clip** draws only what lies
  between two heights above the coverslip; drag both knobs together for a slab moving through the cells (its full
  0-20 um range is off). The advanced **Turn height** sets the height of the point the view turns about.
- **Detailed cells** (default 5): only the cells nearest the view centre get their microtubules, nucleus and dyes;
  the others show their cytoplasm, so a zoomed-out, turned view stays fast. 0 draws every cell in full. Advanced:
  **Scope** draws all cells, the centre cell with faint neighbours, or the centre cell alone; **Look** switches between
  the depth colours and bright colours on black.
- The **x-z** checkbox (View, on by default) adds a side view along the bottom of the map: the cytoplasm, nuclei,
  microtubules and dyes of the view's depth range seen from the front (along y when the view is not turned), at equal
  horizontal and z scale; the Z clip shows as a band.
- Tilted views are drawn in depth order: each cell's cytoplasm surface, nucleus and contour lines back to front, the
  cells far to near, and the microtubules and dyes depth-tested on the GPU, so a nucleus shows through the cytoplasm
  above it and a nearer cell covers a farther one.
- The **Animation** tab (next to Settings) builds short animations of one cell for talks: the view orbits the cell
  while its structures appear. An animation is a list of **cycles**, each bound to a structure (microtubules, nucleus,
  cytoplasm) and made of **steps**. A step lasts some seconds, moves the camera (turning by the cycle's orbit, tilting,
  zooming) and can **sweep a plane** through the cell (up, down, side to side, or along the screen's axes) with a slab
  of some thickness; every layer of the step is shown **ahead** of the plane, **in the slab** and/or **behind** it (its
  wake), with its own colour and opacity. A step without a sweep makes its layers pop in (fading). Typical: the
  simulated microtubules building up bottom to top while the view turns, then a slab moving back down. Duplicate a cycle
  and switch its structure to repeat it for the nucleus; "keep shown after" leaves a layer on for later cycles. Ready-made
  cycles (27): orbits, fly-arounds, turntable, zoom dive, top-view reveal, peel away, x-ray, optical sections, depth scan,
  wipes, outside-in, dye sites, WideField / SMLM / BrightField slices, ground truth vs thresholded or localized, SMLM
  build-up; ready-made sequences under "New from…" (grand tour, ground truth vs data, an SMLM experiment, every
  modality, turntable). The timeline under the view plays and
  scrubs it (Space, arrows, [ and ]; once, loop or ping-pong); the dimmed border shows the export frame. **Export**
  renders every frame off screen (not a screen recording) as MP4 (H.264; WebM where the browser cannot encode it), WebM
  or GIF, at 720p to 4K, with an optional scale bar, per-step captions, the plane's position and a legend. Animations
  are kept in the browser, with undo/redo, and saved or opened as `.json` files. Layers can also be simulated image
  data (below): a **WideField slice**, **SMLM camera frames** or a **BrightField slice** riding on the sweeping plane,
  the **thresholded WideField** surface or the **SMLM localizations** in its wake (e.g. "Simulated up, WideField down,
  thresholded wake", or the ready-made "Simulated, WideField, thresholded" sequence); the export makes their data first
  (a sweep along the screen's depth shows an x-z slice). The data an animation needs is made by itself as soon as it is
  previewed or edited.
- **Data layers** (Display): simulated images of the cell nearest the view centre, drawn in 3D at the **slice height**:
  the WideField image with its focus there (the out-of-focus blur of the rest included), the SMLM camera frames at the
  nearest focus position (blinking), the BrightField image. Checking one makes its z-stacks by itself (for the centre cell, once per
  cell and settings; **Acquire** makes them now) with the movie settings
  (Microscope, Acquisition): one movie per focus position over the cell's box, WideField and BrightField planes the
  mean of a few frames (advanced: step, averaging, SMLM frames and step). They are kept in the browser, so a cell is
  acquired once per settings. "Crop to the cell" shows only the cell's own footprint. SMLM planes start with fresh dyes
  by default (advanced: sequential, where bleaching dyes run out in later planes). **WideField thresholded** is the
  WideField z-stack smoothed and thresholded (Otsu's level x the Threshold slider) as a surface. **SMLM localizations**
  emulate a multi-plane SMLM acquisition: at focus positions every SMLM step, 5000 frames each (advanced), every blink
  within the capture range (±400 nm) is localized per frame, displaced by its precision (from its photons and defocus);
  drawn coloured by height, or as Gaussian spots of their precision.
- The movie player also saves the movie as a GIF or MP4 (as shown: the display range, scaled up to at least 512 px).
- Cells are packed on fixed blocks exactly as in the Micro-Manager adapter, so the view shows what the adapter images
  at the same position. The packed cell positions are remembered in your browser (a few hundred KB of local storage,
  one entry per seed and cell settings), so a reload shows the same field without packing it again.
- URL flags for testing: `?nw` (core on the main thread), `?2d` (force Canvas 2D), `?wfgpu=any` (allow software WebGPU).
- The original JS prototype the core was ported from is at [`viewer/prototype/`](../viewer/prototype/index.html); it is
  the reference implementation that the golden vectors are frozen from.
