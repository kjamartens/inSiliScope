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
- New params (core `Params`, `isc_params_set`): `npcDensity` 6 (per um^2 of envelope; estimate between maeshima2006
  and dultz2010), `npcMinDistNm` 120 (NPC diameter, maeshima2006), `npcRadiusNm` 53.5 (webSMLM; thevathasan2019:
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
- ABI 12: `ISC_STRUCT_NPC 1`, `ISC_STRUCT_COUNT 2` (kinetics rows 1 + 6 x 2), `isc_cell_npcs`.

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

## The port's to-do (Phase B, the plan)

Core (npcs.cpp, params, world, dyes, capi, ABI 12) and tests (world_checks.Npcs, golden npc lines with Node 24,
label_parity with two labels, wasm_abi_smoke 12, scope_parity two-target cases); engine (ScopeMovie per-target options,
None, off targets, auto preset; `SimplePlan` and the D3D11 splat for up to 4 blink groups, `HostSplatsBlinks(gi, f)`,
every group's kernel preloaded); cli probes and `--geometry-json` pores; viewer (`web/index.html` label group per
target, NPC density, the 8-fold ring layer in `web/scene/core.js`, pack-key regex) and `embed_web_module.mjs`; block
(two labels, structure column, abiVersion 12); adapter (per-target rows from `TargetCount()`, `_Mode` Basic,
`Fluorophores.Mode` removed, NPC geometry rows, configs, property reference, `adapter_pixel_hash`, `bench_live.py`);
docs/spec (ALGORITHM, PORT new section, BRIGHTFIELD: no NPC contrast, MM_DEVICES section 4, physics pages + figures,
gallery, CLAUDE.md -- also its "Wanninger et al. 2023 CIR4MICS" is Theiss et al. 2023), PLAN.md; then delete this file.

Still to come in Phase A: the shared dye-to-structure linkage model (binder + linker, a `Legacy` preset = today's
numbers first) and its visualisation.

## Checks (Phase A)

`node web/lab/check.mjs` (includes `web/lab/npc_check.mjs`: count = round(density x area), spacing, on the membrane
with an outward normal, orthonormal frames, cap vs side counts, history independence; two-target imaging cases; pores
off = the old light path), `node web/lab/label_regression.mjs` (the microtubules == origin/main in every mode, with
and without labelled pores; pore labelling statistics and orientation). `engine_check` fails until the port.
