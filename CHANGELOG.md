# Changelog

Versions follow semver; while 0.x, any release may change output for a given seed. Breaking changes for seeds are marked **seed**.

## Unreleased (0.1.0, first public release)

- **seed** Micro-Manager adapter as a hub with devices (2026-10-06, `spec/MM_DEVICES.md`): the module offers the hub
  `inSiliScope` (pre-init `Detail` Basic / Advanced / Expert, `RandomSeed`) and one device per part of the microscope
  (`Camera`, `XYStage`, `ZStage`, `Objective`, `EmissionPath`, `FilterCube`, the Advanced `ExcitationFilter` /
  `Dichroic` / `EmissionFilter` wheels, `Lasers`, `TransmittedLamp`, `SampleHolder`, `CellField`, `Fluorophores`,
  `Renderer`). Every property has a tier and a name without group prefix (`Objective.NA`, was `PSFParam_PsfNa`); the
  full list: `docs/mm-properties.md`. **Configurations from before must be re-made**: the release ships
  `inSiliScope_{Basic,Advanced,Expert}.cfg` with Channel / Objective / Camera / Quality / Drift / Specimen groups and
  pixel sizes. Acquisition in MM is live only (snap, live, sequences, hardware z stacks); the precomputed stack stays
  for the tests (`ISC_TEST=1`). The light comes from the shutters (lasers: fluorescence, lamp: BrightField, both:
  summed on the camera, none: dark frames; cli/viewer `light-epi` / `light-trans`, `modality` unchanged). Labels:
  an experiment-wide `Fluorophores.Mode` and `CellField.Microtubules_Label` (`Typical` per mode, or a dye with data for
  it); new dye Cy3B. Excitation (laser clean-up) filters (cli/viewer `ex-filter`, default `None`: outputs unchanged).
  The pixel size is the sensor pixel / (objective x emission magnification), default 0.667x: 97.45 nm in MM (was 100;
  cli/viewer `pixel-nm` stays 100). The EMCCD ignores the sCMOS per-pixel gain and read-noise spreads (no preset sets
  them). `Renderer.WriteScopeSpecTo` + `insiliscope_cli --spec <file>` render an MM frame in the cli. Live z stacks no
  longer lose a position when a frame is taken late. The shipped configurations start in `ATTO655 DNA-PAINT`.

- Live speed in Micro-Manager (2026-10-07, `spec/PERF_PASS.md`), every output unchanged: frames on the exposure's clock
  (was capped at 64 / 32 fps by the Windows timer); per frame no dye recount at an unchanged pose, the single frame's
  splat and noise on all cores through a persistent thread pool, the mean-field clock weights computed once per distinct
  clock (256 px, 10 ms, loaded laptop: WideField 3 -> 50-80 fps, dSTORM 18 -> 25-40, PALM 6 -> 17-30). The Benchmarks
  page now shows Micro-Manager live frame rates and where each frame's time goes, measured on every release.
  Render-ahead: during a sequence acquisition at unchanged settings and pose, the next 2-8 fluorescence frames are
  rendered as one batch on a helper thread while the current ones are handed out (any change drops them); the GPU
  devices are created once per session instead of per live start (256 px, loaded laptop, 10 / 20 ms: dSTORM 36 / 24 ->
  67 / 29 fps, DNA-PAINT 27 / 12 -> 32 / 27, PALM 22 / 28 -> 28 / 37).
  Live BrightField computes the next focus positions in the background (a z sequence's, else along the last focus
  step) and keeps recent ones, so focusing at low magnification no longer stalls on every step (20x, 0.5 um steps 2 s
  apart: 70-180 ms to the new focus, was 500-800 ms); its images skip the pupil-blocked FFT columns (same pixels).
  dSTORM and PALM (and fluorescence + BrightField) render on the GPU too: the blinks splat there, their continuous
  populations and the lamp are added before the GPU noise (PALM live 28 -> 83 fps at 10 ms on the Iris Xe laptop).

- Sample drift as a random walk, xy and z set separately (2026-10-06; Cnossen et al. 2021, Ma et al. 2024): every frame
  adds a normal step of variance sigma^2 x frame time per axis, so sigma is the RMS displacement after 1 s. MM
  `SimType_DriftXyNmPerSqrtSec` / `SimType_DriftZNmPerSqrtSec`, cli/viewer `drift-xy-nm-per-sqrt-sec` /
  `drift-z-nm-per-sqrt-sec` (default 0: outputs unchanged), in SuperRes, WideField and BrightField, stacks and live; the
  cli writes the true drift to `<name>.drift.csv`; the webSMLM block gains `CellField.driftTrajectory`. **Removed**
  `SimType_DriftNmPerSec` (the linear drift; a saved configuration that sets it must drop it).
- Directed sample drift on top of the random walk (2026-10-06): a mean xy speed in a direction (random per seed
  unless set) and a signed z speed, whose direction and strength wander slowly (correlation time). MM
  `SimType_DriftXySpeedNmPerSec`, `SimType_DriftZSpeedNmPerSec` (everyday), `SimType_DriftXyAngleDeg`,
  `SimType_DriftXyAngleWanderDeg`, `SimType_DriftSpeedWanderPct`, `SimType_DriftWanderTimeSec` (advanced); cli/viewer
  options of the same meaning; the block's `driftTrajectory` takes them in `opts`. A live Live/MDA sequence now starts
  its drift exactly at its first frame (a frame already in flight at the start was counted with the old origin).
- `General_StackLength` is back (frames of a precomputed stack, default 1000): the MM test scripts
  (`tools/test_insiliscope.py`, `tools/test_cellfield_stage.py`, now with `--only <sections>` and per-check timings)
  use short stacks and run in under 5 minutes (were ~25).
- MM adapter, live mode (2026-10-05): a snap takes a frame started after the snap was called, and a sequence
  acquisition frames started after it began. The first snap after a stage move used to return a frame already in
  flight at the old pose, and consecutive snaps could share illumination clocks (the history looked as if an idle
  live loop bleached).
- **seed** Presets and the EMCCD gain (2026-10-05). DNA-PAINT light presets and the default 640 nm line 1 kW/cm² (were
  0.16); PALM suggested labelling 25 % (was 5 %) and its 405 nm line 0.002 kW/cm² (was 0.01). The camera gain (e⁻/ADU) is
  per photoelectron for sCMOS and EMCCD alike; the iXon preset's is 0.0066 (about 150 ADU per photoelectron) in dSTORM,
  PALM and DNA-PAINT and 0.1 in WideField and BrightField (a mode or modality change re-applies it), and the EM gain is
  no longer a value of its own: the preset's pre-amplifier sensitivity (1 e⁻/ADU) / the gain. MM `CamParam_EmGain` is
  read-only; cli `em-gain` defaults to -1 (derived). The per-pixel gain floor is 4 % of the nominal gain (0.01 at the
  default 0.25: unchanged).
- Viewer (2026-10-05): **Preview PSF** (Objective & PSF, Advanced): the movie's own kernel (new WASM export
  `isc_scope_psf_preview`) oversampled beside its camera-pixel splat, with a z slider, in the movie player. The label
  group reads labelled sites, mode, dye (the dyes with data for that mode), orientation; Illumination and Light path
  are one group whose preset is also the modality (BrightField, lasers off); the chamber height sits under Geometry;
  the QE curve select became a "use" checkbox beside the flat QE; the EM gain input is a derived readout; About is its
  own section.

- **seed** Labels, dyes and the light path (issue 16, 2026-10-05). Each structure (the microtubules for now) carries a
  label: a dye of the new library (`data/dyes/`: FPbase spectra, literature kinetics, every value with a reference or
  marked as an estimate) in a mode -- dSTORM (initial ON, intensity-scaled times), PALM (pre-converted state, 405 nm
  and primed activation), DNA-PAINT (imager binding at k_on c, the free imager's flat background; depletion and
  exclusion from cells ignored) or WideField (every dye at once, bleaching by its photon budget). Lasers, dichroic,
  emission filter and camera QE curve (presets for both) set excitation, detected fraction and each state's PSF
  wavelength; continuous populations render mean-field or per dye. cli/viewer movies start 60 s after the
  illumination (`start-sec`); the MM adapter keeps a world-anchored illumination history instead (below). **Core ABI 10** (`isc_world_set_label`, events stride 10, `isc_continuous_in_window`, sites stride 5;
  `isc_world_set_kinetics` and the `labelEfficiency`/`labelNonBleaching` params removed); webSMLM block abiVersion 10.
  cli/viewer options: `mt-dye`, `mt-mode`, `mt-label-pct`, `mt-imager-nm`, `mt-orient*`, `dye1..3.source`, dye-field
  overrides, `laser-*`, `light-preset`, `dichroic`, `em-filter`, `camera-preset`, `qe-curve`, `mean-field-*`;
  removed `photons-per-sec`, `on-sec`, `off-sec`, `bleach-prob`, `photon-cv`, `wavelength-nm`, `milli-activation-rate`,
  `labeling-pct-*`, `wf-excitation-*`, `wf-quantum-yield`, `wf-photon-budget`, `wf-extinction-coeff`; `modality` is
  Fluorescence | BrightField. MM: new `SimType_CellFieldMicrotubule*`, `FluoParam_Microtubule_*`,
  `FluoParam_Dye{1,2,3}_*`, `Optics_*` (new prefix; the illumination profile moved from `FluoParam_Illum*`),
  `CamParam_CameraPreset`/`QeCurve`, `General_MeanField*`; removed `FluoParam_PhotonsPerSecond`, `OnLifetimeSec`,
  `OffLifetimeSec`, `BlinkBleachProb`, `PhotonCV`, `FluoParam_WideField*`, `PSFParam_PsfEmissionWavelengthNm`,
  `SimType_CellFieldLabelingPct*`, `MilliActivationRatePerDyePerSec`; the live WideField bleach map is replaced by
  an illumination history for every label mode: each place's dyes run on the seconds of light it has had (snaps,
  sequence acquisitions and stacks add to it; a place never lit starts fresh at 0), so imaging bleaches and uses up
  dyes only where you imaged; stacks continue it (reproducible on a fresh device). Default: DNA-PAINT ATTO 655, 70 % of the sites, 1.43 nM.
  Hardware configurations that set the removed properties must be re-made.
- MM adapter: the non-CellField patterns are gone (issue 16, 2026-10-05): `SimType_Pattern` and its Circle, Lines, Grid,
  Random, CustomPoints, Spiral, Star, Heart, ResolutionTarget, TiltedPlane, Uniform3D, Shell, NUP, Calibration9Spots and
  FilamentsRing values, with `SimType_CustomPointsFile`, `SimType_ResolutionSpacingsNm`, `General_EmitterDensityPerSec`,
  `General_LabelingEfficiencyPct`, `SimType_Structure*`, `SimType_Nup*` and the background extras that needed them
  (`Background_CellContrast`, `HazeWeight`, `HazeWidthNm`, `OutOfFocusRatio`, `OutOfFocusDepthNm`). The cell field is
  the only specimen; its output is unchanged (`adapter_pixel_hash` CellField configs identical). Hardware
  configurations that set the removed properties must be re-made.
- Viewer data layers and animations on labels, dyes and the light path (issue 11 on issue 16, 2026-10-05): WideField
  stacks image the microtubule dye in WideField mode and SMLM stacks in its blinking mode (with that mode's light preset);
  BrightField is modality 1. Localizations take the label's kinetics (the `events` job carries whole labels,
  `isc_world_set_label`) and the dye's detected photons through the light path; SMLM planes start at the movie's start
  time. Dyes take the dye's emission colour in every look; the animation legend names the dye and mode.
- Viewer animations, more presets and automatic data (issue 11, 2026-10-05): 18 more cycle presets (27) and 5 more
  ready-made sequences; checked data layers and animations that need data acquire it by themselves.
- Viewer thresholded surface and localizations (issue 11, phase 4, 2026-10-05): the WideField z-stack smoothed,
  thresholded at Otsu's level and meshed (surface nets) in a compute worker, drawn as a lit front layer; SMLM
  localizations of a multi-plane acquisition (the core's blinks, a new `events` worker job in the WASM and JS engines:
  per frame and focus position within the capture range, displaced by a photon- and defocus-dependent precision),
  drawn by height or as precision spots. Main view (Data layers) and animations (presets "Simulated up, WideField down,
  thresholded wake", "Simulated up, SMLM down, localizations wake"; the issue's example as a ready-made sequence).
  `web/scene/compute.js`, check `tests/web/scene_compute_check.mjs` (CI `viewer-js`); `engine_check` compares the events.
- Viewer data layers (issue 11, phase 3, 2026-10-05): WideField, SMLM-frame and BrightField z-stacks of a cell (one
  movie job per focus position over the cell's box, averaged frames; memory + IndexedDB cache), drawn as slices at any
  height or any vertical plane (3D texture per fragment, crop to the cell's footprint), in the main view (Data layers
  group: Acquire, slice height) and in animations (slices riding the sweep plane; the export acquires first; preset
  "Simulated up, image slices down"). Fresh or sequential SMLM planes. Check `tests/web/viewer_scene.mjs` (clip bands,
  rotation, determinism, detail budget, stack plane = movie job).
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
