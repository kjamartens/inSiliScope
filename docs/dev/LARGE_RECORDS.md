# Large Records in the viewer: frames to disk, strips, a larger field (plan, 2026-11-20)

Three parts, in this order; each is useful alone. Status: A, B and C done (2026-11-20; spec/PORT.md 32). Added after: "save to a file as recorded" (Chrome, Edge), the
frames straight into a file the person chooses, past the browser's storage quota.

- **A. Frames to disk while rendering** (viewer only, no engine change): a Record's TIFF and ground-truth files
  are written to the browser's private disk storage (OPFS) as they are made, not held in the tab.
- **B. Strips** (engine + cli + viewer): a fresh-sample movie rendered as full-width horizontal strips, every frame of
  strip 1, then of strip 2, ...; memory ~ one strip's, the output = the whole-field movie (bit for bit where the
  model allows, below).
- **C. A larger recorded field** (viewer): the Record box takes a field size W x H, centred on the stage, up to sizes
  only strips can hold.

"Continue the live sample" Records get A; B and C are fresh-sample only (a live sample carries state frame to frame).

## Decisions (2026-11-20)

1. The beam scales with the recorded field (today's rule), and the Record box says so clearly before Start (the
   user's choice).
2. Strip renders of the FFT paths within rounding: accepted.
3. Disk route: OPFS while rendering in every browser; Save downloads Blobs of those files (disk-backed, never read
   into memory). No save picker: a download of a disk-backed Blob already never passes through memory.
4. "Write frames to disk" is a checkbox, on by default; strips automatic when the field does not fit.
5. Row order within a frame of a strip render's localizations.csv may differ: accepted.

The questions as they were put:

1. **The beam under a larger recorded field.** `illum-fwhm` is a percentage of the field (`fovSpanUm = max(W,H) x
   pixel`, `ScopeMovie.cpp:1385`), so a larger Record now gets a proportionally larger beam. (a) keep the live
   field's beam in um (a camera ROI does not change the laser: dimmer edges), or (b) scale it with the recorded
   field (today's rule). Also the beam's centre: the recorded field's centre = the live one's (both on the stage).
2. **Accuracy of strip renders.** Blinks, noise, background, beads and the slide can be bit for bit. The FFT paths
   (WideField mean field, BrightField, binned blinks) depend on the grid's extent (FFT size, height bands, bleach
   basis, BF taper): within rounding, not bit for bit. Accept that (checked to a tolerance), yes / no.
3. **Disk route.** Proposed: always OPFS while rendering (every browser), and at Save either a stream copy into a
   file picked with `showSaveFilePicker` (Chromium) or a download of the disk-backed OPFS file (Firefox, Safari).
   Costs twice the movie's size on disk for a moment. Alternative (Chromium only): pick the file at Record start
   and write straight into it; then the .tif vs .zip choice moves to the start.
4. **Checkbox or automatic.** Proposed: "Write frames to disk" ticked by itself above ~1 GB of frames (the user can
   untick); "Render in strips" automatic when the field does not fit the worker's memory, with an Expert strip
   height. Or plain checkboxes, off by default.
5. **Row order in localizations.csv of a strip render:** frame-major, within a frame strip by strip (the whole
   render's is group then the core's block order). Same rows, different order within a frame. OK?

## A. Frames to disk (viewer)

What is there: frames stream out of the engine in ~32 MB parts (`streamFrames`, index.html:4429), posted to the page
and kept as typed arrays (`frameStore`, live.js:925); `writeTiff16` (index.html:6721) builds the TIFF as a Blob of a
head (all IFDs first) + the parts at Save; ground-truth files are whole `Uint8Array`s held in the GT worker, then the
page, then deflated into the zip (`isc_js_gt_file`, scope_wasm.cpp:37; zip.js always returns one Blob). No OPFS, File
System Access or `navigator.storage` anywhere.

Steps:

1. **TIFF head up front.** Split `writeTiff16` into `tiffLayout(w, h, n, desc, pxUm, bigTiff)` -> `{head bytes,
   frameOffset(f)}` and the Blob assembly; the head is known before the first frame (n, size, description). The
   description is padded to a fixed length so a Record that stops early can rewrite the head in place (`images=` /
   `frames=` = the frames got, the IFD chain ended there, the file truncated after the last frame): same head size,
   no frame moves. `tests/web/tiff_unit.mjs`: the layout's bytes = `writeTiff16`'s byte for byte; a stopped one reads
   back (tifffile) with the right page count.
2. **A disk store** (`web/live/disk_store.js`, page-free): in the worker, `navigator.storage.getDirectory()` ->
   `isc-rec/<id>/movie.tif`, `createSyncAccessHandle()`, `write(bytes, {at})`, `truncate`, `flush`, `close`. The live
   worker's `streamFrames` writes each frame at `frameOffset(f)` instead of posting parts (posts only progress and
   the last frame for the view). The chunk path ("continue the live sample") writes the same way from `onRecChunk`'s
   data in the worker (`liveFrame`). A frame store with the old interface but async (`frame(i)` -> a slice of the
   OPFS `File` read on the page) keeps the player; an LRU of a few decoded frames for playback.
3. **Ground truth to disk.** `M.onGtFile` writes each file to `isc-rec/<id>/gt/<name>` (the module hands files over
   as made, `Files(sink, ...)`); the page gets names and sizes, not bytes.
4. **Save.** zip.js gets a sink: `zipTo(entries, write)` writing parts in order (Blob entries streamed by
   `blob.stream()`, CRC from the stream as now; small text entries deflated as now). Chromium: `showSaveFilePicker`
   at the Save click, `createWritable()`, the movie copied (or the zip streamed) into it. Elsewhere: the OPFS `File`
   (disk-backed) or a Blob composed of OPFS `File`s for the zip, downloaded by an object URL. The bytes = today's
   .tif / .zip byte for byte (checked).
5. **Space.** Before a Record: `navigator.storage.estimate()` against the movie (+ GT estimate) x 2 (scratch + the
   saved copy); refuse with the numbers when it does not fit. Scratch folders deleted after Save, on the player's
   close, on the next Record and at page load (stale ones from a closed tab).
6. **UI.** The Record box: "Write frames to disk" (decision 4); the info line says where the frames are. The memory
   message (live.js:968) becomes a disk message when ticked.

Checks: `tiff_unit.mjs` (layout, stopped head), `zip_unit.mjs` (`zipTo` = `zip` byte for byte),
`viewer_live_record.mjs` and `viewer_save.mjs` once more with the option on (the saved .tif / .zip = the in-memory
path's byte for byte; a cancelled Record's file has the frames got). All < 1 min added.

## B. Strips (engine)

The model is local: a dye's photophysics reads only the light at its own place, so a strip is exact once everything
defined over the whole field is taken from the whole field. The engine already renders row bands inside a frame bit
for bit (`SplatUnitInto`, `RenderPhotonImage` bands, `ApplyNoiseChain` rows); a strip is the same idea across a
whole movie.

**The interface.** Engine options `strip-y0`, `strip-rows` (0 = whole), internal like `frame0`: the spec stays the
whole sensor (`size`, `height`, `x`, `y`); the movie renders rows [y0, y0 + rows) with a working margin and hands out
only those rows (`onFrame(f, adu, y0, rows)`). A driver (cli, WASM) loops the strips. In `ScopeSetup`: the sensor
(`W`, `H`, origin, axis, `fovSpanUm`) and the render window (`y0`, `rows`, margin) apart; every `S.W`/`S.H` use is
audited into one or the other.

Whole-field values (computed once, or passed in):

| What | Where | How in a strip |
|---|---|---|
| beam width, axis, `exciteKey`, BFP map window | ScopeMovie.cpp:1385, 5505, 5510; BfpField.cpp:512 | from the sensor |
| imager map node grid (32-64 nodes over the sensor) | Excitation.cpp:1211 | nodes from the sensor, evaluated on the strip's rows |
| slide: base map, the FOV-anchored past grid, fringes | ScopeMovie.cpp:6321, 6400, 3705 | grids from the sensor, rows evaluated |
| light state: pose, field stop, the start-sec past, `lightKey` | LightHistory.cpp:128-193; ScopeMovie.cpp:5551 | from the sensor; the map rect: verify the map is rect-local, else the whole rect |
| clock at the FOV centre (blink mean field, population clock) | ScopeMovie.cpp:5606, 3251 | from the sensor |
| regime switches: population mean field (`nZ`, `nSlab`, `fovArea`), blink mean field, binned frames (`nOn` per frame) | ScopeMovie.cpp:3885, 5578, 3897 | a counting pre-pass over the strips (counts add up), passed to every strip |
| beads: the set (the halo reaches the whole camera), Taylor order from all beads | Fiducials.cpp:306, 731 | every strip gets the whole field's beads; drawn into its rows |
| camera maps: one `mt19937_64` over W x H, offset then gain then read noise | ScopeMovie.cpp:918 | the sequence run in full per strip, the strip's rows kept (bit for bit; ~3 x W x H x 2 draws per strip) |
| noise draws keyed by pixel `y*W + x` | SMLMNoise.cpp:140 | the index offset by `y0*W` (full-width strips: an offset only) |
| the TIFF description | ScopeMovie.cpp:6580 | the whole movie's |

Margins:

- **Dyes and blinks:** the strip's query = the whole movie's rect (FOV + 2 um + drift) ∩ (the strip's rows ± the
  kernel's reach + drift). Every pixel of the strip then gets the same events in the same order *if* the core lists
  a sub-rect's blocks in the same relative order as the whole rect's: **verify first** (B0).
- **WideField mean field:** scene over (strip ± `wf-kernel-um` + drift) ∩ (FOV ± 2 um + drift); own FFT size: within
  rounding.
- **BrightField:** its own grid with the usual margin + taper over the strip: within rounding (the image is static, so
  one per strip).
- **Binned blinks:** R cells of margin, own FFT: within rounding.
- **Ground truth's `gtOther`** (offset, `bg_centre_photons`): the margin rows rendered anyway.

Ground truth in strips: a blink or bead belongs to the strip holding its centre pixel row (no duplicates); its core
and on-camera photons clipped to the sensor, not the strip (`FootprintCore`, GroundTruth.cpp:709); a dye in dyes.csv
to the strip holding its position (`EndMovie`'s merge would otherwise add overlap dyes twice); x / y in sensor
coordinates (`+ y0`). frames.csv from the first strip only. Tables written per strip, then joined frame-major (decision
5) on disk.

Steps:

- **B0** (a day, decides the rest): the core's block order for a sub-rect; the light-history map's rect locality. A
  small native check: a 96 x 96 DNA-PAINT movie, the blinks of a sub-rect query = the whole query's restricted.
- **B1** camera maps and noise index for a window (ctest: a window's ADU = the whole frame's rows, SMLM paths).
- **B2** `ScopeSetup` sensor / window split, the whole-field table above, the counting pre-pass.
- **B3** ground truth ownership.
- **B4** cli `--strip-rows N` (auto from the memory budget with `0`): `TiffWriter` gets a planned layout
  (`Plan(n, w, h, desc)`, `WriteRows(f, y0, rows, px)`, `Finish`) whose bytes = the append writer's (ctest
  `tiff_writer`); `--estimate` reports the strip height.
- **B5** WASM: `isc_scope_movie` with the strip loop inside (`Module.onMovieFrame(f, rows, y0)`), the worker writing
  the rows at `frameOffset(f) + y0*W*2` (part A's store); the GT run alike.
- **B6** memory: `EstimateScopeMemory` with a window (rows + margins; the dye count over the strip's rect), and the
  strip height = the largest that fits the budget (viewer: the 4 GB heap less a reserve; native: `MemoryBudgetBytes`).

Checks: ctest `strips` (new, target < 30 s): 96 x 96 px, 3 strips and 1, per path -- DNA-PAINT, dSTORM, PALM, beads,
slide, SIM2D, drift: ADU bit for bit; WideField, BrightField, binned: photon images within a tolerance before noise
(`--photons-out`), reported; ground truth: the same rows (sorted). `viewer_save.mjs`: a strip Record's zip
reproduced by the cli with `--strip-rows`.

## C. A larger recorded field (viewer)

1. The Record box: "Field of view" W x H px (default = the live field, `mv_size`), centred on the stage; the strip
   height (Expert). The spec's `size` / `height` for the Record only (the engine's `height` exists already,
   ScopeMovie.cpp:191; the viewer has no `mv_height` yet).
2. The beam (decision 1): (a) means writing the live field's beam into the Record's spec in um (a new option
   `illum-fwhm-um`, or `illum-fwhm` rescaled to the recorded field), (b) nothing.
3. The world view draws the recorded field's outline while the box is open (`drawPlannedFov`, index.html:7438).
4. The cap: `kMaxFovPx` 4096 stays for whole renders; strip renders up to 16384 (a 16384^2 frame is 512 MB on disk;
   the camera maps' draws per strip grow with W x H: measure, maybe the maps once to disk instead).
5. The box shows the estimate before Start: frames on disk, the strip height, a time from the setup's dye count
   (large fields stay slow: WideField 2048 px > 10 min a frame natively).
6. Settings files: the measurement part records the field (`recordFrom`, settings_file.js:128); the cli reproduces
   it. MM: no row (Records are the viewer's); `tests/web/viewer_mm_cfg.mjs` coverage: the new spec keys go on
   `VIEWER_ONLY`.

Checks: `viewer_live_record.mjs`: a 2x-live-size Record (small: 256 px, 5 frames) has the size asked, is centred on
the stage (its centre crop = a live-size Record's frames for SMLM).

## Docs and bookkeeping (in the commits that change behaviour)

`docs/try-viewer.md` (Record), `docs/ground-truth.md` (row order, strips), `spec/ALGORITHM.md` "Large fields of view"
(strips, what is exact), `spec/PORT.md`, `CLAUDE.md`, `docs/privacy.md` if a Record option is counted, `docs/dev/PLAN.md`
checklist.
