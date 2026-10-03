# Changelog

Versions follow semver; while 0.x, any release may change output for a given seed. Breaking changes for seeds are marked **seed**.

## Unreleased (0.1.0, first public release)

- Performance pass (2026-10-03), every output bit-identical (cli TIFF pixel data, `adapter_pixel_hash`, `scope_parity`
  SR 100 %): the diffraction-PSF splat reads a column-polyphase copy of the kernel block sums with vectorised row loops
  and an AVX2 copy chosen at run time (`Simulation/SplatKernel.h`; ~2x per blink single-threaded, the 12-thread cli is
  then limited by memory bandwidth at the 7000 nm kernel). Dev tooling: `ISC_TIMING=1` prints phase times of a cli/viewer
  movie, `sr_render_check --bench`, `isc_core_bench` and `tools/bench_core.mjs` (per-phase core timing, native and WASM),
  `tools/bench.py` gained 1000-frame SR, 200-frame WF and BrightField configs.
- Licensing clarified: own source BSD-3-Clause; the distributed DLL is GPL-3.0 as a whole (it embeds PSFGenerator).
- Renamed to inSiliScope everywhere.
- Release automation: tests, DLL, webSMLM block, gallery and benchmarks built on a `v*` tag and published as GitHub
  Release assets; project site (docs, viewer, gallery, benchmarks) on GitHub Pages. Built binaries are no longer committed.
- Documentation site with physics pages.
- Camera per-pixel gain spread (`CamParam_GainStdPctPerPixel`, cli `gain-std-pct`) default 5% -> 0.5% (typical sCMOS
  PRNU; 5% was a static pattern that swamped brightfield contrast). **seed**: every seeded frame with the default camera
  changes slightly.
- **BrightField** modality (exploration): transmitted light through the cells' refractive index (only simulated
  structures), partially coherent multislice wave optics, `General_BrightFieldQuality` 1-4; core ABI 6
  (`isc_optical_volume_in_window`). SuperRes/WideField output unchanged.
- Cytoplasm default is the "Rounded" look: rim height 0.2-0.5 um (was 0.1-0.3), mid height 2-3.5 um (was 1-2), slope
  caps `cytoMaxSlope` 2 / `cytoDomeSlope` 4 (were 1 / 3). **seed**: default cells are taller and rounder, so packing,
  microtubules, dyes and every CellField image change for every seed.
- Viewer UI: options built from one schema (one line each, units, (i) tips), grouped by sample / sample preparation /
  microscope / acquisition, collapsible; Default/Advanced and three styles (Compact, Focus, Light) in a menu; two-knob
  min/max sliders; mouse wheel on sliders; presets (Fluorophore, Cell shape, Cell look, BF quality).

### Earlier history (summary of the dated notes in `CLAUDE.md`)

- 2026-09-28: SuperRes speed-ups, output bit-identical (threaded core query, PSF kernel memo, parallel live rendering, faster splat and FFT placement).
- 2026-09-27: **WideField** modality (3D PSF convolution, bleaching in physical units, GPU path) and ABI 5 (`isc_density3d_in_window`).
- 2026-09-25: **Emitter density** fixed to mean blink rate independent of exposure (**seed**); Z convention changed so +Z moves focus up through the sample; non-bleaching (DNA-PAINT-like) sites, core ABI 3/4; renames to `inSiliScope`.
- 2026-09-21: webSMLM parity round 2 (chirp-Z only, 28 Zernikes, double helix, rich photophysics, counter-based noise RNG: every seeded frame changes once; **seed**).
