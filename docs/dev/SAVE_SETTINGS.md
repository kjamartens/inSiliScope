# Save and load settings (2026-10-28, branch `lamin_network`)

A setup as a file, `<name>.isc.json`, in five parts that can be mixed: the **instrument** (hardware), the
**settings** (how the instrument is driven, and the render quality), the **sample**, the **measurement** (where and
how long it is imaged, and what has been done to the sample: its light history) and, optional, the **view** (the
viewer's own looks). The same sample on another microscope; a known microscope with a new sample (towards a digital
twin); a session handed to a colleague or to the back-end, which renders it (`insiliscope_cli --settings`) and later
its ground truth.

Code: `web/live/settings_file.js` (the viewer: save, load, the ☰ menu's section and the two icons beside the settings level), `web/live/live.js` (the light history log
and its replay), `cli/settings_file.*` (the reader, the native replay), `ScopeOptionPart` /
`ScopeOptionNames` (`adapter/inSiliScope/Simulation/ScopeMovie.*`: the part of every option, the names of its values),
`LiveScope::Advance` (`Simulation/LiveScope.*`: the live sample's state after n frames, without rendering), the
module's `isc_scope_options` (now `name \t default \t help \t part \t names|...`), `isc_live_advance`, `isc_live_time`.
Checks: ctest `cli_settings` (`cli/settings_check.cpp`, 0.3 s), ctest `live_scope` (replay = taken frames, dSTORM,
DNA-PAINT, drift), `tests/web/viewer_settings_file.mjs` (~10 s).

## The file

```json
{ "format": "insiliscope-settings", "version": 1,
  "made": { "app": "viewer", "release": "0.1.0", "worldVersion": "2026-10-05", "date": "2026-10-28T..." },
  "parts": {
    "instrument":  { "name": "lab scope", "id": "idj0c3", "summary": "100× NA 1.4 · ...",
                     "values": { "na": 1.4, "camera-preset": "Kinetix22", "read-noise": 1.2, ... },
                     "viewer": { "objectiveMag": 100, "emissionMag": 0.667 } },
    "settings":    { ..., "values": { "laser-640": 1, "dichroic": "...", "modality": "Fluorescence", ... } },
    "sample":      { ..., "values": { "specimen": "CellField", "mt-dye": "ATTO655", "p.cellDiamMin": 18, ... },
                     "viewer": { "userDyes": [ { "slot": 1, "name": "My dye 1", "mode": 0 } ] } },
    "measurement": { ..., "values": { "x": 14.8, "y": -6, "z": 0.5, "exposure-ms": 50, ... },
                     "viewer": { "recordFrames": 100 },
                     "history": { "tSec": 132.5, "records": [ { "n": 240, "lit": true, "set": { "x": 14.8, ... } }, ... ] } },
    "view":        { ..., "values": { "tiltDeg": 30, "mtMap": "...", "view.scale": 9, "ui.settingsLevel": "advanced" } } } }
```

- **Every value** of a part, defaults too: each engine option of that part as the engine got it (the parsed
  `movieSpec()`, the engine's default for a key the viewer did not send), plus the spec's other keys (`p.*`, `zern.*`,
  the dye field overrides). Taken from the spec, not the controls, so what the viewer forces is in the file
  (`psf-kernel-half-width-nm=3000`, the cli's default is 7000; `ISC_MM_EXTRA`; `world-seed`).
- **Enumerations by name** (`ScopeOptionNames`: dyes, modes, cameras, filters, presets), so a file means the same when
  the library grows; numbers stay numbers. A reader takes names, numbers, booleans and the specials `Typical`,
  `DyeDefault`, `Global`, `None` (= `ScopeOptionValue`).
- `viewer`: what only the viewer keeps (the objective's magnification the pixel size comes from, the custom dye slots,
  Record's N); back-ends ignore it.
- `id`: 6 base-32 characters of a 53-bit hash (cyrb53) of the part's values: the same values, the same id. The ☰ menu
  shows it, and "edited" when the values moved since the part was loaded or saved.
- Any subset of parts. `version` is the format's; a newer one is read as far as it is known, with a warning. Renamed
  options go in `RENAMED` (`settings_file.js`, none yet). `made.release` / `made.worldVersion` differing from the
  reader's: a warning ("the same settings, the image may differ"), never a refusal.

## The parts

One source: `PartOf` in `ScopeMovie.cpp`; `Options()` throws when an option has none, so a new option must be placed.
`p.*` are sample, `zern.*` instrument, dye field overrides sample. `host` options (`disk-cache`, `prepare`,
`illum-geometry`) change no image and are never saved. The view part is the viewer's (control ids, `view.scale`,
`ui.settingsLevel`, `ui.style`); unticked by default in Save.

- **instrument** (34): `pixel-nm`, `na`, `laser-custom-nm`, `illum-profile`, `illum-fwhm`, `ex-lo-nm`, `ex-hi-nm`,
  `dichroic-edge-nm`, `em-lo-nm`, `em-hi-nm`, `fid-blocking-od`, `camera-preset`, `qe-curve`, `qe`, `camera-type`,
  `dark-per-sec`, `gain`, `offset`, `offset-std`, `read-noise`, `gain-std-pct`, `read-noise-std-pct`, `cic`,
  `bit-depth`, `bf-condenser-na`, `bf-wavelength-nm`, `bf-aberrations`, `immersion-index`, `psf-model`,
  `psf-zernike-preset`, `psf-mask`, `psf-mask-modes`, `psf-mask-waist`, `psf-working-distance-um`; `zern.*`
- **settings** (43): `size`, `z-range-um`, `laser-405`, `laser-488`, `laser-561`, `laser-640`, `laser-730`,
  `laser-custom`, `light-preset`, `illum-bfp`, `ex-filter`, `dichroic`, `em-filter`, `em-gain`, `modality`,
  `light-epi`, `light-trans`, `wf-upscale`, `wf-plane-nm`, `wf-kernel-um`, `mean-field-density-per-um2`,
  `mean-field-slab-nm`, `mean-field-max-emitters`, `blink-binned-density-per-um2`, `blink-binned-max-emitters`,
  `blink-mean-field-density-per-um2`, `blink-mean-field-max-emitters`, `blink-binned-upscale`, `bf-quality`,
  `bf-sources`, `bf-upscale`, `bf-sub`, `bf-slice-um`, `bf-margin-um`, `bf-photons-per-px-per-sec`,
  `psf-oversampling`, `psf-kernel-half-width-nm`, `psf-z-range-um`, `psf-z-step-um`, `psf-pupil-samples`,
  `psf-halo-cut`, `psf-interp`, `blink-min-photons`
- **sample** (104): `seed`, `world-seed`, `background-per-sec`, `specimen`, `mode`, every target's label options
  (`<prefix>-dye`, `-mode`, `-label-pct`, `-imager-nm`, `-paint-kon`, `-paint-on-sec`, `-orient`, `-orient-polar-deg`,
  `-orient-azimuth-deg`, `-wobble-deg`, `-motion`, `-binder`, `-binder-nm`, `-binder-flex-nm`, `-linker-min-nm`,
  `-linker-max-nm` for `mt`, `npc`, `lamin`, `ruler`), `dye1.source`..`dye3.source`, `chamber-height-um`, `chunk-um`,
  `occupancy`, `cell-diam-min-um`, `cell-diam-max-um`, `mt-density`, `npc-density`, `lamin-density`, `packing`, the
  `origami-*` and `fid-*` options (but `fid-blocking-od`), `bf-n-medium`, `bf-n-cytoplasm`, `bf-n-nucleus`,
  `bf-n-microtubule`, `bf-absorption-per-um`, `psf-sample-index`, `psf-sample-depth-nm`; `p.*`, the dye fields
- **measurement** (17): `x`, `y`, `z`, `frames`, `exposure-ms`, `start-sec`, the `drift-*` options, `focus-um`; and
  the light history

The choices between parts (each one decided on its own; move one by editing `PartOf`):

| option | part | why |
|---|---|---|
| `size` | settings | the camera ROI one images with, not the sensor |
| `em-gain` | settings | set per acquisition; the EMCCD's noise is the instrument's |
| `illum-profile`, `illum-fwhm` | instrument | the beam's shaping optics |
| `illum-bfp` | settings | the TIRF/HILO knob one turns |
| `ex-filter`, `dichroic`, `em-filter` | settings | which filter is in place; the custom edges (`ex-lo-nm` ...) are the hardware |
| `laser-custom-nm` / `laser-custom` | instrument / settings | the line is the hardware, its power the setting |
| `qe-curve`, `camera-type` | instrument | the sensor |
| `psf-*` render numerics (`-oversampling`, `-kernel-half-width-nm`, `-halo-cut`, ...) | settings | render quality, beside `wf-*`, `blink-*`, `bf-quality` |
| `psf-sample-index`, `psf-sample-depth-nm` | sample | the mounting medium and depth |
| `chamber-height-um`, `background-per-sec`, `bf-n-*` | sample | the chamber, the medium and the cells |
| `fid-blocking-od` | instrument | a filter's blocking |
| `bf-condenser-na`, `bf-wavelength-nm` | instrument | the transmitted-light optics; the lamp's photons are a setting |
| `mode` | sample | the experiment's label mode |
| `z-range-um` | settings | a z stack's range, how one acquires |
| `focus-um` | measurement | the focus, as `z` |

## The light history: what has been done to the sample

`sim::LiveScope`'s state after frames is a function of the frames' specs and counts only (the world reset on a new
world, the drift walker, the light history's records, the sample's time, the noise counter). So the history is stored as
the live frames taken: runs `{n, lit, set}`, each `set` the changes from the run before (`null`: a key it no longer
has), **without the sample's keys**. Loading resets the live sample and replays the runs (`LiveScope::Advance` = the
state step of `Frames`, no rendering; one function, so the two cannot drift apart) with the current sample's keys put
in: the light that reached each place, when, applied to the loaded sample (a different dye re-reads the past light,
as the history allows). The same sample and release: the next live frame is the saved session's, bit for bit (checked
in C++, in the viewer and viewer -> cli).

- The viewer logs every live job (`L.log`, runs of one spec merged); a new world or a fresh sample (↺) starts it
  afresh. A Record with "continue the live sample" logs its chunks; a Record of a fresh sample does not touch it.
- Semantics in the file: a measurement with a history is "live, continued" (`LiveScope`); without one, "a fresh sample
  at `start-sec`" (`ScopeMovie`, as Record and Make movie).
- Micro-Manager's producer is not on `LiveScope` yet: MM cannot write or replay a history for now.

## Loading in the viewer

Quiet apply: `self.ISC_QUIET` holds the panel's couplings (a schema `apply` handler's values are dropped, the specimen
switch does not move the BFP), so loading a sample does not load its dye's light preset over the settings. Order: the
custom dye slots and the objective first, then the instrument, sample, settings and measurement keys (the specimen and
camera preset first, the pixel size last), twice, then the view. Keys: `x`/`y` the world view's centre, `seed` the
Seed (`world-seed` follows it), `qe-curve` the flat-QE tick, `p.*` the world controls, `mv_<key>` the rest, else
`ISC_MM_EXTRA`. The light history is replayed on the live worker (a `live-replay` job). After it the viewer compares
its own values with the file's and lists what it could not take ("Not taken") and keys it does not know.

## Back-end: `insiliscope_cli --settings` and ground truth

`--settings <file>[:part,part]`, repeatable, later files and options override; all parts but view by default. With a
light history the cli replays it (`settings::Replay`) and writes the live sample's next `frames` frames
(`LiveScope::Frames`); without one, the usual movie. Ground-truth export on top of this is a later task; what it will
meet:

- The render regimes decide what the GT means: binned blinks snap to a grid, mean-field blinks have no events, dim
  blinks under `blink-min-photons` are left out of the image, the halo cut trims kernels; continuous populations
  (WideField, PALM pre states, the free imager) have no single emitters. A GT must flag each event's regime and warn
  unless the quality is Exhaustive.
- It needs an event hook in `FluorescenceMovie` on both the movie and the `LiveScope` path (the events exist:
  `CollectFrameEmitters`, the core's stride 10), with the drift per frame, the dye position against its target (the
  linkage) and the detected photons per frame.
- Viewer vs native pixels: the GT positions are the same (address-based); images can differ where the viewer uses
  WebGPU (WideField mean field) and where the cytoplasm mesh is "near" across libms. Noise draws are counter-based.
- Frames run back to back (no dead time between frames): a frame's time is `T0 + f x exposure`.
- The release is pinned in `made`; another release warns.

## Later

Ground-truth export; MM `Hub.SaveSettingsTo` / `LoadSettingsFrom` (needs MM's live producer on `LiveScope` for the
history); an instrument's installed hardware (which lasers, filters, objectives exist) so a settings part can be
checked against it; usage counts for save/load (only if wanted).
