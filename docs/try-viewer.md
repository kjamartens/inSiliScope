---
hide:
  - navigation
  - toc
---

# Viewer

The inSiliScope core as WebAssembly in your browser. Pan and zoom through the cell field, tilt for a 3D impression,
then render a short SMLM or widefield movie of the dyes under the view centre and save it as a 16-bit TIFF (identical
to the `insiliscope_cli` output with the same settings). **[Open it full screen](../viewer/index.html)** for more room.

<div class="isc-viewer">
  <div class="isc-viewer-loading">Loading the viewer (about 1 MB of WebAssembly)…</div>
  <iframe src="../viewer/index.html" title="inSiliScope cell field viewer" loading="eager"
          onload="this.previousElementSibling.style.display='none'" allow="fullscreen"></iframe>
</div>

Notes:

- Movies use the same PSF as the Micro-Manager adapter: scalar Gibson-Lanni + Zernike aberrations by default (the
  "PSF / aberrations" selects; Gaussian is the fast option). The first movie with a new PSF computes its kernel first.
  The viewer uses a 3 um kernel half width (the adapter: 7 um) to save browser memory; `--psf-kernel-half-width-nm 3000`
  reproduces a viewer movie with the CLI.
- Widefield movies use WebGPU when your browser exposes a hardware adapter; the movie's info line says `GPU` or `CPU`.
  Everything falls back to the CPU on any GPU failure.
- Cells are packed on fixed blocks exactly as in the Micro-Manager adapter, so the view shows what the adapter images
  at the same position.
- URL flags for testing: `?nw` (core on the main thread), `?2d` (force Canvas 2D), `?wfgpu=any` (allow software WebGPU).
- The original JS prototype the core was ported from is at [`viewer/prototype/`](../viewer/prototype/index.html); it is
  the reference implementation that the golden vectors are frozen from.
