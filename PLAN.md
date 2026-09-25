# insiliscope -- plan and status

Last updated: 2026-09-25 (M4 CI block done; webSMLM side open).
Tick items in the commit that finishes them; add a short note
with the evidence (test, report, commit) rather than just a checkmark.

## Goal

One C++ world model of a field of cells (cell body, nucleus, cytoplasm, microtubules, dye sites;
later NPCs, DNA, other cell types, excitation profile), consumed by:

1. **inSiliScope** Micro-Manager device adapter (native C++, `adapter/`).
2. **Browser viewer** (`web/`): JS UI + WebGL, generator = the same C++ compiled to WASM.
3. **webSMLM** (upstream HohlbeinLab, single-file, no build): receives a CI-generated block (WASM as
   base64 + thin JS wrapper). CI only builds and publishes the block (artifact / release asset); it
   never opens PRs. Bringing it into the fork `kjamartens/webSMLM` is a separate, hand-made change.

No second hand-maintained implementation.

## Decisions (settled -- do not re-litigate)

- One repo (this one): demoCam_SMLM_MM grown, cell_field_sim imported, both with history.
- **Engine option B:** single C++ core -> native (adapter, cli) and WASM (viewer, webSMLM).
- RNG/hashing (pcg4d, integer) bit-exact everywhere. Floating point: no fast-math, no FP contraction,
  own transcendentals in core. M0 went further than planned: core uses V8's own fdlibm, making
  **geometry bit-exact too** (JS = native = WASM), because packing turned out to be chaotic.
- Every cell/MT/dye is a pure function of `(seed, address)`; never a running stream.
- `core/` has no MMDevice/GPU/OS dependencies; the adapter stays MSBuild.
- Public core interface stays narrow: `sitesInWindow`, `densityInWindow` (+ the event query), exposed via
  a C ABI (flat buffers, no exceptions/STL across it).
- webSMLM integration: CI builds and publishes the block, nothing more (no PRs, no pushes to other
  repos; changed 2026-09-25). It lands in the fork `kjamartens/webSMLM` only, by a separate change
  made there; never a direct push to upstream.
- TIFF output: JS writer in the viewer + native `cli/`.
- Viewer: `draw()` never generates on the main thread; generation in Web Workers, each worker
  instantiates the WASM module.
- **Out of scope for now** (removed from spec/PORT.md on 2026-09-25; the per-frame query stays, as PORT.md 6.2): an illumination-dependent
  bleaching clock (per-block exposure time) and automatic activation-rate calibration to a target
  ON-density. Dye schedules run on plain simulated time; the mean activation time is a parameter.

## Milestones

### M0 -- Feasibility spike (go/no-go on B) -- DONE, GO

- [x] RNG (`pcg4d`, `hashUnit`, `hashStream`) ported to `core/src/rng.h`: bit-exact vs JS incl.
      negative/wrapping addresses (252 addresses + 4 streams).
- [x] Packing ported (`rawCandidate`, `envelopNucleus`, `buildCandidateMap`, `relax`, `prune`,
      `packMap`, `interactionChunks`): **50/50 seeds bit-identical** JS vs native (MSVC) vs WASM.
- [x] Native (MSVC 2022) and WASM (Emscripten 6.0.10) builds via CMake presets.
- [x] Size: core WASM 29.6 KB raw / 12.8 KB gzip / 17 KB base64-of-gzip (budget < ~400 KB).
- [x] Report with per-seed table: [spec/m0-feasibility.md](spec/m0-feasibility.md).

### M1 -- Repo restructure -- DONE

- [x] demoCam_SMLM_MM history merged, `DeviceAdapter/` -> `adapter/` (git filter-repo; from
      branch `websmlm-parity-and-property-groups` @ 9d66b32).
- [x] cell_field_sim history merged -> `web/` (+ `web/tools/check_cellfield_microtubules.mjs`),
      from websmlm branch `cell-field-simulation` @ f871727.
- [x] Adapter builds unchanged at its new path (MSBuild Release|x64).
- [x] Adapter output unchanged: `tools/adapter_pixel_hash.py` gives identical SHA-256 for 4 configs
      (defaults/GibsonLanniZernike, Gaussian, NUP, EMCCD) before and after the move; deterministic
      run to run.
- [x] `tools/test_insiliscope.py` passes against the moved build (all checks; `ADAPTER_DIR` added so the
      DLL need not be copied into the MM install; one stale default expectation fixed -- it failed on
      the pre-move DLL too).
- [x] `web/tools/check_cellfield_microtubules.mjs` passes from its new location.
- [x] Golden vectors frozen in `spec/golden/` (JS reference: 50 packing windows, RNG, 2000 math
      samples); ctest `golden_vectors` passes natively and under Node+WASM.
- [x] CI workflow `.github/workflows/ci.yml`: core native (MSVC), core WASM (pinned emsdk 6.0.10),
      golden freshness vs `web/index.html` (now `web/prototype/`), adapter MSBuild.
      **Manual-only for now** (`workflow_dispatch`; not triggered by push/PR) and not yet run on GitHub;
      jobs verified locally step by step.
- [x] `CLAUDE.md` rewritten (repo rules on top, adapter knowledge kept below).
- [x] Port spec moved to `spec/PORT.md` and updated for the new layout; prototype README ->
      `spec/ALGORITHM.md`. (`PORT.md` is deleted once the port is complete, per its header.)
- [ ] Load in Micro-Manager Studio by a human (headless checks passed; nothing visual changed).

### M2 -- Full core port -- DONE

- [x] Cytoplasm height field: `cytoHeightAt`, `buildCytoMesh`, `smoothCytoGrid`,
      `sampleCytoMeshHeight` (MTs clamp against the *smoothed mesh*, so the mesh is needed).
      Uses `Math.pow`: small native/WASM-vs-JS differences here are **accepted** (decided
      2026-09-25); the prototype is not changed for it. Golden check for the mesh uses a tolerance.
      `core/src/cytomesh.*`. Bit-identical native (glibc) vs a Node 24 freeze; WASM near (1e-13 um).
- [x] Microtubules: `buildMicrotubulesForCell` and everything it calls (collisions, turn radius,
      cytoplasm clamp), per-MT arc length + parallel-transport frames. `core/src/microtubules.*`;
      fdlibm `asin`/`cbrt` and V8's 3-argument `Math.hypot` added to `jsm`. Golden `cells` cases:
      2306 microtubules + lattice windows bit-identical native, near WASM.
- [x] Dyes: lattice -> binder -> dye with addressed linker draws; only decorate segments reaching the
      window (a full network is millions of sites). `core/src/dyes.*` (H1/H2 per PORT.md 5.2, 1 um
      blocks partition the lattice exactly); stats checked in ctest `world_checks` (12.5/24.5 nm,
      linker r^3 CDF, 13_3 angles/stagger, 1625 sites/um, count ~ efficiency).
- [x] Fixed-block packing (PORT.md 4.3) so a moving stage reproduces cells; `packing=off` switch
      (`enablePacking = 0`). `core/src/world.*`; `world_checks`: stage 1 mm away and back, dropped
      caches, other query history, 4-tile union all identical. Border effect measured in PORT.md 4.3.
- [x] C ABI: `sitesInWindow`, `densityInWindow` (`excitationAt` dropped with M5):
      `isc_world_new/free`, `isc_cells_in_window`, `isc_sites_in_window`, `isc_density_in_window`
      (ABI version 1); exercised natively (`world_checks`) and through WASM (`wasm_abi_smoke.mjs`).
- [x] Golden vectors extended (mesh, MTs, dyes) and green, native + WASM. Dye *geometry* is in the
      golden `mtl` lines (lattice sites through `buildMicrotubuleLabelPoints`); dye *identity* is
      normative C++ hashing with no JS counterpart, so it is covered by `world_checks` instead.
      WASM core: 75 KB raw / 35 KB gzip.
- Not done here, noted for M3 (both done in M3's core step): dye blocks were not cached between
  queries; the viewer needed a mesh/microtubule polyline query.

### M3 -- Consumers

- [x] Core side of the consumers (ABI 2): per-dye blink schedules (PORT.md 6.1), dye blocks cached with
      their schedules (LRU by dye count), `isc_events_in_window` (6.2 steps 1-3 + cull), full cell
      record + `isc_cell_outline/mesh/microtubules` for the viewer. `world_checks`: kinetics stats,
      events = brute force, frame slicing, determinism; steady state ~3 ms/frame native, ~6 ms WASM at
      a 12.8 um FOV (~210k dyes in z 0.5-3 um).

- [x] Viewer on WASM: workers instantiate the module; pan benchmark (300 synthetic frames; previous
      p50 17 ms / p95 26 ms with workers in software GL) re-measured. `web/index.html` runs the core
      (`web/insiliscope_module.js`, single-file build `insiliscope_web`, embedded by
      `tools/embed_web_module.mjs`, `--check` in CI) in workers (cells / per-cell assets / dyes) or on
      the main thread (`?nw`); the prototype moved to `web/prototype/` (parity + MT checks updated).
      `web/tools/bench_pan.mjs` (300 frames x 0.8 um, 1280x800, headless Chromium + SwiftShader,
      4-core container): WASM draw p50 1.5 / p95 11.4 / max 309 ms, 0 frames with cells loading,
      initial load 7.5 s; prototype on the same box p50 2.1 / p95 15.9 / max 284 ms, 5.3 s. Same cells
      and microtubule counts as the prototype at the origin; workers, `?nw` and `?2d` agree after
      seed/param changes. Dyes (real core sites) replace the JS label preview; the lattice debug
      popup is gone (the prototype keeps it). Not yet looked at by a human in a real browser.
- [ ] Adapter: `CellFieldSource::EventsForFrame` + `XYStage` (PORT.md 2, 6-8) feeding the
      existing `RenderPhotonImage/CollectGpuEmitters/ApplyNoiseChain` unchanged; property naming
      convention; stage test in `test_insiliscope.py`; XY stage moves over the field.
      Code done (PORT.md 11 steps 3-4, deviations listed there) and verified on a test-only Linux
      build with pymmcore-plus (`tools/build_adapter_linux.sh`, `tools/test_cellfield_stage.py`: move
      time 0.52 s for 500 um at 1000 um/s, Busy while moving, +2 um stage = -20 px image shift,
      TransposeMirrorX flips it, 1000-frame precomputed stack 1.7 s and byte-identical after a 1 mm
      excursion). MSBuild on Windows passes in CI (job `adapter`, which also publishes
      `bin/windows-x64/`). **Open: the full `test_insiliscope.py` against the real DLL (needs
      Windows + JRE) and a look in Micro-Manager Studio.**
- [x] TIFF: JS writer in the viewer; `cli/` (window/seed -> sites/frames -> TIFF) reusing the
      adapter's `Simulation/` render code without MM. `Simulation/ScopeMovie.*` = the camera's
      precomputed CellField pipeline for one FOV (Gaussian PSF; same seed streams), used by
      `cli/insiliscope_cli` (native, 16-bit multi-page TIFF, ctest `cli_tiff`) and compiled into the
      viewer's module (`insiliscope_scope_web`, export `isc_scope_movie`): the viewer's "blink movie"
      panel renders N frames at the view centre in a worker, plays them and saves a TIFF (JS
      writer). A 10-frame 128x128 movie from the viewer is byte-identical to `insiliscope_cli` with
      the same spec. Vectorial PSF models only in the adapter (JVM).

- [x] Rename (user request, 2026-09-25): SMLMDemoCam -> **inSiliCellScope** (now inSiliScope, see below) everywhere: `adapter/inSiliScope/`,
      `inSiliScope.sln/.vcxproj`, `mmgr_dal_inSiliScope.dll`, devices `Camera`/`XYStage`/`ZStage`,
      classes `CInSiliScopeCamera`/`InSiliScopeXYStage`/`InSiliScopeZStage`,
      `tools/test_insiliscope.py`. Old MM hardware configs must be re-made.

- [x] Rename (user request, 2026-09-25): repo `insilicell` -> **insiliscope** (renamed on GitHub by the
      owner) and every internal name: adapter `inSiliCellScope` -> `inSiliScope` (dir, `.sln/.vcxproj/.rc`,
      `mmgr_dal_inSiliScope.dll`, classes `CInSiliScopeCamera`/`InSiliScopeXYStage`/`InSiliScopeZStage`),
      `core/include/insiliscope/insiliscope.h`, CMake targets/outputs `insiliscope_*`, `insiliscope_cli`,
      `web/insiliscope_module.js` + `createInsiliscope`, `tools/test_insiliscope.py`, block markers.
      Kept: the `isc_`/`ISC_` C ABI prefix, `isc::`, MM property and device names. Old MM hardware
      configs must be re-made; `bin/windows-x64/` removed until CI (`publish_dll`) rebuilds the DLL.
      Output unchanged: native + WASM ctest pass; `insiliscope_cli` frames pixel-identical (only the
      TIFF description string changed); block module sha256 unchanged (9d76094f4867) with the same
      block-test results; `tests/parity/run.mjs` PASS; viewer loads and renders in headless Chromium;
      Linux test build + `tools/test_cellfield_stage.py` all pass under module `inSiliScope`.

- [x] (user request) `General_EmitterDensityPerSec` = blink onsets / um^2 / s at any exposure and
      ON time (was scaled by exposure / ON time); live-mode Poisson truncation and warm-up fixed;
      ctest `emitter_density`. `CellField` is the default pattern (default FOV at stage 0,0 shows a
      cell edge). Seeded precomputed output is unchanged when exposure = ON time (the defaults).

- [x] (user request) CellField kinetics: `SimType_CellFieldActivationRatePerDyePerSec` replaces the
      activation mean; `SimType_CellFieldNonBleachingLabelingPct` adds persistent (DNA-PAINT-like)
      sites whose supply never drops (PORT.md 6.3, core ABI 3). Defaults unchanged in output.

- [x] (user request) CellField speed + defaults. Slow frames with more labelling at a lower rate:
      the 2M-dye cache evicted the query's own blocks (784 -> 1.5 ms/frame at 2.8M dyes; blocks in use
      are never evicted now) and persistent blinks were re-derived per dye per frame (now cached per
      block for a range of time bins: 548 -> 2.7 ms); events byte-identical, world_checks
      `CacheUnderLoad`. Renames: `SimType_CellFieldLabelingPctBleaching` / `...LabelingPctNonBleaching`
      (were `...LabelingPct` / `...NonBleachingLabelingPct`), `SimType_CellFieldMilliActivationRatePerDyePerSec`
      (1e-3/s, max 1000; was `...ActivationRatePerDyePerSec` in 1/s). Defaults 0% / 70% / 1.43 = the old
      1e-3 activations per lattice site per s, now constant (22824 vs 21311 blinks in 10 s: the old
      bleaching dyes deplete); 2.4 ms/frame at the adapter's window. `test_cellfield_stage.py` checks
      the defaults and passes on the Linux test build. Old MM configs setting these properties must
      be re-made.

- [x] (user request) CellField moves without stalls. Every 20th frame (each 1 s persistent-blink
      bin at 50 ms frames) all blocks rebuilt their bin range at once (40-85 ms vs ~2 ms); ranges are
      now extended ahead at a per-block point of their last bin. A focus move regenerated the whole
      window (~2 s: eviction ran mid-query and dropped blocks the query still needed); eviction now
      waits for the end of the query. Core ABI 4: `isc_world_set_dye_cache` (adapter: 16 M dyes) and
      `isc_world_prefetch`; live mode pre-loads the FOV's z column + 3 um in x/y in the wait before
      each frame (smooth xy + focus motion: worst query 191-336 -> <= 40 ms, RSS < 0.8 GB). Events
      byte-identical; world_checks `CacheUnderLoad` extended.

### M4 -- webSMLM integration via CI

- [x] Release workflow: build + test, emit the block and publish it as an artifact / release asset.
      **CI does not open PRs** (nor push to other repos). `.github/workflows/release.yml` ("webSMLM
      block", manual; `release_tag` input -> GitHub release asset + `.sha256`); `ci.yml` core-wasm builds
      and checks it too. The block is `dist/cellfield_block.js` (JS, not `.html`: it replaces the IIFE
      inside webSMLM's existing `<script>`), between `// ==== BEGIN/END insiliscope CellField block ====`
      markers; header: source repo + commit, Emscripten version, core ABI, sha256 of the module,
      licence, rebuild command. Not committed (`dist/` is gitignored). Pieces: target `insiliscope_block`
      (core C ABI, WASM inlined, `WASM_ASYNC_COMPILATION=0` + `MODULARIZE=0` so it instantiates
      synchronously -- webSMLM builds structures synchronously on the main thread),
      `tools/make_cellfield_block.mjs` (+ `--check <file>`: header checksum = module = this build),
      `tests/block/check_cellfield_block.mjs`. Same entry point as the IIFE:
      `CellField.buildWindow(w, h, {seed, xUm, yUm, pxnm, mtDensity, cellDensity, focusUm, slabNm})` ->
      `{sites: [[x px, y px, z nm]], nCells, nMt, removed: null, packed}`; webSMLM's `CF_PARAMS` are
      passed explicitly, every lattice site carries a dye (labelEfficiency 1, webSMLM thins sites
      itself); plus `workerSource()` (text that evaluates to a CellField in a Worker), `dispose()`.
      141.6 KB. Verified: block test (bounds, determinism after a 1.2 mm excursion / seed change /
      dispose, two half windows = full window, parameters reach the world, worker copy identical);
      headless Chromium main thread (sync compile accepted) and a Blob Worker give the same sites.
      Against webSMLM's pre-migration IIFE (branch `cell-field-simulation` @ b957244, 25.6 um window,
      defaults): seed 1249 at the origin 2047593 vs 2047594 sites, z mean/SD/histogram equal to 0.1 nm;
      elsewhere the cells differ as expected from fixed-block packing (PORT.md 4.3): e.g. seed 7 at
      (100, 100) um 3.19M vs 3.02M sites, same z profile. Timing: first call at a new place ~3-6 s
      (packing a block + ~2M sites), repeat ~2 s; the JS was 1.8-4.5 s.
- [ ] webSMLM side (separate repo, separate change, per its CLAUDE.md): `tools/sync_cellfield.mjs`
      (+ `--check`), replace the `CellField` IIFE with the wrapper (main thread + worker), keep
      `simulation_mt_*` PARAMS behaviour, MODULE INDEX, build letter, CHANGELOG/docs, remove
      `cell_field_sim/` and the two-way-sync rule. **Done in https://github.com/kjamartens/webSMLM/pull/1**
      (into `cell-field-simulation`, build 2026-09-25a; block from 8e6632f); tick when merged. No
      CHANGELOG row (dev build; rows are per release).
- [ ] Verified statistically/visually against the pre-migration build (density, lattice geometry,
      focus-height clipping) + webSMLM syntax check. Headless part done (in that PR): syntax OK; old vs
      new webSMLM, 141x141 px at 100 nm, defaults, 4 seeds/places: site counts within 0.01%
      (1522403/1522393 at seed 1249, origin), z mean/SD equal (one SD 0.1 nm apart); Simulate movie runs.
      **Open: a look in a real browser.**

## Open points / risks

- Upstream acceptance of a base64 WASM block (mitigation: provenance header, checksum, `--check`,
  rebuild instructions).
- Viewer worker plumbing changes (workers run generator *source text* today; they will instantiate
  WASM).
- CI is disabled on push/PR (manual trigger only) and has never run on GitHub; until it is
  re-enabled, run `ctest` locally (native + WASM) before committing core changes.
