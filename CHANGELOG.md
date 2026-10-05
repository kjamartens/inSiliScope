# Changelog

Versions follow semver; while 0.x, any release may change output for a given seed. Breaking changes for seeds are marked **seed**.

## Unreleased (0.1.0, first public release)

- Viewer Animation tab (issue 11, phase 2, 2026-10-05): animations of one cell as cycles of steps (camera moves, plane
  sweeps with per-layer zones ahead / in the slab / behind, pop-ins with fades, captions), previewed on a timeline under
  the view and exported frame by frame off screen as MP4 (H.264), WebM (VP9/VP8) or GIF (self-written muxers and GIF
  encoder over WebCodecs, no libraries), with scale bar, captions, plane position and legend overlays; undo/redo, local
  storage, `.json` save/open. The movie player saves GIF and MP4 too. `web/anim/`, `web/encode/`; checks
  `tests/web/anim_unit.mjs`, `encode_unit.mjs` (CI job `viewer-js`), `viewer_anim_export.mjs` (browser).
- Viewer rotation, z clip and detail budget (issue 11, phase 1, 2026-10-05): the view turns about the vertical through
  its centre (Rotation, Shift-drag) and tilts up to 90° (was 75°); a Z clip draws only what lies between two heights
  (clipped per fragment on the GPU); only the 5 cells nearest the view centre (Detailed cells, 0 = all) get their
  microtubules, nucleus and dyes (every visible cell had them before); advanced Scope (all / centre cell + faint
  neighbours / centre cell) and Look (depth colours / dark fluorescence). Dyes are depth-tested on the GPU. One camera
  (`web/scene/core.js`, with the structure/layer registry the animation tab builds on); at rotation 0 the image is
  the previous one. Display only.
- Viewer depth order (issue 9, 2026-10-05): the cytoplasm surface, nucleus surface, outline and contour lines of a cell
  are painted back to front in one order and the cells far to near; microtubules are opaque ribbons in a z-buffer
  (WebGL2, simplified to a quarter pixel per zoom level). The nucleus no longer draws in front of the cytoplasm, and
  cells no longer overlap in the wrong order when tilted. The x-z side view draws each cell inside out (nucleus and
  microtubules under the translucent cytoplasm). Display only.
- **seed** Shaped nuclei (issue 12, 2026-10-05): lobes, a kidney bend, uneven thickness, a wider base and a lowered
  widest point per cell (`nucIrregMin/Max`, `nucBendMin/Max`, `nucSmooth`, `nucThickIrreg`, `nucAsym`,
  `nucWidestMin/Max`); the nucleus sits `nucBaseMin/Max` (0.4-0.9 um) above the coverslip and the dome top follows it
  (`cellHeightMin/Max` removed); nucleus height 0.2-0.3 x long axis, margin 0.5 um. Microtubules start in the cytoplasm
  near the nucleus and end near the edge, sampled by distance (`mtStartDecayPct`, `mtEndDecayPct`) with the end picked
  by direction (`mtDirKappa`; replace `mtStartFracMin/Max`, `mtStartOffsetXY`, `mtEndFracMin/Max`, `mtEndJitterDeg`),
  and ride over or under the nucleus on a smooth envelope. **Core ABI 9** `isc_cell_nucleus_rings`; the optical
  volume (BrightField) follows the shaped nucleus. MM properties `SimType_CellFieldNuc*` (11) and
  `SimType_CellFieldMicrotubule{StartDecayPct,EndDecayPct,DirKappa}`. Viewer: the nucleus drawn from the core's rings, an x-z side view
  (on by default), the Focus style by default.
- Adapter live mode: a frame taken after a property change no longer can be one still rendered with the old settings
  (frames carry the configuration they were rendered with; same idea as the z-sequence epoch), 2026-10-05.
- **seed** BrightField defaults (2026-10-05): lamp 80000 photons/px/s (was 40000), condenser NA 0.4 (was 0.55),
  cytoplasm and nucleus index 1.35 (were 1.345).
- Performance pass (2026-10-03/04), every output bit-identical (cli TIFF pixel data, `adapter_pixel_hash`,
  `scope_parity` SR 100 %, golden vectors): the diffraction-PSF splat reads a column-polyphase copy of the kernel block
  sums with vectorised row loops and an AVX2 copy chosen at run time (~2x per blink); the kernel memo shares one
  immutable copy of the planes; the cli/viewer keep world, scene and PSF across SR and WideField movies as they did for
  BrightField; WideField frames render in parallel batches; the chirp-Z PSF kernel is batched and pruned (0.65 -> 0.46 s);
  BrightField setup is parallel. Core: a worker pool instead of threads per call, parallel packing relaxation and
  microtubule generation, lazy microtubules for mesh queries, hoisted trig and tables (packing block 88 -> 19 ms,
  cold cells 207 -> 78 ms, cold dyes 225 -> 48 ms native; WASM packing 741 -> 408 ms); **core ABI 7**
  `isc_world_pack_block` / `isc_world_set_block` hand packed blocks between worlds. Viewer: one WASM compile shared by
  the workers, the pack worker's blocks injected into the others (no duplicate packing), cell assets as typed arrays
  (no per-vertex objects), LUT movie playback. JS references refactored bit-exactly (no BigInt, cached noise maps:
  the parity cases run 2x faster). Build: Release by default, link-time optimisation (`ISC_LTO`), WASM SIMD for the
  core. Measured (12 threads): SR 128 px 200 frames 3.3 -> 1.4 s, 256 px 10.3 -> 4.5 s, 1000 frames 10.7 -> 7.1 s,
  WideField 256 px 200 frames 3.3 -> 1.8 s, BrightField 256 px level 3 0.59 -> 0.47 s, level 4 2.0 -> 1.4 s. Dev
  tooling: `ISC_TIMING=1` prints a movie's phase times, `sr_render_check --bench`, `isc_core_bench` and
  `tools/bench_core.mjs` (per-phase core timing, native and WASM), `tools/bench.py` gained 1000-frame SR, 200-frame WF
  and BrightField configs.
- Viewer: the cells of a new seed (or new cell parameters) are packed one block per job on every worker instead of the
  whole window on one, nearest the view first, and drawn as the blocks arrive (2026-10-04).
- Viewer drawing (2026-10-04): microtubule paths are built once per tilt/rotation/detail level and reused across
  zooms, the painter's order of the cytoplasm quads is a comparator-free typed sort, the dyes are one path and one
  fill. Same picture, smoother panning and zooming.
- Viewer: a BrightField movie is split across the browser's workers (each computes a share of the condenser source
  points from the movie worker's phase screens; the images are summed in the same order, so the frames are identical to
  the single-worker ones), 2026-10-04.
- Persistent caches and PSF preload (2026-10-04), output unchanged. **Core ABI 8**: `isc_world_set_cache_dir` keeps a
  world's packed cell positions in a small per-user file (five numbers per cell; validated when read back), so a rerun
  with the same seed and cell parameters starts with the cells in place; `isc_world_version`. MM `General_DiskCache`
  (`Off` / `Cells`, the default / `CellsAndPsf`, which adds the ~200 MB PSF kernel file), cli `--disk-cache 0|1|2` and
  `--prepare 1` (build the world, pack the field of view, compute the kernel; no movie). The adapter computes the PSF
  kernel in the background from `Initialize()`; the viewer does the same in its movie worker when it loads and when a
  PSF setting changes, and remembers the packed cells in local storage across reloads.
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
