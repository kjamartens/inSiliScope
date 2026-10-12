# Port pending: DNA-origami nanorulers, a second specimen

The JS reference (`web/prototype/scope/`, `web/lab/`) is ahead of the C++ core and engine. This file says what the port
(same branch, `claude/nanorulers`) must do; delete it when `bash tools/port_check.sh` passes again.
Plan: `C:\Users\kjamartens\.claude\plans\c-users-kjamartens-downloads-origami-be-glistening-lark.md` (approved 2026-10-12).

## What changed in the JS

- **Registry**: `data/specimens.json` has a second specimen `Nanorulers` (index 1) with one target `docking` (prefix
  `ruler`, structure 1); `data/dyes/library.json` `typicalLabels.docking` (ATTO 655 DNA-PAINT, 80 % = incorporation).
  `tools/gen_dye_library.mjs` already regenerated `DyeLibraryData.inc` (two specimens, two targets).
- **Designs**: `data/origami/designs.json` (3D-ready schema; v1: body cylinder, deformation wlc2d / rigid, roll
  uniform / fixed, ring spots; the rest validated as reserved). `tools/gen_origami_designs.mjs [--check]` writes
  `web/prototype/scope/origami_designs_data.js`; the port adds `core/src/origami_designs.inc` to it.
- **Model** (`web/prototype/scope/origami.js`, port to `core/src/origami.{h,cpp}` line for line, `jsm::` math):
  - Field: 2 um tiles; count = `poissonInverse(density x 4, unit(pcgA(seed ^ ORIGAMI_SALT, tx, ty, CH.COUNT)))`
    (cap 4096); candidate i = `pcg4d(seed ^ ORIGAMI_SALT, tx, ty, CAND0 + i)` -> x = (tx + unit(a)) x 2, y, prio = c,
    yaw = 2 pi unit(d); Matern-II thinning over the 3x3 tiles (`beats`: prio, then ty, tx, i); order: tx outer, ty
    inner, candidate order. `moleculesInRect` keeps molecules within reach = (L/2 + D/2 + linker) / 1000 of the rect.
  - Molecule hash `h1 = pcgA(seed ^ ORIGAMI_DYE_SALT, tx, ty, i)`; bending normals Box-Muller on
    `pcg4d(h1, ORIGAMI_K_BEND, i >> 1, 0)` (cos for even i, sin for odd); roll `pcgA(h1, ORIGAMI_K_LAND, 0, 0)`.
  - `bentShape`: n = max(8, ceil(L / segment)), dtheta = z sqrt(ds / Lp) (0 when rigid), exact arcs (`arc`, sinc
    form with the half == 0 case), cumulative nodes, "aligned" frame (phi = -atan2 of the chord, anchor = chord
    midpoint); `centerline`, `transform`, `shapeOutline`.
  - Sites (`moleculeSites`): spot k, ring position n < min(ring n, strandsPerSpot); LABEL / FLUOR draws as on
    microtubules (`DYE_CH`), linker in the outward half-ball (LINK_U = cos, LINK_PHI, LINK_R cbrt), straight frame
    (x, y, h), bent (X, Y), yaw + position; z = attach + D/2 (+ h with `origamiHeights`).
  - Params (core `Params`/`kFields`, JS `ORIGAMI_DEFAULTS`, `normalizeOrigamiParams`): `specimen`, `origamiDesign`,
    `origamiDensity`, `origamiMinSpacingNm`, `origamiBend`, `origamiLpNm` (1500), `origamiStrandsPerSpot`,
    `origamiAttachNm`, `origamiLinkerNm`, `origamiRoll`, `origamiRollDeg`, `origamiHeights`. `PackingFingerprint` must
    skip `origami*` (and the CellField block store is not opened for specimen 1).
- **World** (`world.js`): `params.specimen` picks the geometry; an origami world yields one dye block per molecule
  (`origamiDyeBlock`: cx, cy, mtIndex = tx, ty, i; k = spot, n = ring position; structure 1), no cells, an empty
  optical volume, `maxCellHeight` = attach + D + linker when a molecule reaches the rect, `dyeOrientation` refuses
  non-Free on structure 1. Labels: one per structure (now two); the schedules are unchanged.
- **Engine** (`scope_movie.js`, `fluorescence.js`): `specimen` 0/1 (names), the per-target label options generated
  from `STRUCTURES` (`targetLabelOptions`: `mt-*` texts unchanged, `ruler-*` new, right after `mode`), the `origami-*`
  options after `packing` (`scopeSpecimenParams`; `p.origami*` on top), `z` defaults to the strands' height for the
  nanorulers (`scopeDefaultZ`), only the mounted specimen's labels render (`S.mounted`: groups, populations, imager
  background, info.labels), `light-preset auto` and the camera preset's WideField test use the mounted targets.
- **Checks**: `web/lab/origami_check.mjs` (33 checks, ~1 s; the cross-check runs `tools/origami_reference/crosscheck.py`
  = `origami_bend.py` fed the same normals: same sites to 1e-14 nm), called from `web/lab/check.mjs` with two nanoruler
  imaging cases. CellField JS movies are bit-identical to main (5 specs, ADU sha256).

## The port must

1. Core: a `Specimen` interface (`core/src/specimen.h`), the cell field moved behind it unchanged (golden, all
   `world_checks`, `adapter_pixel_hash` identical), `core/src/origami.*` = `origami.js`, ABI 12 (`ISC_STRUCT_ORIGAMI_SITES
   1`, `ISC_STRUCT_COUNT 2`, `isc_origami_in_window` / `_outline` / `_sites`), `world_checks.Origami`, golden re-freeze
   (origami lines added), `label_parity` with specimen 1.
2. Engine/cli/WASM: the options above in `ScopeMovieOptions()` (same order), `MountedStructures`, the default z,
   `CellFieldSource` -> `SampleSource`, `--origami-json`, ctest `cli_origami`, `scope_parity` nanoruler case.
3. Viewer (specimen select, origami drawing, label groups per mounted target), the webSMLM block, MM (`SampleHolder`
   states, `Nanorulers` device), docs (Samples section), figures, gallery; see the plan.
