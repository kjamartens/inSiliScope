# Port pending: issue 16, structures / dyes / light path

The JS reference (`web/prototype/scope/`, `web/index.html`, `web/lab/`) is ahead of the C++ core and engine. This file
says what the port (issue 16 phase 3, same PR) must do; delete it when `bash tools/port_check.sh` passes again.
Plan: `C:\Users\kjamartens\.claude\plans\start-a-new-branch-synchronous-moth.md` (approved 2026-10-05).

## What changed in the JS

- **Labels per structure** (`scope/dyes.js` `makeLabel`, `LABEL_MODES`; `scope/world.js` `STRUCTURES`, `setLabels`):
  one label per structure (only `microtubules` now), `{density, fluorescentFraction, mode: dSTORM | PALM | DNA-PAINT |
  WideField, kinetics (+ initialOnSec), preState, orientation {mode Free|Fixed|Random, polarDeg, azimuthDeg,
  wobbleDeg}, motion 'Static', offTarget []}`. Non-empty `offTarget` and motion other than Static are refused ("not
  implemented yet"). `World(P, seed, geometryParams, labels)`; `labelEfficiency`/`labelNonBleaching` and
  `setKinetics` are gone.
- **Dye draw** (`dyesInBlock(..., density, fluorescentFraction)`): labelled iff LABEL draw `u < density` (the old
  threshold, so `density = e + p` gives the old dye set), fluorescent iff `fluorescentFraction >= 1` or the new
  **FLUOR** draw `< fluorescentFraction` (no draw at 1). Dyes carry `theta` (protofilament azimuth) and `structure`.
- **Channels** (`DYE_CH`): FLUOR 6, INIT_ON 7, AUX 8, ORIENT_U 9, ORIENT_PHI 10, MOTION 11 (reserved), OFFTARGET 12
  (reserved). Existing channels unchanged.
- **Schedules** (`labelSchedule`): DNA-PAINT = `persistentGen` (rate = k_on x imager concentration); PALM =
  `dyeSchedule` (+ a PRE window [0, first blink) with an AUX draw when `preState`); dSTORM = an INITIAL_ON window
  [0, Exp(initialOnSec)) (INIT_ON draw) then `dyeSchedule` shifted by it; WideField = one ALWAYS_ON window [0, inf) with
  an AUX draw. `EVENT_STATE` BLINK 0, PRE 1, INITIAL_ON 2, ALWAYS_ON 3. Blinks sorted by tOn per block as before;
  continuous windows in dye order.
- **Queries**: `eventsInWindow` adds `structure, state 0, aux 0`; new `continuousInWindow` (all times, dye order);
  `density3d(..., structureMask)`; `sitesInWindow` dyes carry `structure`; `dyeOrientation(d)` (not used by the
  renderer; statistics checked in `web/lab/label_regression.mjs`).
- **Imaging** (`scope/spectra.js`, `scope/dye_library.js`, `scope/fluorescence.js`, `scope/widefield.js`
  `meanFieldImage`, `scope/scope_movie.js`): modality 0 = Fluorescence, 1 = BrightField. Per (structure, state):
  k_exc = sum over lasers of sigma(lambda) x flux x (1 - T_dichroic(lambda)); k_em = QY k_exc; detected fraction
  F = sum(E T_D T_F QE)/sum(E), lambda_eff (the PSF's, rounded to 2 nm); detected photons/s = k_em eta F. Noise chain
  at QE 1 (BrightField: QE(bf-wavelength)); flat background x QE at the filter centre; DNA-PAINT free imager = a static
  offset c N_A H A_px x detected rate. Kinetics: dSTORM k_act = 1/offSec + a405 I405 (offSec_core = 1/k_act); PALM
  k_act = spont + a405 I405 + primed I(470-510) I(690-780); DNA-PAINT rate k_on c.
  Frame = background + imager, then blinks per group (own kernel, `renderPhotonImage(..., into)`), then continuous
  populations: mean-field (`meanFieldImage` of the structure x the exact mean photons per dye
  rate (e^-lt0 - e^-lt1)/l) while expected emitters exceed `mean-field-density-per-um2` in the `mean-field-slab-nm`
  slab or `mean-field-max-emitters` in the z range, else per dye through a running image (unit splats added/removed
  as windows start/end, double accumulator; partial frames by overlap). Per-dye bleach: aux x budget / k_em.
  The mean-field grid now spans the FOV + the 2 um margin (was the FOV square), so it equals the per-dye path's mean
  (`web/lab` check: within 1 % total, 4 % per pixel at 2 % labelling).
- **Data**: `data/dyes/*`, `tools/fetch_fpbase.mjs`, `tools/gen_dye_library.mjs` (writes
  `web/prototype/scope/dye_library_data.js`, `web/dye_library.js`, `docs/references.md`; generic blocks for modes a
  dye has no data for).
- **Options** (`SCOPE_OPTIONS`): removed `photons-per-sec`, `on-sec`, `off-sec`, `bleach-prob`, `photon-cv`,
  `wavelength-nm`, `milli-activation-rate`, `labeling-pct-*`, `wf-excitation-*`, `wf-quantum-yield`,
  `wf-photon-budget`, `wf-extinction-coeff`; `modality` is 0 Fluorescence / 1 BrightField. New: `mt-dye`, `mt-mode`,
  `mt-label-pct`, `mt-imager-nm`, `mt-orient`, `mt-orient-polar-deg`, `mt-orient-azimuth-deg`, `mt-wobble-deg`,
  `mt-motion`, `dye1..3.source`, `<prefix>-dye.<field>` / `dye<N>.<field>` overrides (`DYE_FIELDS`), `laser-405..730`,
  `laser-custom-nm`, `laser-custom`, `illum-geometry`, `chamber-height-um`, `dichroic`, `dichroic-edge-nm`,
  `em-filter`, `em-lo-nm`, `em-hi-nm`, `camera-preset`, `qe-curve`, `camera-type`, `em-gain`, `cic`, `bit-depth`,
  `mean-field-density-per-um2`, `mean-field-slab-nm`, `mean-field-max-emitters`. Camera options take the preset's
  values unless given. Defaults: ATTO 655 DNA-PAINT, 70 %, 1.43 nM, laser-640 0.1607 kW/cm^2, LP650 + 676/37,
  Kinetix22.
- **Viewer** (`web/index.html`): Microtubule label, Dye slots, Light path (spectra plot from the JS physics),
  lasers, camera presets, mean-field rows; dyes in their emission colour; sites jobs carry `labels`; the movie
  spec is every `mv_` control; the WebGPU WideField movie is off (`d.meanFieldGpu`) until the port wires it.

- **Scale** (2026-10-05, a default AF647 dSTORM movie ran out of memory: 70 % labelling = 5.4 M dyes in a 128 px FOV):
  blinks are scheduled only up to a horizon (twice the latest query end; `dyeSchedule(..., tMax)` stops at the first
  blink starting after it -- the earlier blinks are the same, so it is exact); continuous windows are built on first
  use (`continuousOf`), only for populations that reach the per-dye path; dye blocks are packed typed arrays
  (`forEachDye` for counts and mean-field planes, no per-dye objects). 2.4 GB -> 230 MB, 30 s (JS). The C++ schedules
  whole lifetimes today: port the horizon too.
- **Progress**: `fluorescence.js` reports per frame which backend drew it (SMLM splat for blinks; mean-field or per
  dye for each continuous population); the lab engine posts `movie-progress` messages and the viewer shows them on
  the movie button (`onMovieProgress`). The WASM worker must post the same (isc_scope_movie progress callback).

## The port must

1. **Core** (`core/src/dyes.*`, `world.*`, `params.*`, `capi.cpp`, `insiliscope.h`): the label model above,
   bit-exact with the JS; **ABI 10**: `isc_world_set_label(w, structure, const double* v, n)` with an `ISC_LABEL_*`
   layout (density, fluorescent fraction, mode, the kinetics, initialOnSec, preState, orientation mode/angles/wobble,
   motion, off-target count) replacing `isc_world_set_kinetics` and the `labelEfficiency`/`labelNonBleaching` params;
   events stride 10 (+ structure, state, aux); `isc_continuous_in_window`; sites stride 5 (+ structure);
   `isc_density3d_in_window(..., structureMask)`; `ISC_STRUCT_MICROTUBULE 0`. `PackingFingerprint` unaffected (cells
   do not move: `ISC_WORLD_VERSION` stays). `world_tests.cpp`: label determinism, FLUOR nesting, orientation
   statistics, refusals; `tests/parity` golden lines for the new channels.
2. **Engine** (`Simulation/`): `Spectra.*`, `LightPath.*`, `DyeLibrary.*` (+ `DyeLibraryData.inc` from
   `tools/gen_dye_library.mjs`, `--check`), `CameraPreset`, `ScopeMovie.cpp` FluorescenceMovie = `fluorescence.js`
   (kernel memo groups + 1, running image, mean-field switch, imager background), `WidefieldRender` mean-field image
   per structure (grid with the margin), GPU WideField job per kernel group (summed; CPU fallback), BrightField QE.
   `ScopeMovieOptions` = `SCOPE_OPTIONS`. `CellFieldSource`: labels, the new strides.
3. **Adapter**: `SimType_CellFieldMicrotubule{Dye,LabelMode,LabelingPct,ImagerNm,Orientation,OrientPolarDeg,
   OrientAzimuthDeg,WobbleConeDeg,Motion}`, `FluoParam_Microtubule_*` (loaded on a dye pick), `FluoParam_Dye{1,2,3}_*`,
   `Optics_*` (lasers, geometry, chamber height, dichroic, filter, the illumination profile moved from
   `FluoParam_Illum*`), `CamParam_CameraPreset`/`QeCurve`, `General_MeanField*`, `General_ImagingModality`
   Fluorescence | BrightField; remove `FluoParam_PhotonsPerSecond`, `OnLifetimeSec`, `OffLifetimeSec`,
   `BlinkBleachProb`, `PhotonCV`, `WideField*`, `PSFParam_PsfEmissionWavelengthNm`, `SimType_CellFieldLabelingPct*`,
   `MilliActivationRate`. CLAUDE.md: the `Optics_` prefix; ask before updating websmlm's PARITY.md.
4. **cli / WASM / block**: options; `tools/embed_web_module.mjs`; viewer sites stride 5 and the WebGPU path back on;
   webSMLM block abiVersion 10 (same `buildWindow`).
5. **Checks**: `bash tools/port_check.sh`; `scope_parity.mjs` cases for every mode, a pre state, two lasers,
   per-dye and mean-field frames; `tests/web` viewer movies; `tools/adapter_pixel_hash.py` configs (outputs change on
   purpose); bench configs `dstorm-128px-1000f`, `palm-green-128px-1000f`, `paint-bg-128px-1000f`,
   `wf-2group-256px-200f` with today's configs before/after.
6. **Docs**: spec/PORT.md section 16, ALGORITHM.md, `docs/physics/dyes-and-light-path.md` (new; the imager
   depletion note), structures/photophysics/camera/quickstart/try-viewer/roadmap/README, gallery entries, CHANGELOG,
   PLAN.md.

## Open questions for the user

- Default label density 70 % suits DNA-PAINT; dSTORM/PALM labelling is a few %, and 70 % with a bleaching dye is slow
  (a schedule per dye). Keep one default for all, or let a dye's mode suggest a density?
