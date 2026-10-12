# PORT_PENDING: nuclear pore complexes, a second cell-field target (issue 15; branch `claude/npc-target`)

While this file exists the branch is in iteration mode (CLAUDE.md): the JS prototype and lab are ahead of the C++, and
CI's `port-gate` fails. The port (C++ core + engine + cli/WASM/viewer/block/adapter + docs) makes the C++ match the JS
again, then deletes this file. Plan: `C:\Users\kjamartens\.claude\plans\i-want-to-create-luminous-wind.md` (local).

## What changed in the JS (the reference for the port)

Phase A touched, besides `web/prototype/` and `web/lab/`, the data and the files generated from it (needed to define
the target): `data/specimens.json` (target `npcs`, prefix `npc`, structure 1), `data/dyes/library.json`
(`typicalLabels.npcs`: dSTORM AF647 58 %, PALM mEos3.2 58 %, DNA-PAINT Cy3B 60 %, WideField mEGFP 100 %),
`data/dyes/light_path.json` (`multiTarget`), `data/references.json` (thevathasan2019, theiss2023, maeshima2006,
dultz2010, schlichthaerle2019, soeller2025, fruh2021, ries2012, nieves2019), `tools/gen_dye_library.mjs`
(`lightPathMultiTarget`) and its outputs (`DyeLibraryData.inc` -- harmless until the port: nothing calls `TargetCount()`
yet --, `dye_library_data.js`, `web/dye_library.js`, `docs/references.md`), `tests/parity/load_prototype.mjs`
(`GEN_EXPORTS`).

### 1. Pore placement (generator: `web/prototype/index.html`, after `nucleusRingsLocal`) -> new `core/src/npcs.{h,cpp}`

- `nucSurfaceLocal(c, piece, th, w)`: piece 0 side (w = alpha in [0, pi], zeta = -cos alpha, s = `NucSectionW` of
  that half; the unshaped nucleus by the ellipsoid formula of `NucleusRingsLocal` -- `NucMapLocal` leaves
  `nucCos/nucSin` unset there), 1 bottom cap / 2 top cap (`NucMapLocal(c, w, C, S, -1 / +1)`, w = s in [0, f]).
- `nucSurfaceMesh(c)`: NT 128 angles (vertex i at th = (i / NT) 2 pi, i = 0..NT), NA 64 side bands, NC 16 cap rings;
  bottom cap (if fBot > 0), side, top cap (if fTop > 0); triangles in that order, bands bottom to top, sectors by
  angle; a band whose inner/lower ring is one point (cap centre, or the side's pole when that half is not widened)
  is one apex triangle per sector `[apex, b_i, b_{i+1}]` (upper pole: `[b_i, a_{i+1}, a_i]`), else two triangles
  `[a_i, a_{i+1}, b_{i+1}]`, `[a_i, b_{i+1}, b_i]`; flat area 0.5 |cross| (sqrt only), running CDF, area = the last.
- `buildNpcsForCell(seed, cx, cy, c, p)`: want = floor(npcDensity x area + 0.5); candidate i = 0.. <
  want x 50 while fewer than want: q = pcg4d(seed ^ 0x4E504331, cx, cy, i) (all four words; `unit` = ((a >>> 9) +
  0.5) 2^-23); triangle = first with cdf > u0 x total (binary search); sq = sqrt(u1), b1 = 1 - sq, b2 = sq (1 - u2),
  b3 = sq u2; parameters: apex triangles th = th2 + (th3 - th2) (b3 / (b2 + b3)) (th2 if b2 + b3 = 0), w = b1 w1 +
  (b2 + b3) w2; else th, w = the barycentric sums (b1 v1 + b2 v2 + b3 v3, in that order); the point on the true
  surface; rejected when its squared distance to an accepted centre is < (npcMinDistNm / 1000)^2 (a grid of cell
  size dMin is speed only). Normal: central differences delta 1e-5, w clamped to [0, pi] / [0, f]; side and bottom
  cap dP/dth x dP/dw, top cap dP/dw x dP/dth; |n|^2 <= 1e-40: the triangle's face normal turned away from (nucOffX,
  nucOffY, nucZ). Frame: Duff et al. 2017 (sg = nz >= 0 ? 1 : -1, a = -1 / (sg + nz), b = nx ny a, f1 = (1 + sg nx^2 a,
  sg b, -sg nx), f2 = (b, sg + ny^2 a, -ny)), turned by phi = 2 pi u3: e1 = cos f1 + sin f2, e2 = cos f2 - sin f1.
  Pore = {x, y, z, n, e1, e2, cand = i, piece}, in acceptance (= candidate) order. Exact expressions: the JS.
- `npcAttachmentPoints(p)`: 64 x (a, b, c) nm, k = ring x 32 + corner x 4 + arc; ring 0 at c = -sep/2 (inside),
  corner th = corner pi / 4, arc phi in (-67.5, -22.5, 22.5, 67.5) deg, u = cos(phi) spread/2, v = sin(phi) spread/2,
  a = R ct + (u ct - v st), b = R st + (u st + v ct) (webSMLM's `buildNupAttachmentPoints`).
- New params (core `Params`, `isc_params_set`): `npcDensity` 12 (per um^2 of envelope; the user's choice 2026-10-12, above the
  measured 5-8.6 of maeshima2006 and dultz2010), `npcMinDistNm` 120 (NPC diameter, maeshima2006), `npcRadiusNm` 53.5 (webSMLM; thevathasan2019:
  53.7), `npcRingSepNm` 50, `npcCornerSpreadNm` 12. `PackingFingerprint` and the pack-key regexes skip `npc*` (no cell
  moves: `ISC_WORLD_VERSION` stays).

### 2. Pore dyes (`scope/dyes.js` -> `core/src/dyes.*`)

- `NPC_DYE_SALT` 0x4E504344, `npcH1 = pcg4d(seed ^ salt, cx, cy, 0).a` per cell; site (k = attachment point, n =
  candidate index): LABEL / FLUOR / LINK_U / LINK_PHI / LINK_R as on a microtubule; position = centre + (a e1 + b e2
  + c n) / 1000 + a linker uniform in direction, radius cbrt(min^3 + (max^3 - min^3) u) with [2, 5] nm (cubes as
  l*l*l, not pow).
- `dyeOrientationFixed(o, t, r)` and `dyeOrientationRandom` factored out of `dyeOrientation` (the microtubule path is
  the same arithmetic); pores: t = n, r = cos(corner pi/4) e1 + sin(...) e2.

### 3. World (`scope/world.js` -> `core/src/world.*`)

- `STRUCTURE_NPC` = 1; two labels; pores built lazily per cell (`npcs(a)`), dye blocks = the pores per cell-local
  1 um cube, ordered (iz, iy, ix), pores in candidate order; walked after the cell's microtubule blocks, only when the
  pore label's density and fluorescent fraction and npcDensity are > 0; cull: cube centre z +- rb, rectDist(world
  centre) > rb, cell: centreDist > reachXY + rb, rb = sqrt(3)/2 + hypot(R + spread/2, sep/2)/1000 + 0.005.
- Block key {cx, cy, structure, element, block} (element = mtIndex, 0 for pores); dye rows: mtIndex -1 and element 0
  for pores. `npcsOfCell(cx, cy)` = the new export `isc_cell_npcs` (stride 9: centre, n, e1; cell-local).
- ABI 13 (after the nanorulers' 12): `ISC_STRUCT_NPC 1`, `ISC_STRUCT_ORIGAMI_SITES 2`, `ISC_STRUCT_COUNT 3` (kinetics
  rows 1 + 6 x 3), `isc_cell_npcs`. **Ported** (core, world_checks.npcs, golden `npcs` cases, label_parity, wasm_abi_smoke).

### 4. Imaging (`scope/scope_movie.js`, `fluorescence.js`, `dye_library.js` -> `Simulation/ScopeMovie.*`, `DyeLibrary.*`)

- Per-target option rows generated from the registry (`targetOptions`): `<prefix>-dye` (default: the typical dye in
  the library's default mode: mt ATTO655, npc Cy3B), `-mode`, `-label-pct`, `-imager-nm`, `-orient*`, `-wobble-deg`,
  `-motion`; names per prefix; `npc-density` (world `npcDensity`).
- `DYE_CHOICES` gains `None` (appended). A target is off (`scopeTargetOn`) when its dye is None, its label-pct is 0
  or its `<prefix>-density` is 0: no dyes, no group, no imager background (`offLabel`).
- `light-preset` default 0 = auto: `autoLightPreset` combines the on targets' mode presets (one distinct: it; several:
  lasers per line max, filters where all agree, else `lightPathMultiTarget` + no clean-up filter). Pores off = the
  old default light path (lab check).
- The `mode` option is only the fallback for `<prefix>-mode = -2`; PSF preview: the first emitting target.

### 5. Label linkage (`scope/dyes.js` -> `core/src/dyes.*`; the user's request: the dye-to-structure distance of both targets)

- Three parts, one model for every structure: `linkedDye(b, a, attachNm, L, h1, k, n, r1, r2, r3, parts)`.
  **Binder** (the probe): rigid end = b + a (attachNm + binderNm) NM, **one product** as before (bit-identical: MT
  attachNm 12.5 + binder 12, pore 0 + 0); its **flexibility** (binderFlexNm > 0 only): + a uniform point in a ball,
  cos polar = 2 u - 1 with u = `DYE_CH.BIND_U` (13), azimuth 2 pi `BIND_PHI` (14), radius flex NM cbrt(`BIND_R` (15)).
  **Linker** (chemical, probe to dye): exactly as before (uniform direction, length uniform in volume in [min, max]);
  cubes `l*l*l` (the microtubule path used `pow(l, 3)`: equal at 2 and 5 nm, checked). **Dye**: the end point.
  `parts` (optional; drawing only): binding site, rigid end, binder end. All 16 DYE_CH channels are now taken.
- Axes: microtubule a = the protofilament's radial direction (ct U + st V), attachNm = 12.5; pore a = the corner's
  direction cos(c pi/4) e1 + sin(c pi/4) e2 (c = (k & 31) >> 2), base = the site, attachNm = 0.
- `LEGACY_LINKAGE` (by target id): microtubules {binder 12, flex 0, linker 2-5}, npcs {0, 0, 2-5} = the numbers before
  the model. A label without a linkage gets its structure's; `normLinkage` clamps (>= 0, max >= min). A linkage change
  redraws the dyes (like density). Cull reach: `mtDyeReach` = (12.5 + binder + flex + linkerMax) 1e-3 (same operation
  order as the old constant), `npcDyeReach` + (binder + flex + linkerMax) 1e-3.
- Core ABI 13: `ISC_LABEL_BINDER_NM` 16, `ISC_LABEL_BINDER_FLEX_NM` 17, `ISC_LABEL_LINKER_MIN_NM` 18,
  `ISC_LABEL_LINKER_MAX_NM` 19, `ISC_LABEL_COUNT` 20; a vector shorter than 20 (or NaN there) = the structure's legacy.
- Data: `library.json` `linkagePresets`, each with binder, flexibility and linker and its sources (notes): Legacy =
  structure default; antibodies from Fruh et al. 2021 Table 1 (binder = shell radius - 12.5 nm, flexibility = FWHM /
  sqrt 2, linker 0.5 nm); nanobody (Mikhaylova 2015's < 2.5 nm), nanobody + DNA-PAINT docking strand, SNAP/Halo (< 5 nm,
  Nieves 2019), FP (chromophore inside the barrel: no linker); the splits within the measured bounds are estimates.
  `gen_dye_library.mjs` emits them to the JS only; the port adds `kLinkagePresets` (+ its struct in `DyeLibrary.h`).
- Options per target: `<prefix>-binder` (preset, names), `-binder-nm`, `-binder-flex-nm`, `-linker-min-nm`,
  `-linker-max-nm` (-1 = the preset's; `scopeLinkage`). MM: `<Target>_Binder` (Advanced), the four numbers (Expert).
- Defaults (user, 2026-10-12): microtubules IgG2-PAINT, pores SNAP-Halo (`linkagePresets.defaults`); a label without a
  linkage (the core's default label, the ABI's NaN) keeps the structure's built-in numbers (Legacy). The port changes
  the default images: new cli reference TIFFs and `adapter_pixel_hash`.
- Visualisation: `web/lab/linkage.html` (binder / flexibility / linker / dye in their own colours: cross-sections, a
  pore corner close-up, histograms). The port: the viewer's `linkage` layer (the same parts at close zoom), a physics
  figure (`structures.md`), `--dyes-json` with each dye's binding site and binder end.
- Prototype canvas (viewer half of `web/prototype/index.html`; the port does the same in `web/scene/core.js`): pores in
  bordeaux (cytoplasmic ring light, nucleoplasmic dark), a "Show nuclear pores" toggle in the 3D view panel, drawn
  between the nucleus slices by height (`drawNucleus`: a slice hides what is below its plane when seen from above).

## The port's to-do (Phase B, the plan)

~~Core (npcs.cpp, params, world, dyes, capi, ABI 13) and tests (world_checks.npcs, golden npcs lines with Node 24,
label_parity with three structures, wasm_abi_smoke 13)~~ (done); ~~scope_parity two-target cases; engine (ScopeMovie
per-target options, None, off targets, auto preset)~~ (done); `SimplePlan` and the D3D11 splat for up to 4 blink groups,
`HostSplatsBlinks(gi, f)`, every group's kernel preloaded; cli probes and `--geometry-json` pores; ~~viewer
(`web/index.html` label group per target with None and the attachment rows, NPC group, combined auto light on a dye or
mode pick, a structure's data layers image it alone, pack-key regex, engine_check with pores and linkage) and
`embed_web_module.mjs`; block (three labels, structure column, npcDensity 0 by default, abiVersion 13)~~ (done; the
8-fold ring layer in `web/scene/core.js` and per-structure dye colours go with the visualisation branch); adapter (per-target rows from `TargetCount()`, `_Mode` Basic,
`Fluorophores.Mode` removed, NPC geometry rows, configs, property reference, `adapter_pixel_hash`, `bench_live.py`);
docs/spec (ALGORITHM, PORT new section, BRIGHTFIELD: no NPC contrast, MM_DEVICES section 4, physics pages + figures,
gallery, CLAUDE.md -- also its "Wanninger et al. 2023 CIR4MICS" is Theiss et al. 2023), PLAN.md; then delete this file.

## Checks (Phase A)

`node web/lab/check.mjs` (includes `web/lab/npc_check.mjs`: count = round(density x area), spacing, on the membrane
with an outward normal, orthonormal frames, cap vs side counts, history independence; two-target imaging cases; pores
off = the old light path), `node web/lab/label_regression.mjs` (the microtubules == origin/main in every mode, with
and without labelled pores; pore labelling statistics and orientation; the linkage: the same dyes under any preset,
the antibody shell radius, the cone's tilt distribution, the pores' binder offset). `engine_check` fails until the port.

---

**Merged in from `claude/nanorulers` (2026-10-12, its PORT_PENDING.md as of ed8cc2c).** Its core, engine/cli and
viewer/block steps are committed there (ABI 12, structure 1); in this branch the registry order makes the docking strands
structure 2 (`STRUCTURE_ORIGAMI` / `ISC_STRUCT_ORIGAMI_SITES` 2, the pores 1) and the NPC port takes ABI 13. Its open
items (MM `SampleHolder` states and the `Nanorulers` device, docs, figures, gallery) stay that branch's.

## Port pending: DNA-origami nanorulers, a second specimen

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
