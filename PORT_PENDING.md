# Port pending: excitation geometry (epi -> HILO -> TIRF, the BFP slider)

Iteration mode (CLAUDE.md): the JS reference has it, the C++ does not yet. Plan: `claude/illum-geometry`
(2026-10-13). The JS: `web/prototype/scope/excitation.js` (header = the physics), consumers in `fluorescence.js`,
`render.js frameEmitters`, `widefield.js meanFieldImage`, `dye_library.js`, `scope_movie.js` (options,
`scopeBfpPosition`, `scopeExcitation`); checks `web/lab/excitation_check.mjs` (part of `web/lab/check.mjs`).

## What the port must do

1. `adapter/inSiliScope/Simulation/Excitation.{h,cpp}`: the C++ twin of `makeExcitation`, `jsm::` for every
   `Math.*` (only `Math.exp`; `sqrt` is exact), the same operand order. `lateral`, `depth`, `depthMean`,
   `at`, `column` (HILO: 256 midpoints over the crossed heights), `depthNm`, `regime`, `uniform`.
2. Options in `ScopeMovie.cpp` (and `OptionNames`): `illum-profile` (Flat/Gaussian/FlatTop, default FlatTop),
   `illum-fwhm` (100), `illum-bfp` (unset: the light preset's `bfpPosition`, with the mounted specimen's
   `illumination.bfpPosition` in its place), `chamber-height-um` default 5 -> 100; remove `illum-geometry` (the JS
   keeps it as an ignored option only until `web/index.html`'s Geometry select is replaced). `bf-n-medium` is the
   medium index of the critical angle too. None of them is a kinetic key (`IsLightKey`): shaping only.
3. `DyeLibrary`: `StatePhysics` gets the per-line excitation shares (`lineShares`); `LabelPhysics` the imager rate per
   um of lit column (`imagerPerPxPerUmPerSec`). The generator (`tools/gen_dye_library.mjs`) carries light presets'
   `bfpPosition` and specimens' `illumination` into `DyeLibraryData.inc` (struct fields).
4. `FluorescenceMovie`:
   - each (structure, state) group gets the excitation factor at (x, y, zNm) in the FOV frame (axis at (W-1)/2 px,
     z = zNm/1000 + focus-um), null when uniform; `CollectFrameEmitters` multiplies photons by it after the
     brightness (the CPU splat, `CollectGpuEmitters`, binned blinks), and so do the running images' unit splats and the
     partial windows of per-dye populations;
   - the mean-field scenes (`WidefieldScene`, the four `FlatIllumination` uses): per dye plane k a per-cell weight =
     F x lateral(cell centre, plane centre) x depthMean(plane), null = unlit plane (skip it); the WGSL/D3D11 path
     must get the per-plane weights (check whether dD is formed on the CPU);
   - the background: not uniform -> a per-pixel map = background x lateral(px, 0) + sum over DNA-PAINT labels of
     imagerPerPxPerUmPerSec x column(px, chamber) x exposure; uniform -> the old scalar (outputs unchanged);
     `info.imagerBackgroundPerPxPerFrame` is the map's mean; `info.excitation` {regime, bfp, uMax, fieldFactor,
     depthNm}.
   - The host's `illumField` (the adapter's 2D field) goes: the adapter uses the engine's excitation.
5. Rates stay at the peak (as the in-plane profile always did): the factor scales photons only, also for WideField
   bleaching and the dSTORM initial ON / PALM pre-state decay (follow-up: dose groups for the bleach rate).
6. Checks: `scope_parity.mjs` cases for Gaussian/FlatTop, HILO, TIRF, Clipped, DNA-PAINT nanorulers (default TIRF);
   a ctest `excitation` (the JS check's unit part in C++); re-baseline the cli references and the gallery (FlatTop is
   the default now; default DNA-PAINT is HILO, the nanorulers TIRF).
7. Then the adapter (Phase 3 of the plan: rows, history in z bands, render-ahead with a weighted `Advance`, MM
   config group `Illumination` Epi/HILO/TIRF), the viewer slider and the docs (Phase 4). Delete this file in the port.
