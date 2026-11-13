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
- Live performance pass (2026-10-07, [docs/dev/PERF_PASS.md](PERF_PASS.md), single-molecule first): [x] phase 0 frame
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
- DNA-origami nanorulers, a second specimen (2026-10-12, branch `claude/nanorulers`, spec/PORT.md 20): [x] JS model in
  iteration mode (2D WLC bending, Lp 1500 nm, 5-strand rings, Matern-II field), [x] core ABI 12, [x] engine/cli, [x]
  viewer + block, [x] MM (`SampleHolder`, `Nanorulers` device), [x] docs (Samples section), figures, gallery.
  [x] Site spacing (`origamiSpacingNm`, 640-5 nm, the body follows) and a first 2D origami (`TUDelftLogo`: rigid hex-map
  plate, face up; 2026-10-13, branch `claude/origami-spacing-2d`).
  Next: [ ] 3D designs (cuboid, 3D bending, ring heights validated), [ ] face-down 2D origami (mirrored), [ ] DNA-PAINT
  kinetics per imager sequence.
- Nuclear pore complexes, a second cell-field target (issue 15, 2026-10-12, branch `claude/npc-target`, spec/PORT.md
  21): [x] JS model in iteration mode (Nup96 rings, area-uniform over the envelope, min spacing), [x] label linkage
  (binder / flexibility / linker presets, defaults MT IgG2-PAINT, pores SNAP-Halo), [x] core ABI 13, [x] engine/cli
  (per-target options, None, combined auto light), [x] viewer + block, [x] MM (per-target `_Mode`, `Fluorophores.Mode`
  removed, `Lasers.Preset` Auto), [x] GPU splat with up to 4 blink groups, [x] docs, figures, gallery.
  Next: [ ] the viewer's 8-fold ring layer and per-structure dye colours (visualisation branch), [ ] `--dyes-json`
  with each dye's binding site and binder end (the viewer's linkage layer), [ ] pore variability (CIR4MICS).
- Nuclear lamina, a third cell-field target (2026-10-27, branch `lamin_network`, spec/PORT.md 25, spec/ALGORITHM.md
  "Nuclear lamina"): [x] random points on the envelope (50 /um^2, none within 20 nm of a pore centre), unlabelled by
  default (registry `defaultDye: "None"`), [x] JS + core ABI 16 (lamina structure 2, docking strands 3,
  `isc_cell_lamin`), [x] engine/cli `lamin-*`, viewer, block, MM `NuclearLamina_*`, [x] docs, gallery (`lamin-paint`).
  Next: [ ] a non-random lamin network (meshwork / filaments) in place of `buildLaminForCell`, [ ] a drawn lamina layer.
- Save and load settings (2026-10-28, branch `lamin_network`, [docs/dev/SAVE_SETTINGS.md](SAVE_SETTINGS.md)): `.isc.json`
  in mixable parts (instrument, settings, sample, measurement with the light history, view): [x] each option's part
  (`ScopeOptionPart`), names (`ScopeOptionNames`), `LiveScope::Advance`, [x] viewer Save / Load (☰ and two icons), quiet
  apply, the history replayed (`tests/web/viewer_settings_file.mjs`), [x] `insiliscope_cli --settings` (ctest
  `cli_settings`). Next: [x] ground-truth export (2026-11-04, below), [ ] MM `Hub.SaveSettingsTo` / `LoadSettingsFrom`, [ ] an
  instrument's installed hardware.
- Live WideField speed (2026-10-29, branch `lamin_network`): [x] the Direct3D 11 and WebGPU hosts keep the spectra of
  the 4 scene geometries used last (two scenes in turn re-made ~500 plane spectra per job: a z step 0.5 s, 0.8 fps),
  [x] dye counts from a dye index (native only: the viewer's WASM keeps its slab columns, 2 GB heap; 1 um bins sorted by
  height, exact; drift 70-90 ms per frame -> 7 ms), made ahead
  4 um around the FOV on a thread and world of its own in live, [x] a drifting scene on the CPU's FFT sizes (GPU on:
  28 -> 48 fps), [x] drift on the GPU paths measured and dropped (a basis rebuild costs the GPU as much as the CPU, the
  shifted frame is ~3 ms), [x] dSTORM z move: scenes too big for the GPU's plane pool beside the others render on the
  CPU (two initial-ON scenes evicted each other's planes: a fresh-site z step 1.4-1.6 s -> 0.2-0.3 s; bench `zMove1um`
  8-10 s -> 2.5 s), [x] a steady view's bleach-basis rebuilds off the live thread (MM live: the next basis made ahead
  on a thread, the dropped spectra freed on another; takes 2-60 ms). A new pose's first full basis (a stage move, drift
  re-anchoring, a channel or objective switch: 0.3-0.7 s per scene) stays on the live thread: the next frame needs it
  and no idle time precedes it; [x] its plane spectra cheaper, bit for bit (a buffer per thread, empty rows skipped:
  990 planes 0.41-0.46 -> 0.30-0.35 s, 1380 planes about the same).
- Excitation geometry: epi -> HILO -> TIRF on one BFP-position slider (2026-10-13, branch `claude/illum-geometry`,
  spec/PORT.md 22, spec/ALGORITHM.md "Excitation geometry"): [x] JS reference (`excitation.js`: FlatTop default, the
  HILO sheet, TIRF's evanescent field, clipped beyond the pupil; the imager over a 100 um chamber; DNA-PAINT HILO,
  nanorulers TIRF), [x] kinetics at the local light (dSTORM clock, PALM activation, windows, mean field on the map's weights),
  [x] C++ port (engine, cli, viewer module; ctest `excitation`), [x] MM (`Lasers.BfpPosition`, the history in z bands,
  render-ahead, config group `Illumination` Epi / HILO / TIRF), [x] viewer slider (with the regime readout), docs,
  figures (`light-geometry`, `light-geometry-images`), gallery (`light-epi`, `light-tirf`, `light-flat`,
  `nr-paint-epi`), the cli's excitation probe (`--setup-json` `excitation`), [x] the MM mean field bleaches by the
  exact dose (no edge at the history levels' half contour; 2026-10-15), [x] blinks, per-dye windows and every mean
  field by the exact dose on every consumer (the clock map, core ABI 14; branch `claude/exact-dose`, 2026-10-15/16),
  [x] the history levels' code removed, docs and figures (P4, 2026-10-16), [x] the viewer's live microscope on the map
  (P5, 2026-10-16, with the merge of main).
- Live microscope in the viewer (2026-10-13, branch `claude/live-viewer`): [x] layout A2 (world view + View bar left,
  draggable live panel right: live image, Live/Snap/Rec, focus, configuration, settings with search), [x] the adapter's
  live mode as an MM-free `sim::LiveScope` (light history, drift walker, running images, live noise counter;
  ctest `live_scope`) and in the viewer module (`isc_live_*`), [x] the Configuration from the Micro-Manager configs
  (`tools/gen_mm_configs.py` writes `web/live/mm_config.js` from the adapter's engine spec per preset;
  `tests/web/mm_config_check.mjs`), [x] docs. Next: [ ] the adapter's live producer on `LiveScope` (it keeps the GPU,
  render-ahead, BrightField prefetch, illumination profile and background fade: output must stay identical), [ ] live in
  lab.html (the JS reference has no illumination history yet), [ ] a camera-size/ROI control beside the live image.
  2026-10-14: [x] settings levels Basic/Advanced/Expert from the MM tiers (`ISC_MM_TIERS`), [x] the render path per label
  in the info, [x] a live histogram with draggable display range; [x] ● Record: N frames unpaced into one ImageJ TIFF
  stack (a fresh-sample movie by default; continuing the live sample with `LiveScope::Frames`, `isc_live_frames`, as an
  Advanced option), [ ] the Acquisition group (hidden), [ ] streaming to webSMLM (tried
  2026-10-14 and dropped: needs a server or a webSMLM change, to be decided), [x] per-frame performance breakdown (fixed
  rows), [x] the focal plane in the x-z view (with drift), [x] the pixel size derived from camera and objective,
  [x] nanoruler dyes inside the camera drawn at any zoom, [x] dim blinks skipped (`blink-min-photons`, Quality).
- Fiducial markers on any sample (2026-10-22, branch `claude/fiducials`): [x] data (`data/fiducials/`: Balinovic et al.
  2019's spectra read from figures 2a-6a with fitted tails, FPbase bead spectra, brightness from the paper's images,
  gold Mie table; `tools/fiducial_spectra/`), [x] JS model in iteration mode (`web/prototype/scope/fiducials.js`:
  Poisson field on any specimen, heights Coverslip / OnTop / Layer, 12 % size spread, one emitter per bead with the
  sphere's PSF in a 1 um core + the point PSF's halo, bleaching by a half-time at each component's excitation peak
  through a Taylor basis of static images, gold's laser scatter through the filters' blocking (OD 8);
  `web/lab/fiducial_check.mjs`: beads 1-4 % of a movie's time), [x] core + engine
  port (ABI 15 `isc_fiducials_in_window`, `Simulation/Fiducials.*`, `fid-*` options; spec/PORT.md 24; golden,
  `world_checks.fiducials`, `scope_parity` 100 %; cli beads 1-1.6 % of a movie), [x] MM `Fiducials` device (live
  bleaching from the light history's dose per bead; `tools/test_fiducials.py`), [x] viewer (Sample group, beads in the world view, `tests/web/viewer_fiducials.mjs`), [x] docs (`docs/samples/fiducials.md`), figures (`fid-spectra`, `fid-heights`; cli `--setup-json` `fiducials`), gallery (`fid-drift`).
- Bead sample (2026-10-30, spec/PORT.md 26): [x] a third specimen of fiducial beads only (ABI 17, density 0.3 and
  aggregation 40 % at a mount, 561 nm at 0.1 kW/cm^2, no OnTop), [x] bead clumps (`fid-aggregation-pct`, one slider:
  the chance a bead sticks to the next), [x] bead size apart from the type (a pick sets its product size; 20 nm beads:
  dyes x d^3; Basic on the bead sample in the viewer), [x] the beads' PSF z range (14 um), [x] Height Basic, [x] MM,
  viewer, docs, figure `fid-clumps`, gallery.
- Fluorescent slide (2026-10-30, spec/PORT.md 27): [x] a fourth specimen, a uniform dye layer (ABI 18; thickness 5 um,
  a TetraSpeck-colour mix or a library dye at 10 uM, bleach half-time 900 s at 1 kW/cm^2; its light 0.02 kW/cm^2),
  [x] its light the free imager's collected column, bleaching per pixel column from the light history, [x] MM
  `FluorescentSlide`, viewer group, docs (`docs/samples/slide.md`), figure `slide-bleach`, gallery, ctest `slide`.
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
  webSMLM block's `driftTrajectory`; SR, WideField and BrightField, stacks and live; spec/PORT.md 17). Next: [x] drift on
  the WideField GPU paths (2026-10-29: measured, not faster; dropped), [ ] webSMLM's simulator on the block's `driftTrajectory` (fork branch).
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
- 2026-10-30 (branch `claude/sim-illumination`): [x] structured illumination in the engine: the light as coherent beams
  in the back focal plane (`Simulation/SimPattern.*`: position, spot size, tilt, amplitude, phase, polarization,
  phase step, line per beam; the types SIM2D, SIM3D, LatticeSIM2D, LatticeSIM3D, Custom), the frame program (phase
  and orientation per frame), the kinetics on the cycle mean (the light history's components), the emission on each
  frame's pattern (blinks, per-dye windows, one mean-field scene per state, the background, the beads), cli
  `--sim-beams` and `<out>.pattern.csv`, the setup JSON's beams and pattern; ctest `sim_pattern`, `history_check`,
  `live_scope`. [x] MM `Lasers.Pattern*` rows, the frame counter (`PatternFrame`, `PatternRestart`, the producer waits
  for each frame to be taken under a pattern) and `tools/test_sim.py` (26 s); [x] the viewer's rows, readout and
  **Preview illumination** (`web/live/illum_preview.js`, `isc_illum_preview`; `web/illumination.html` the docs'
  interactive figure; 2026-10-31: a height slider for the section seen from below, captions as page text, the open
  dialog follows the settings; `sim-anchor` Focus / Coverslip, the beams in phase at the focus by default), the live
  info's pattern state, figures `sim-patterns`, `sim-cycle`, `sim-spot`, gallery
  `sim-2d`, `sim-3d`, `sim-lattice-3d`, `tests/web/viewer_sim.mjs`. Next: [ ] the GPU mean field under a pattern, [ ] the exact per-frame dose,
  [ ] stroboscopic / every-n-th-frame programs on `FrameLight.scale`.
  2026-10-31: [x] the RAM of the SIM path and under it (bit for bit): a memory report per owner (`MemoryReport.h`,
  `isc_memory_json`, the Expert box), PSF splat sums lazy in one layout, SIM state scenes not deferred on the WebGPU
  path and their spectra streamed, the slide's fringes capped, stale pattern slots freed, an aborted movie worker
  replaced alone, the module's heap up to 4 GB; `tests/web/wasm_heap_check.mjs`, `viewer_oom.mjs`. Open: [ ] the light
  history's per-component footprints over a long pan (measure with the report), [ ] sharing a population's dye
  planes / per-cell arrays across its states.
  2026-10-31: [x] live SIM with blinking labels: the frame counter out of the excitation key (bit for bit; DNA-PAINT
  SIM2D at 256 px 320 -> 105 ms), [x] dSTORM's initial ON and PALM's pre state under the cycle-mean light (Quality
  Exhaustive: each frame's pattern, `sim-frame-pattern-continuous`).
- 2026-11-03 (branch `general_fixes`): [x] the Microscope view: a Sample | Microscope switch in the title bar swaps
  the world view for panels the C++ describes (`cli/scope_panels.*`: typed views, controls, one float buffer;
  `isc_scope_panels` / `isc_scope_panel`), drawn by `web/live/scope_panels.js`: the light path drawn in the middle
  (clickable stations), the light in the sample, the PSF, the back focal plane, the spectra; a tile fills the view at
  twice the size. It replaced Preview illumination, Preview PSF and the spectra plot. ctest `scope_panels`,
  `tests/web/viewer_scope_view.mjs`. [x] Second round: the panels over the world view (the sample keeps loading), the
  light path on top lying down (names only in the tile, details filled), beams dimmed / stopped by the filters, the
  hover marks, equal tile images, the PSF filled as a 2 x 2 grid with the cut's angle. Next: [ ] more panels (camera noise, BrightField pupils) as C++ additions.
- Ground truth (2026-11-04, branch `gt-export`, docs/ground-truth.md): [x] a Record's settings file reproduces it
  (fresh / continued, `history.recordFrom`), [x] the engine's sink (`Simulation/GroundTruth.*`, `GroundTruthScope`:
  frames bit for bit with and without; photons on the camera through the splat's footprint, PSF-weighted background),
  [x] core ABI 19 `isc_dye_details_in_window` (binding sites, dipoles), [x] density volumes (photon-weighted float32
  ImageJ stacks), [x] cli `--gt-out` / `--gt-min` / `--gt-volume`, the module's `isc_scope_gt` (both on
  `settings::RenderMovie`), [x] the viewer's Save box and zip (`web/live/zip.js`), `web/voxelize.html`; ctest
  `ground_truth`, `tests/web/gt_unit.mjs`, `zip_unit.mjs`, `viewer_save.mjs`. Measured: collecting it during a Record
  adds ~70 % (cli DNA-PAINT 128 px 200 frames 2.8 -> 4.8 s), so it is computed at Save. Next: [ ] multi-dimensional
  acquisitions (z planes, tiles) in `frames.csv`, [ ] the same from MM.
- Live stage moves (2026-11-13, branch `live-move-speed`; viewer WASM, 128 px, a 1 um step): WideField moves cost
  110-450 ms (a still frame ~4 ms), and 3-7 s where the pores fall under the per-dye limit by the nucleus edge (2.1 M
  unused microtubule windows fetched every frame there); DNA-PAINT moves 60-150 ms. Plan: [x] core ABI 20
  `isc_continuous_in_map_masked` (the structures whose populations need windows; the ground truth's dye list all),
  core `Density3dInWindow` skips blocks outside its mask, [x] dye counts of a moved rect from the last count of its
  size (less the strips left, plus those entered: the same number), [x] WideField kernel spectra kept across a rect
  move (they depend on the FFT shape, R, levels and PSF only), [x] `LiveScope::Prefetch` / `isc_live_prefetch`: the
  viewer's live loop spends its spare time preparing 3 um around the FOV (the adapter's PrefetchScope), [x] checks:
  `world_checks` (masked = unmasked filtered, masked counts), ctest `live_scope` (a moving WideField sequence across
  the per-dye switch = the same with every cache cleared before each frame; prefetch between frames changes no frame),
  [x] docs (CLAUDE.md, CHANGELOG.md, spec/PORT.md 30), [ ] native + WASM ctest, the viewer module embedded, the viewer checked by hand.
  Tried and dropped: a live mean-field anchor (`AnchorMeanField` with a wider reach: no gain, frames 0.3-0.5 % rms off);
  fewer dyes (1e-3 of the labelling: moves ~40 ms, but no signal left). Next: [ ] the next frame's bleach basis
  (~100 ms WASM), [ ] new dye-tile rows and the per-dye switch's splat sums made in the spare time too.
