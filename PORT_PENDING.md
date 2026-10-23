# Port pending: fiducial markers (beads) on any sample

Iteration mode (CLAUDE.md): the JS reference has it, the C++ does not yet. Branch `claude/fiducials` (2026-10-22;
docs/dev/PLAN.md "Fiducial markers on any sample"). Data (committed, already in `DyeLibraryData.inc`):
`data/fiducials/` (types, spectra, gold Mie table; `DYE_DATA.fiducials`). The JS: `web/prototype/scope/fiducials.js`
(header = the model), consumers in `world.js` (`fiducials`, `fiducialsInRect`, `surfaceHeightAt`), `fluorescence.js`
(`fiducialBeads`, `splatFiducial`, `renderFiducials`), `scope_movie.js` (`fid-*` options, `scopeFiducialParams`,
`scopeFiducials`); checks `web/lab/fiducial_check.mjs` (part of `web/lab/check.mjs`).

Beads are **not** a labelled structure: no `ISC_STRUCT_*`, no label, no kinetics rows, no clock-map block. The core
places them (sites: centre, diameter); the engine owns their light, kernel and bleaching. Off by default (density 0):
every existing output stays as it is.

## What the port must do

1. Core `core/src/fiducials.{h,cpp}`: `FiducialField` = `fiducials.js` bit for bit: tiles of `FID_TILE_UM` 2, salt
   `0x46494455`, count `Pcg4d(...).a` on channel 0 through `poissonInverse` (cap 4096), candidate i on channel
   16 + 2 i (x, y, Box-Muller with `jsm::log`, `jsm::cos`; `d = d0 max(0.1, 1 + cv g)`), the Layer height on channel
   16 + 2 i + 1; heights Coverslip / OnTop / Layer (a Layer candidate below `surface + r` is dropped). Params
   `fidDensity` (0-100), `fidDiameterNm` (10-2000), `fidDiameterCvPct` (0-50), `fidHeight` (0-2), `fidLayerUm`
   (0-1000) in `params.cpp` kFields with the JS clamps; **`PackingFingerprint` skips `fid*`** (no cell moves).
   `World::SurfaceHeightAt(x, y)` (cells covering the point, inside the outline, max `SampleCytoMeshHeight`; 0 for
   the nanorulers) = `world.js surfaceHeightAt`. Beads exist on both specimens (not inside the origami early return).
2. ABI 15: `isc_fiducials_in_window(w, x0, y0, x1, y1, out, cap)` -> rows (tx, ty, i, x, y, z um, d nm), tile by
   tile (tx outer, ty inner), centre in [x0, x1) x [y0, y1); header history line; `ISC_WORLD_VERSION` unchanged (no
   cell moves). Golden: a `fiducials` case per height mode (`tests/parity/golden.mjs`, re-freeze with Node 24);
   `world_checks.fiducials` (determinism under query order, density, heights, size spread).
3. Engine `Simulation/Fiducials.{h,cpp}` (= `fiducials.js` from "the bead's image" on): `FiducialPhotophysics`
   (per component through `LightPath`: `StatePhysics` with eps 1e5 / QY 0.5, `bleachPerSec` = kEm / kEm(ref at the
   component's `exPeakNm`) x ln2 / halfSec; gold's scatter per laser line with `GoldScatteringUm2` and the blocking
   floor 10^-OD on T_dichroic x T_em), `BeadRatePerSec`, the bead kernel (`beadPlane`: `BEAD_CORE_UM` 1.0,
   `BEAD_FADE_UM` 0.4, `BEAD_SLAB_NM` 10, `BEAD_BIN_NM` 10, `BEAD_LATERAL_SUB` 4, the photons given back in the core
   in proportion to the point PSF; same loops and operand order), memoized per (kernel planes, interp, pixel size)
   and (plane, diameter bin); `SplatBead` = the core square |dx|, |dy| <= rc from the bead kernel (the movie's
   interpolation, Fft read as Cubic) + the four halo rects from the point kernel at its nearest sub-sample (`splatRect`,
   SplatRows' arithmetic). The generator must emit the fiducial types, components and the gold Mie table into
   `DyeLibraryData.inc` if it does not yet (check).
4. `FluorescenceMovie`: after the dye groups, `FiducialBeads` (sites in the query rect and z window, per component the
   beads' rate x excitation factor at the undrifted centre, components under `FID_MIN_PHOTONS` 1e-3 per frame left
   out, a dye group's kernel reused within `FID_KERNEL_REUSE_NM` 10 nm, else its own); per frame, after the
   continuous populations, `RenderFiducials`: no drift = the Taylor images (order D: the lowest with
   (L_max (N-1) exp / 2)^(D+1) / (D+1)! <= 1e-9, at most 6; image k = sum of beads at rate e^-L (t0 + tm) q (-L)^k,
   q = (1 - e^-L exp) / L; frame f adds fround(c_k) x image k, c_k = (f exp - tm)^k / k! built by c = c u / k);
   else (drift, or no order converges) one splat per bead per frame at meanPhotons. The Gaussian PSF: sigma^2 + r^2 / 5.
   `info.fiducials` (with `sec`). GPU path: the beads go into the `populationsOnly` CPU image. Timing phase
   `fl.fiducials`.
5. Options (`ScopeMovie.cpp`, `OptionNames`): `fid-type` (names), `fid-density`, `fid-diameter-nm` (-1 = the type's
   `refDiameterNm`, resolved before the world params),
   `fid-diameter-cv-pct`, `fid-height` (names), `fid-layer-um`, `fid-bleach-half-sec` (28800),
   `fid-bleach-ref-kwcm2` (1), `fid-dyes` (-1), `fid-blocking-od` (8); `p.fid*`. None changes the labels'
   kinetics; none is a kinetic key.
6. `tests/parity/scope_parity.mjs`: a bead movie per height mode (and one with drift, one shaped beam) C++ = JS.
7. Live (`LiveScope`, the adapter, phase 4 of the plan): the bead's H from the light history's dose at its centre
   (`LightHistory::DoseAt` x the component's rate) instead of the movie's t0 + f x exposure.

Then `bash tools/port_check.sh`, docs/spec (spec/ALGORITHM.md "Fiducial beads", spec/PORT.md section), delete this file.
