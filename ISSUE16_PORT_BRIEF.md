# Brief for the agent porting issue 16 to C++

You are porting a finished, user-approved JavaScript model to the C++ core, the render engine, the Micro-Manager
adapter, the cli, the WASM viewer module and the webSMLM block of **inSiliScope** (`C:\GitHub\inSilicell`). The JS is
the reference: make the C++ match it, do not redesign it. Read this file, then `CLAUDE.md` (project rules, binding),
then `PORT_PENDING.md` (the exact list of what changed in the JS and what the port must do). Delete this brief and
`PORT_PENDING.md` in the last commit of the port.

## Where things stand

- Branch **`claude/multi-dye`** (from `main`, local, **not pushed**: ask the user before pushing or opening the PR).
  Commits so far:
  - `39eb295` phase 0: the non-CellField MM patterns are gone (CellField pixel hashes unchanged);
  - `b24ef9f` phase 1: `data/dyes/` (FPbase-backed dye/filter/camera data), `data/references.json`,
    `tools/fetch_fpbase.mjs`, `tools/gen_dye_library.mjs`;
  - `00b56ca`, `a3884e4`, `b582e27`, `9c0dadf` phase 2: the JS model and the viewer (iteration mode,
    `PORT_PENDING.md` exists, so CI's `port-gate` fails on purpose).
- Issue: GitHub issue 16 of `kjamartens/inSiliScope` ("more structured backend for multiple labels/dyes").
  Approved plan: `C:\Users\kjamartens\.claude\plans\start-a-new-branch-synchronous-moth.md` (decisions table, speed
  section). `PORT_PENDING.md` "Decided with the user" lists the later decisions.
- The JS works end to end: `node web/lab/serve.mjs --open` -> http://localhost:8123/web/lab.html (the viewer on the JS
  engine). The production viewer (`web/index.html` on the committed WASM) is broken until you regenerate the module.

## The model in one paragraph

Structures (only `microtubules` now, a registry `STRUCTURES` for more) each carry one **label**: a dye (library entry
or one of three user slots, with overrides), a **mode** (dSTORM, PALM, DNA-PAINT, WideField), a labelled-site density
(suggested per mode), the dye's fluorescent fraction, orientation (Free/Fixed/Random + wobble; not rendered yet),
motion (Static only) and an off-target list (must be empty). A **light path** (lasers 405/488/561/640/730 + custom
in kW/cm^2, dichroic, emission filter, camera QE curve; **light presets** per modality/colour) turns each dye state into
an excitation rate, a detected fraction, an effective PSF wavelength and detected photons/s. One **fluorescence movie**
renders every label: blinks with one PSF kernel per (structure, state), continuous populations (WideField dyes, PALM pre
states, the dSTORM initial ON) mean-field (FFT) or per dye (running image) depending on emitter density, and the
DNA-PAINT free imager as a static offset. dSTORM times scale with excitation relative to Dempsey et al. 2011's
measurement conditions. Movies start 60 s after the illumination by default.

## Rules that bite (from CLAUDE.md; read it all)

- **Address-based determinism, bit-exact with the JS.** Every draw is `pcg4d` on `(seed, address, channel)`; new
  channels are in `PORT_PENDING.md` (FLUOR 6, INIT_ON 7, AUX 8, ORIENT_U 9, ORIENT_PHI 10, reserved MOTION 11 and
  OFFTARGET 12). Keep JS operand order; never two draws from one stream in one expression; `isc::jsm::*` for
  transcendentals; no `-ffast-math`. Results consumed in serial order when threaded.
- The JS is the reference for imaging too: `tests/parity/scope_parity.mjs` must pass again (SR 100 % identical ADU,
  mean-field >= 99.9 %, BrightField >= 99.5 %), plus new cases (below).
- `ISC_WORLD_VERSION` stays (no cell moves). Golden vectors: re-freeze only if a golden line legitimately changes,
  with Node 24 (`node tests/parity/golden.mjs --freeze`).
- Generated files are never hand-edited: `web/insiliscope_module.js` (`tools/embed_web_module.mjs`), the HLSL
  (`tools/gen_wf_gpu.mjs`), `jsmath_fdlibm.cpp`, and now `dye_library_data.js`, `web/dye_library.js`,
  `docs/references.md` and the new `DyeLibraryData.inc` (`tools/gen_dye_library.mjs`; add the C++ output and keep
  `--check` passing; CI already runs it).
- **References** (user rule): every literature value gets a full reference in `data/references.json` and its key
  next to the value; check citations against the source (Europe PMC full text where open), never from memory; values
  without a source say *estimate*.
- Docs in step with code in the same commit (see CLAUDE.md "Keep the online docs in step").
- MM property names carry a group prefix; this port adds **`Optics_`** (update CLAUDE.md's list). Ask the user before
  touching `C:\GitHub\websmlm` (its `PARITY.md` needs a note).
- Windows: MSBuild from Git Bash needs `MSYS_NO_PATHCONV=1` and `-p:` switches; a stale DLL hashes "wrong" (check
  its timestamp); LNK1104 = a leftover pymmcore test holds the DLL. Benchmarks on this machine vary 15-25 %: best of
  3, same session.
- Commit per step, ending with the `Co-Authored-By` line the session gives you.

## Port in this order (one commit each, tests green each time)

1. **Core** (`core/src/dyes.*`, `world.*`, `params.*`, `capi.cpp`, `core/include/insiliscope/insiliscope.h`):
   mirror `web/prototype/scope/dyes.js` and `world.js` line for line: `makeLabel`/validation (refuse non-Static
   motion and non-empty off-target with a clear "not implemented yet"), `dyesInBlock(density, fluorescentFraction)`,
   `labelSchedule`, `dyeSchedule(..., tMax)`, the windowed `schedule(b, tLo, tMax)`, `continuousInWindow(tMin)`,
   `dyeOrientation`, density by structure mask. **ABI 10** as in `PORT_PENDING.md` (set_label replaces set_kinetics
   and the label params; events stride 10; sites stride 5; `isc_continuous_in_window`; density3d structure mask).
   Bump `ISC_ABI_VERSION` and every consumer's check (`web/index.html`, `tests/parity/wasm_abi_smoke.mjs`,
   `CellFieldSource.cpp`). `world_tests.cpp`: label determinism under any query history and window, FLUOR nesting,
   the old bleaching/persistent equivalence (as `web/lab/label_regression.mjs`), orientation statistics, refusals,
   8 threads = 1. Memory: 70 % labelling of a 128 px FOV is ~5.4 M dyes; keep dye storage compact.
2. **Engine** (`adapter/inSiliScope/Simulation/`): `Spectra.*` = `spectra.js`, `DyeLibrary.*` + `LightPath.*` =
   `dye_library.js` (effective dye with slot + structure overrides, light path, `statePhotophysics`,
   `labelPhotophysics` incl. the dSTORM excitation scaling and the 405/primed activation, imager background, camera
   presets, light presets, `backgroundQe`), `DyeLibraryData.inc` from the generator, `ScopeMovie.cpp`
   `FluorescenceMovie` = `fluorescence.js` (groups, kernel memo of groups + 1, `kernelWavelengthNm` 2 nm rounding,
   blinks into one image, mean-field switch, running image with a double accumulator, partial-frame overlaps, imager
   offset, noise at QE 1, per-frame backend progress), `WidefieldRender` mean-field image of a structure on a grid
   that spans the FOV + 2 um margin (`widefield.js` `meanFieldImage`), the GPU WideField job per kernel group
   (summed; CPU fallback, `General_GpuStatus`), BrightField QE at its wavelength. `ScopeMovieOptions()` = JS
   `SCOPE_OPTIONS` (same names, order, defaults, help, `light-preset`, `mt-label-pct -1`, `start-sec 60`).
3. **cli, WASM module, viewer, block**: cli options follow `ScopeMovieOptions`; `node tools/embed_web_module.mjs`;
   in `web/index.html` restore the WASM paths (sites stride 5, `labels` on sites jobs, the WebGPU mean-field path
   behind `d.meanFieldGpu` today, `movie-progress` messages from the WASM worker); `tools/make_cellfield_block.mjs`
   (abiVersion 10, same `buildWindow` output). `node web/lab/engine_check.mjs` must pass (lab.html engine = WASM).
4. **Adapter** (MSBuild): properties in `PORT_PENDING.md` item 3, with the viewer's behaviour:
   - `SimType_CellFieldMicrotubuleDye` (library names + Dye1..Dye3), `...LabelMode` (dye default | 4 modes),
     `...LabelingPct`, `...ImagerNm`, orientation/motion properties;
   - picking a dye or mode **loads** `FluoParam_Microtubule_*` from the library (editable after), sets
     `...LabelingPct` to the mode's suggestion on a mode change, and applies the dye mode's **light preset** to the
     `Optics_*` properties (`Optics_Preset` shows it; picking a preset writes the lasers, dichroic and filter);
   - `FluoParam_Dye{1,2,3}_Source` + fields (slots), `Optics_*` lasers/geometry/chamber/dichroic/filter/presets and
     the illumination profile moved from `FluoParam_Illum*`, `CamParam_CameraPreset`/`QeCurve` (a preset writes the
     camera properties), `General_MeanField*`, `General_ImagingModality` Fluorescence | BrightField;
   - the stack's and live mode's clock start at 60 s; remove the properties listed in `PORT_PENDING.md`.
   Update `tools/test_insiliscope.py`, `tools/test_cellfield_stage.py`, `tools/adapter_pixel_hash.py` (outputs change
   on purpose: record new baselines), `build_adapter_linux.sh` if needed.
5. **Checks and benchmarks**: `bash tools/port_check.sh` (native + WASM ctest, golden, scope_parity, engine_check,
   block). Add `scope_parity` cases: each mode, a PALM pre state, two lasers, a per-dye frame and a mean-field frame,
   a Gaussian-PSF slot dye, `light-preset=auto`, start 0 and 60 s. `tests/web/*` viewer movies (headless Chromium).
   Bench (`tools/bench.py`): today's configs before/after (the default must not get slower) plus
   `dstorm-128px-1000f`, `palm-green-128px-1000f`, `paint-bg-128px-1000f`, `wf-2group-256px-200f`.
6. **Docs**: `spec/PORT.md` new section 16 (the model, ABI 10, channels), `spec/ALGORITHM.md`, new
   `docs/physics/dyes-and-light-path.md` (physics, presets, the mean-field switch, the imager background **with the
   note that imager depletion and exclusion from cells are ignored**), update structures/photophysics/camera/
   quickstart/try-viewer/roadmap/README, `gallery/manifest.json` entries (dSTORM AF647, PALM mEos3.2 with its green
   state, DNA-PAINT ATTO 655 + background, mEGFP widefield, a crosstalk example), `CHANGELOG.md`, `PLAN.md` (tick
   issue 16's phases), `CLAUDE.md` (new properties, `Optics_` prefix, the data/ and references rules are already in).
   Then delete `PORT_PENDING.md` and this brief.

## Checks that already exist on the JS (keep them passing)

- `node web/lab/check.mjs` (every mode renders, finite, not flat), `node web/lab/label_regression.mjs --base main`
  (label model vs the old two-population world, event for event; FLUOR nesting; dSTORM/WideField/PALM windows;
  orientation statistics; refusals), `node tools/gen_dye_library.mjs --check`.
- Mean-field vs per-dye agree on the mean image (1 % total, ~4 % per pixel at 2 % labelling); keep that true in C++.

## Pitfalls already found

- Dense labels: never schedule whole dye lifetimes or build per-dye windows for mean-field populations (the JS ran out
  of memory at 70 % AF647 before the windowed schedule).
- The mean-field grid must include the 2 um margin or it is ~20 % dimmer than the per-dye path.
- dSTORM at 50 ms frames saturates during the initial ON; the 60 s start avoids it by default.
- The lab keeps changed controls in sessionStorage (click the orange badge to reset); in the browser pane,
  `requestAnimationFrame` does not run while the pane is hidden (call `draw()` by hand when testing).
- Bash heredocs choke on Python with triple quotes: write scripts to a file first.

## Ask the user before

pushing or opening the PR; touching `C:\GitHub\websmlm` or `C:\GitHub\demoCam_SMLM_MM`; changing any default or model
choice the JS made; adding a value without a source.
