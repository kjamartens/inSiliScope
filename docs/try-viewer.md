# Viewer

The viewer runs the inSiliScope core as WebAssembly in your browser. Pan and zoom through the cell field, tilt for a
3D impression, then render a short SMLM or widefield movie of the dyes under the view centre and save it as a 16-bit
TIFF (identical to the `insiliscope_cli` output with the same settings).

**[Open the viewer](../viewer/index.html)** (it is deployed with this site and rebuilt from the repository on every change).

Notes:

- Widefield movies use WebGPU when your browser exposes a hardware adapter; the movie's info line says `GPU` or `CPU`.
  Everything falls back to the CPU on any GPU failure.
- Cells are packed on fixed blocks exactly as in the Micro-Manager adapter, so the view shows what the adapter images
  at the same position.
- URL flags for testing: `?nw` (core on the main thread), `?2d` (force Canvas 2D), `?wfgpu=any` (allow software WebGPU).
- The original JS prototype the core was ported from is at [`viewer/prototype/`](../viewer/prototype/index.html); it is
  the reference implementation that the golden vectors are frozen from.
