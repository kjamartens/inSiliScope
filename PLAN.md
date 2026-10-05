Upcoming ideas:

- cleanup of code of mm of non-cell-field i think
- more targets
- deliniation of dstorm, palm, dna-paint, spt.
- addition of regular fluorescence -- WideField modality done (2026-09-27: core ABI 5 density3d, CPU FFT
  engine, cli/viewer, MM stack + live with a world-anchored bleach map; see CLAUDE.md). Speed-ups done
  (2026-09-27, spec/PORT.md 13): [x] mixed-radix real FFT, [x] kernel + per-dye-plane spectra caches
  (focus = re-pairing), [x] per-focus images (no FFT per frame), [x] focus bands (gated, inactive for
  sharp-pupil PSFs), [x] world-anchored grid + dye tiles, [x] sub-cell phase ramp, [x] bleach basis
  (groups / Chebyshev), [x] destination prefetch, [x] pipelined live loop, [x] sequenceable ZStage + z
  series, [x] the GPU WideField path (one WGSL source: WebGPU in the viewer, D3D11 in MM via naga HLSL,
  fp16 resident spectra, stack frames + noise on the GPU, self-check + CPU fallback). Next: visual check
  of the D3D11 path on a real Windows GPU; the Gaussian PSF's defocus law (`WidefieldGaussianSigmaUm`,
  TODO(human)).
- SuperRes speed-ups (2026-09-28), every output bit-identical (cli TIFFs, adapter_pixel_hash + 3 CellField
  configs, world_checks `Threads`, ctest `sr_render` against the previous splat/FFT code): [x] core
  query on threads (packing blocks, cell assets, dye blocks, schedules, persistent covers) + hoisted
  invariants, [x] short first persistent cover, [x] PSF kernel memo (a property change no longer
  recomputes it), [x] stack query concurrent with the PSF computation, [x] live CPU render + noise in
  row bands on all cores, [x] bounds-check-free splat, [x] Fft placement: tabulated twiddles, line
  transforms on all cores in live mode, [x] cli frames in parallel batches.
- Performance pass (2026-10-03/04), every output bit-identical (21 cli reference TIFFs, adapter_pixel_hash,
  scope_parity SR 100 %, golden vectors, memcmp checks against the previous splat and chirp-Z; CLAUDE.md): [x] polyphase
  + AVX2 splat, [x] shared kernel memo, [x] MovieCache for SR/WF, [x] WF frame batches, [x] core hoisting, [x] core
  worker pool + parallel relax/microtubules, [x] query path (lazy microtubules, block midpoints), [x] ABI 7 block
  injection, [x] viewer: one WASM compile, no re-packing, typed-array assets, LUT playback, [x] batched pruned chirp-Z,
  [x] parallel BF setup, [x] JS prototype and imaging-reference refactors, [x] build flags (Release default, LTO,
  core SIMD). Next: [ ] `wf_gpu.js` lazy copies and smaller upfront buffers, [ ] WF warm-up query if the cold tile
  fill shows in `ISC_TIMING`, [ ] a generated `sincos` (`tools/gen_jsmath.py`), [ ] candidate reuse across packing
  blocks, [x] per-block packing on any idle viewer worker (2026-10-04). Persistent caches (2026-10-04): [x] core ABI 8 packed-block
  store (cli, MM `General_DiskCache`, viewer local storage), [x] opt-in PSF kernel file, [x] PSF preload at MM
  `Initialize()` and at viewer load / PSF change, [x] cli `--prepare`.
- more targets
- brightfield (exploration, 2026-10-01, spec/BRIGHTFIELD.md): [x] package survey (waveorder, chromatix, TorchOptics,
  PyWolf, holopy, CytoPacq/SimuCell: none embeddable, chromatix-style multislice ported), [x] core ABI 6 optical volume,
  [x] multislice + Abbe engine with a speed/precision quality (1-4; a hidden reference level 5), [x] camera PRNU default 5% -> 0.5%, [x] cli/viewer (slider)/adapter (stack, live, z
  sequences), [x] tests (grating theory, empty field, determinism, adapter, viewer), [x] list of missing structures
  (only simulated ones make contrast). Next: [ ] MSBuild + visual check in MM, [ ] waveorder comparison, [ ] GPU path,
  [ ] stage-move prefetch, [ ] phase contrast / DIC, [ ] the missing structures (nucleoli, lipid droplets, vesicles...).
  Speed (2026-10-02): [x] shared transmittances/propagator/defocus, band-pruned cache-blocked FFTs, WASM SIMD,
  [x] quality levels re-tuned on measured accuracy (grid at lambda/4n), [x] cli/viewer keep world + scene across movies:
  256 px level 3 0.9 s in the viewer (was ~12 s), same accuracy.
- Issue 11, viewer z-slicing and layered animations (2026-10-05, viewer only): [x] phase 1: one camera with rotation
  (`web/scene/core.js`), tilt to 90°, Z clip per fragment, detail budget (5 central cells), scope and look, structure /
  layer registry, dyes on the GPU; [x] phase 2: Animation tab (sequences of cycles and steps: camera moves, plane
  sweeps with ahead / in-slab / behind layers), preview timeline, MP4 / WebM / GIF export (also for the movie player);
  [ ] phase 3: WF / SR / BF slices from z-stacks (main view and animations); [ ] phase 4: thresholded WF isosurface,
  SMLM localizations (multi-plane acquisition, precision noise).
- 2026-10-05: [x] shaped nuclei (issue 12: lobes, kidney bend, thickness, wider base, widest point, basal gap; dome
  follows the nucleus), [x] microtubule starts/ends sampled by distance with a direction pick, smooth over/under
  envelope (generic obstacle interface), [x] ported to the core bit-exactly (ABI 9 nucleus rings, optical volume),
  [x] viewer x-z side view; [x] BrightField defaults lamp 80k, condenser NA 0.4, indices 1.35.
- 2026-10-03: viewer UI (`web/index.html`, also `web/lab.html`): [x] panel built from one option schema (every option
  one line, a unit and an (i) tip), [x] groups by sample / sample preparation / microscope / acquisition, collapsible,
  [x] Default/Advanced and three styles (Compact, Focus: one group open, Light), mouse-wheel sliders in a ☰ menu, [x] min/max as two-knob sliders, [x] presets
  (Fluorophore, Cell shape, Cell look, BF quality); [x] world default cytoplasm = "Rounded" (rim 0.2-0.5, mid 2-3.5,
  slope caps 2/4; prototype + core + block, golden re-frozen) with their options indented under them; `engine_check.mjs` reads the defaults
  from the schema. Next: [ ] tune the new presets' values and the Default/Advanced split after use.
- 2026-10-01: [x] C++ Gibson-Lanni+Zernike PSF (`Simulation/ZernikePsf.*`, = webSMLM's scalar chirp-Z model) in the
  adapter (no JVM for the default model), cli and viewer (their default PSF now); [x] photonCV 0.5 and WideField
  excitation 4e8 (t1/2 120 s) defaults; [x] the 2x2 overview (map, structures, widefield, SMLM) on the gallery and
  Home (`tools/build_overview.py`, `insiliscope_cli --geometry-json`); [x] site: homepage link, viewer embedded on
  its docs page with a back link and a loading text.
- 2026-10-01: cell edges and height (spec/ALGORITHM.md): [x] fractal outline tail (harmonics 6-64, amplitude
  ∝ blob x k^-(2.5-D), `cellRough` 0.15, `cellFractalDim` 1.35, bit-exact complex recurrence), [x] cytoplasm height
  relaxed on a Cartesian grid (screened Poisson, exact boundary, nucleus obstacle; `cytoRelaxUm` replaces
  `cytoSmoothPasses`): no folds. Next: [ ] thin protrusions (filopodia, retraction fibres), [ ] tension arcs between
  adhesions (concave scallops).
- 2026-10-01: [x] fast iteration loop: `web/lab/` (prototype A/B vs main with hot reload, imaging preview, MT shape
  metrics, `check.mjs` ~8 s), `PORT_PENDING.md` + CI `port-gate` (C++ port only in the PR to main),
  `tools/port_check.sh` (all builds/tests in one go); prototype loader shared with the golden reference
  (`tests/parity/load_prototype.mjs`, plain `Function` instead of a `vm` context: re-freeze 68 s -> 10 s, same output).
  [x] JS imaging reference `web/prototype/scope/` (world/dyes/kinetics, Gibson-Lanni+Zernike PSF, splat, camera,
  WideField) = the C++ (`tests/parity/scope_parity.mjs`: events equal, SR and WideField movies 100% identical ADU vs
  the committed WASM; CI job `scope-parity`), shown in the lab next to main's C++ movie.
- 2026-10-02: [x] JS imaging reference caught up with main's C++: BrightField (`web/prototype/scope/brightfield.js` =
  `BrightfieldRender.cpp`; >= 99.5% identical ADU, intensity within 1e-4 relative: complex float32 FFTs), the
  optical-volume query (`World.opticalVolume`, bit-identical), the fractal cells' reach, the PRNU 0.5% default and the
  `bf-*` options; the lab shows BrightField.
