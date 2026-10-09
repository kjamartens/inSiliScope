Upcoming ideas:

- [x] cleanup of code of mm of non-cell-field (2026-10-05, issue 16 phase 0: the legacy patterns are gone).
- Issue 16, structures / dyes / light path (branch `claude/multi-dye`; plan in the PR): [x] phase 0 legacy MM patterns
  removed (CellField output unchanged), [x] phase 1 dye/filter/camera data from FPbase + generators (data/dyes, data/references.json), [x] phase 2 JS
  model (labels per structure, modes dSTORM/PALM/DNA-PAINT/WideField, light path, camera presets, mean-field switch,
  imager background, orientation/off-target/SPT plumbing) under `PORT_PENDING.md`, phase 3 port: [x] core ABI 10
  (labels, windowed schedules, continuous windows; `tests/parity/label_parity.mjs` bit-exact with the JS), [x] engine
  (Spectra, LightPath, DyeLibrary + generated DyeLibraryData.inc, FluorescenceMovie: blinks per kernel group, mean-field and
  per-dye populations, imager background; cli movies = the JS, 100 % identical ADU in every mode tried),
  [x] cli/viewer module/block (ABI 10 in the viewer, sites stride 5, labels on sites jobs, the WebGPU mean-field path
  per scene, movie-progress from the WASM; block abiVersion 10; engine_check: lab.html = WASM), [x] adapter, [x] checks + benchmarks, [x] docs.
- MM adapter as a hub with devices (2026-10-06, branch `claude/mm-devices`, spec/MM_DEVICES.md): [x] hub + peripherals,
  tiered registry, Test tier, [x] label model (global mode, typical labels, specimen registry, Cy3B), [x] excitation
  filters, [x] light from the shutters (both summed, none dark), [x] EMCCD without sCMOS spreads, [x] emission
  magnification 0.667x (97.45 nm), [x] generated configs + property reference, `insiliscope_cli --spec`. Next: [ ] load
  the configs in Micro-Manager Studio (Channel group images, Mode switch updates the Label list, Basic <= 30 rows).
- Live performance pass (2026-10-07, [spec/PERF_PASS.md](spec/PERF_PASS.md), single-molecule first): [x] phase 0 frame
  pacing (precise waits, absolute schedule, condition-variable handoff; BrightField 32 -> 50 fps at 20 ms), [x] MMCore
  phase profile (`Test_ProfileWriteTo`, `bench_live.py --profile`, release job `mm-bench`, Benchmarks page), [ ] phase 1
  persistent live session (done so far: dye-count memo, all-core single-frame splat, persistent `ParallelFor` pool,
  mean-field clock weights memo, no unused noise maps per frame), [x] phase 2 render-ahead queue + frame-parallel rendering
  (`LiveAhead.cpp`; BrightField focus prefetch `BrightfieldLive`), [x] phase 3 GPU for SR frames (dSTORM/PALM, populations and lamp added on the GPU),
  [ ] phase 4 interactive latencies, [ ] phase 5 CPU per-frame costs.
- Physics figures (2026-10-08, branch `claude/physics-figures`): [x] cli diagnostic outputs (`cli/scope_probes.*`,
  output-neutral, ctest `cli_probes`), [x] `tools/build_physics_figures.py` (33 figures, quality/realism presets as
  side-by-side tables), [x] markers in `docs/physics/*.md`, injected by `build_site.sh`, [x] rebuilt by `pages.yml`.
  Next: [ ] the viewer's drift preset table from `RenderPresets.h` (`web/index.html` keeps its own copy).
- Blink render regimes (2026-10-08, spec/ALGORITHM.md "Blink render regimes", spec/PORT.md 19): [x] binned FFT
  (approximate SMLM) above 12.5 ON blinks per um^2 (C++ + JS, 100 % identical ADU), [x] blink mean-field (expected ON
  time, off by default), [x] adapter GPU splats only the SMLM frames, `Renderer.Blink*` rows, viewer controls, docs.
  Next: [ ] a D3D11 binned FFT if live profiling of dense DNA-PAINT asks for it.
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
  [x] phase 3: WF / SR / BF slices from z-stacks (main view and animations); [x] phase 4: thresholded WF isosurface,
  SMLM localizations (multi-plane acquisition, precision noise).
- 2026-10-06: [x] sample drift as a random walk, xy and z separately (`Simulation/Drift.*`, `drift.js`; MM, cli, viewer,
  webSMLM block's `driftTrajectory`; SR, WideField and BrightField, stacks and live; spec/PORT.md 17). Next: [ ] drift on
  the WideField GPU paths, [ ] webSMLM's simulator on the block's `driftTrajectory` (fork branch).
- 2026-10-07: [x] drift presets (`SampleHolder.DriftPreset` Off ... Extreme, Basic; the viewer's Drift select; the
  configs' Drift group in every tier), the z drift as a magnitude with a direction, bounded direction/z swings.
- 2026-10-08: [x] the sample's history in MM: rate history per tile (epochs, core ABI 11
  `isc_world_set_kinetics_history`, JS twin; a light/label change acts from now on: PALM 405 off keeps the converted
  dyes, power steps continue from the present state), light counted at publish, lit rect follows the drift, drift
  continues across Live stops/starts, `SampleHolder.TimeWhileIdle` (Running/Paused), fixed camera pattern maps, live
  per-dye running images carried across frames; `tools/test_history.py`.
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
