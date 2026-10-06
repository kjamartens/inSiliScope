# Porting the cell field simulation into the insiliscope core and the inSiliScope adapter

> **Keep this file up to date while it exists.** It is the port spec (originally the hand-off between
> webSMLM's `cell_field_sim/` and demoCam_SMLM_MM, both now in this repo). Whenever the prototype in
> `web/` changes in a way that affects anything described here (hash channels, defaults, geometry, dye
> model, function names), update this file in the same commit. Correct it when reality diverges from it
> (a decision changed, a section turned out wrong), and mark finished steps in section 11 (the overall
> milestones are tracked in [PLAN.md](../PLAN.md)). Delete the file only once the port is complete and
> [ALGORITHM.md](ALGORITHM.md) plus `CLAUDE.md` carry everything that is still true.
>
> **Changed by the insiliscope decisions (M0/M1), overriding the text below where they conflict:**
> the geometry lives in `core/` (C++17, no MMDevice/GPU/OS dependencies), not in
> `adapter/inSiliScope/Simulation/`; the adapter only calls core through its C ABI
> (`core/include/insiliscope/insiliscope.h`). Every transcendental in core goes through `isc::jsm`
> (V8's fdlibm), not `<cmath>`, which is what makes native/WASM/JS bit-identical (see
> [m0-feasibility.md](m0-feasibility.md)). Golden vectors live in [golden/](golden/), not
> `tools/cellfield_parity_check/`.

**Audience:** a coding agent working in this repo.
**Goal:** inSiliScope simulates a *field of cells* (cell body, nucleus, microtubules, fluorophores on the
microtubules) that is effectively infinite, plus a **dummy XY stage** so the field-of-view can be moved
over it, with **blinks generated from the actual dye positions** and rendered by the existing PSF/noise
pipeline.

Read `CLAUDE.md` first (property naming convention, the "two draws in one expression" RNG
gotcha, GPU/CPU render paths). Then read the JS source listed in section 1.

---

## 1. Source material (the JS prototype is the reference implementation)

Prototype changes are tried in the lab first (`web/lab/`, [README](../web/lab/README.md)) and ported here in the
PR to main, listed in `PORT_PENDING.md` until then. The imaging path (dyes, kinetics, PSF, render, camera, WideField)
has a JS twin too, `web/prototype/scope/` (2026-10-01), held equal to the C++ by `tests/parity/scope_parity.mjs`.
All in `web/prototype/` (moved there from `web/` in M3, when the viewer switched to WASM; history imported from `C:\GitHub\websmlm\cell_field_sim\`). Line numbers drift; grep
the function names. The *why* of every algorithm is in [ALGORITHM.md](ALGORITHM.md) (the prototype's
README).

| File | What to port |
|---|---|
| `index.html` `pcg4d`, `hashUnit`, `hashStream` | Address-based RNG. Same primitive as demoCam's `Pcg4d` in `SMLMCounterRng.h`. |
| `index.html` `CH`, `rawCandidate`, `cellRadiusAt`, `envelopNucleus`, `cellOutlineLocal`, `nucleusSignedDistLocal` | Per-chunk cell candidate: position, ellipse + angular-harmonic outline (+ the fractal tail, `cellTailAt`, 2026-10-01), nucleus ellipsoid. |
| `index.html` `cytoHeightAt`, `buildCytoHeightGrid` (+ `edt1d`, `cytoGridCrossings`, `cytoCrossFrac`), `sampleCytoHeightGrid`, `buildCytoMesh`, `getCytoGeometry`, `sampleCytoMeshHeight` | Cytoplasm height field: the raw profile relaxed on a Cartesian grid (screened Poisson, nucleus obstacle; was `smoothCytoGrid` on the polar mesh until 2026-10-01). Microtubules are clamped against the *relaxed* height, not the analytic function, so the grid must be ported too. |
| `index.html` `buildCandidateMap`, `relax`, `prune`, `interactionChunks` | Packing (cells are moved apart, never shrunk). **See the viewport-dependence trap in 4.3.** |
| `microtubules.js` `buildMicrotubulesForCell` and everything it calls (`mtGenerateOne`, `mtResolveCollisions`, `mtEnforceMinTurnRadius`, `mtClampIntoCytoplasm`, `buildMtDirectionTable`, ...) | Per-cell 3D microtubule centrelines (`{x,y,z}` µm, cell-local frame). |
| `microtubules.js` `buildMicrotubuleLabelPoints`, `MT_*` constants | Lattice site -> binder tip -> dye. Currently only a windowed debug preview in JS. |
| [ALGORITHM.md](ALGORITHM.md) (was the prototype's `README.md`) | The *why* behind every non-obvious decision (blobbiness area correction, z-as-fraction-of-local-ceiling, over/under-nucleus crossing, turn-radius enforcement). Read it before touching an algorithm; do not "simplify" what it says was fixed on purpose. |

The JS header of `microtubules.js` already states it is written to be reimplemented in C++ (plain scalar
math, named constants). The cell generator in `index.html` is the same in spirit.

---

## 2. Architecture

```
                    SharedStageState (process-wide, no MMDevice dependency)
   XYStage --writes--> x,y target + motion model      zPositionUm (existing)
        (MM::XYStage)                    |                         |
                                         v                         v
  CInSiliScopeCamera live loop / stack worker: per frame ->  stage(x,y) at frame time, z
                                         |
                                         v
      CellFieldSource::EventsForFrame(f, stageXY, fovSize, frameDur, params)
        |  cells (hash by chunk) -> microtubules (per cell, cached) -> dye blocks (per 1 um of MT, cached)
        |  block schedule = every dye's full blink lifetime, sorted by time, pure function of address
        v
   std::vector<sim::BlinkEvent>   (FOV-relative um, zNm, tStart/tEnd in frames)   <-- existing type
        |
        v
   RenderPhotonImage / CollectGpuEmitters / ApplyNoiseChain   (UNCHANGED)
```

Design rules that make this work:

1. **Everything is a pure function of `(seed, address)`.** Cells, microtubules, dyes and their blink
   schedules are never stored in a global array and never depend on draw order, viewport, or
   what was visited before. Caches exist for speed only and can be dropped at any time.
2. **The renderer stays untouched.** The new source emits ordinary `BlinkEvent`s already translated
   into FOV-relative coordinates. `RenderPhotonImage`, the GPU path, camera noise, drift, illumination
   all keep working.
3. **Geometry is C++ in `core/`, no MMDevice includes**, so it can be tested standalone and compiled to
   WASM for the viewer and webSMLM.

Files: `core/src/rng.h`, `cells.*`, `packing.*` (M0); `cytomesh.*`, `microtubules.*`, `dyes.*` (lattice
geometry, addressed labels), `world.*` (fixed-block packing, per-cell asset LRU, window queries) (M2);
schedules come with M3. Adapter side: a thin `CellFieldSource` (event query over the C
ABI) and `InSiliScopeXYStage.h/.cpp` in `adapter/inSiliScope/`; add them and core's sources to
`inSiliScope.vcxproj` and `.filters` (the adapter stays MSBuild, core is compiled into it).

---

## 3. RNG and addressing

* `hashUnit(seed, cx, cy, k)` = `((Pcg4d(seed, cx, cy, k).a >> 9) + 0.5) * 2^-23`. Identical to
  `CounterRng::Uniform()` in `SMLMCounterRng.h` (same constants). Add a free function
  `HashUnit(uint32_t, uint32_t, uint32_t, uint32_t)`.
* **Integer semantics.** JS does `seed|0`, `cx|0`, `k|0` (int32 wrap) then `Math.imul` (uint32 wrap).
  In C++ take each argument as `int64_t`/`double`, reduce mod 2^32, cast to `uint32_t`. `hashStream`
  computes `k = base * 4096 + ctr`; with `base` up to ~9.5e6 that exceeds 2^32 and **wraps**, so the
  C++ must wrap identically (do the multiply in `uint64_t`, then truncate to `uint32_t`). Negative chunk
  coordinates are normal.
* **Channels.** Copy `CH` (0..44) and the `MT_*` stream bases (`MT_STREAM_BASE = 1000`,
  `MT_RESAMPLE_SPACING = 100000`, label bases) exactly, or the geometry stops matching the JS reference.
  `NUC_LONG (30)` is deliberately retired; leave it unused.
* **Never write two draws from the same sequential stream in one expression.** C++ leaves operand
  evaluation order unspecified (demoCam already hit this with `CombinedShotAndReadNoise`). JS is
  left-to-right, so a line-for-line port can silently diverge. Draw into named locals in the JS order.
* Use `double` everywhere the JS does, and `float` exactly where the JS uses a `Float32Array` (the
  packing radius LUT and collision outline are Float32 in JS; making them double changes layouts).
* Use `isc::jsm::sin/cos/atan/atan2/exp/log/hypot`, never `<cmath>`, for anything the JS computes
  with `Math.*`. JS `Math.pow` is the host's `std::pow` (V8 >= 13 `--use-std-math-pow`; V8 12 used
  fdlibm), so it has no bit-exact counterpart; expect "near" wherever the JS uses it (cytoplasm mesh
  ring spacing) on a libm other than the one Node was built with.

**Golden vectors** ([golden/](golden/), `tests/parity/`). `tests/parity/js_reference.mjs` evals the
generator half of `web/prototype/index.html` under Node and dumps RNG addresses (negative `cx,cy`, wrapping `k`),
V8 math samples and 50 packed windows; `golden.mjs --freeze` stores that in `spec/golden/`, and ctest
`golden_vectors` requires native and WASM output to match it **bit for bit**. Achieved for RNG and
packing (M0). M2 extended them with `cells` cases (cytoplasm mesh, microtubules, lattice sites through
`buildMicrotubuleLabelPoints` with a constant rng): **bit-identical** natively on Linux/glibc against
a Node 24 freeze; WASM (musl `pow`) is `near`, max 1.1e-13 um. The compare accepts mesh/microtubule
lines within 1e-6 um (`--geomtol`); everything else must stay bit-exact. MSVC is expected to be
`near` there too (UCRT `pow`); a near-tie in the collision pass could in principle make it diverge.

---

## 4. Cells, nucleus, cytoplasm

### 4.1 Units and frames
Everything is µm and radians. Cell fields (`semiMajor`, `nucLong`, `height`, ...) are in the cell's
cached **local** frame (origin at the cell centre, before `packRot`). `localToWorld(cell, lx, ly)`
rotates by `packRot` and translates by `cell.x, cell.y`. Microtubules and dyes are generated in the local
frame and mapped to world at the end; `z` is height above the coverslip (0 = floor).

### 4.2 Default parameters (from `index.html`)
Put these in a `CellFieldParams` struct with these defaults; expose only a curated subset as MM
properties (section 9) and keep the rest as constants until asked.

Field: `chunkSize 26`, `jitter 0.8`, `density (occupancy) 0.33`.
Cell: diameter 25-35, elongation (short/long) 0.5-1, `cellBlob 1.75`, `cellRough 0.15` (fractal edge tail, x blob, 0 = off), `cellFractalDim 1.35` (its box-counting dimension, 1-2). The cell height is no longer drawn (2026-10-05): the dome top follows the nucleus.
Nucleus: long axis 8-12, short/long 0.6-1, height 0.2-0.3 x long axis (0.3-0.5 before 2026-10-05), offset 0.1, margin 0.5
(sides and top; 0.6 before), gap below `nucBaseMin/Max` 0.4-0.9; shape (2026-10-05, spec/ALGORITHM.md "Nucleus shape"):
`nucIrregMin/Max` 0.03-0.2, `nucBendMin/Max` 0-0.3, `nucSmooth` 2.5, `nucThickIrreg` 0.1, `nucAsym` 0.5,
`nucWidestMin/Max` 0.2-0.4.
Cytoplasm (defaults "Rounded" since 2026-10-03; were rim 0.1-0.3, mid 1-2, slope caps 1 and 3): rim height 0.2-0.5, edge rise 0.1-0.5, mid height 2-3.5, mid distance 0.1-0.3 x cell radius, `nucMargin` 0.6. `cytoMaxSlope` (default 2 µm/µm, slider 0-3, 0 = off) caps how fast the cytoplasm may rise outside the nucleus dome, and `cytoDomeSlope` (default 4, slider 0-6, 0 = off) separately caps the dome flank over the nucleus (falls from `c.height` to mid height along a smoothstep of reach `1.5·(H-mid)/cytoDomeSlope`, ≥ `nucMargin`). Cytoplasm cap: the edge rise is `Hc(1-exp(-s·dEdge/Hc))` with `Hc = 2·cytoMidHeight` (saturating, so no linear pyramid), the rim→mid ramp is stretched to keep its peak slope ≤ s, and `envelopNucleus(c, margin, cytoSlopeRunout)` pushes the outline out so the nucleus is always ≥ dome reach + `2·mid·ln2/cytoMaxSlope` from the edge. The four `cyto*` per-cell fields are now sampled before `envelopNucleus`. `cytoRelaxUm` (default 1 µm, slider 0-3, 0 = raw profile; replaced `cytoSmoothPasses` on 2026-10-01) is the screened-Poisson relaxation length of the height grid (0.25 µm, `CYTO_GRID_UM`; spec/ALGORITHM.md "Cytoplasm height: relaxation"); `cytoRings` (default 60, slider 4-60) is the drawn mesh's radial ring count; `cytoTheta` (default 256 since 2026-10-01, was 128; slider 32-512) is its angular sample count (and `isc_cell_outline`'s).
Packing: enabled, min gap 1.0, relax iterations 80 (slider to 150), step (damping) 0.55, rotation allowed.
Microtubules: density 0.9 /µm², start decay `mtStartDecayPct` 1.6 % and end decay `mtEndDecayPct` 20 % of the
equivalent diameter, direction concentration `mtDirKappa` 1.5 (these replaced start offset, start XY jitter, end offset
and end direction jitter on 2026-10-05), wobble turn 0.8, wobble path x1.05, step length 0.05, path smoothing 1.5,
min turn radius 0.15, min separation 0.05, max z slope 5, line width (draw only, skip).

### 4.3 The viewport-dependence trap (do not port this part as-is)
`draw()` builds the candidate map over "visible chunks + margin", runs `relax`/`prune` over *that
window*, and the README admits a cell near the window edge can resolve to a slightly different position
depending on how far the margin extends (the viewer now packs once over the view padded by 2 views each side and reuses that
window until the view leaves it, so positions depend on where the last repack happened). That is fine for a viewer and **wrong for a moving stage**:
panning the FOV away and back must reproduce identical cells.

Do this instead: pack on **fixed, absolutely aligned blocks** (e.g. 8x8 chunks). For a block, build
candidates over the block plus a margin of `interactionChunks(p) + 2`, run `relax`/`prune` (same
iteration counts), and *keep only the cells whose home chunk lies in the block core*. The result then
depends only on the block address. Cells near block borders can differ slightly from what the neighbour
block would have decided for the same pair; accept that (it is deterministic), and log it. Cache per
block. Provide a switch `packing=off` that uses raw jittered positions (exactly reproducible, some
overlaps) for a first milestone.

Keep the safety caps (`CHUNK_CAP`, `PRUNE_ROUNDS`); they exist because a pathological setting hangs.

**Implemented (M2, `core/src/world.*`):** blocks of `PACK_BLOCK_CHUNKS = 8`, margin
`InteractionChunks(p) + 2`, `enablePacking = 0` = raw candidates. Measured against one big 80x80-chunk
pack (compared on its central 48x48 chunks, 3 seeds): at the default occupancy 0.33, 88-94% of cells
are bit-identical, the rest moved by at most 2.2 um, and 0-12 of ~730 cells exist in one version only.
At occupancy 0.9 packing is globally chaotic (cells shift by up to ~55 um and ~30% differ in presence),
so there the block result shares little with any big-window pack; it is still deterministic.

### 4.4 Microtubules
Port `buildMicrotubulesForCell` faithfully, including the caches keyed by a signature of the cell shape
and parameters (`mtResultSig`, `mtCellShapeSig`). Key constants: `MT_MAX_PER_CELL 5000`,
`MT_COLLISION_MAX_TOTAL_POINTS 8000` (collision resolution is *skipped* above this; documented, not a
bug), `MT_NUDGE_ROUNDS 5`, `MT_RESAMPLE_ROUNDS 2`. Density is per µm² of the cell footprint.

Additionally store, per microtubule, what the dye stage needs and JS recomputes each time: cumulative
arc length and the **parallel-transport frame** `(T, U, V)` per segment (see
`buildMicrotubuleLabelPoints`). Parallel transport is sequential along the path, so this must be a
per-microtubule table built once, not evaluated per dye.

---

## 5. Fluorophore labels (dyes)

### 5.1 Geometry per lattice site (from `buildMicrotubuleLabelPoints`)
13 protofilaments (`MT_N_PROTOFILAMENTS`), 13_3 lattice: protofilament `k` sits at angle
`phase + k*2*pi/13` and axial offset `(k * 3 * 8 / 13) mod 8` nm, then every 8 nm (`MT_DIMER_NM`).
Surface attachment at radius `MT_RADIUS_NM = 12.5` nm from the axis; binder tip a further
`MT_BINDER_NM = 12` nm radially outward; dye displaced from the tip by a uniform-in-volume distance in
`[MT_LINKER_MIN_NM, MT_LINKER_MAX_NM] = [2, 5]` nm in a uniform direction (`mtDisplaceByLinker`,
originally `displaceByLinker` in webSMLM). Per-microtubule seam phase = `2*pi*hashUnit(seed,cx,cy,8000000+i)`.
That is about **1625 sites per µm of microtubule** at 100% labelling.

### 5.2 Do not materialise the sites
A 30 µm cell at the default density holds millions of sites. A FOV of 13x13 µm covers on the order of
800 µm of microtubule = ~1.3 M sites at 100% labelling. Rules:

* **Hash first, geometry later.** A dye's identity is
  `H1 = Pcg4d(seed ^ DYE_SALT, cx, cy, mtIndex)`, `H2 = Pcg4d(H1.a, k, n, purpose)` with `k` the
  protofilament and `n` the dimer index along the microtubule. Every per-dye draw (labelled?, first
  activation time, blink durations, brightness, linker displacement) is `HashUnit`-style off `H2` with a
  different `purpose`. Decide "labelled" and "ever activates before the horizon" from hashes only, and
  compute the 3D position (frame lookup, linker) **only for dyes that will emit**.
* **Blocks.** The unit of generation and caching is a *block*: one microtubule, 1 µm of arc length
  (~1625 sites). Blocks are addressed `(cx, cy, mtIndex, blockIndex)`.
* **Labelling efficiency** is one hash draw per site. Default the new property to something sparse
  (start at 5-10%; the webSMLM structures use 70% but their site counts are tiny).
* **Implemented (M2, `core/src/dyes.*`):** `DYE_SALT = 0x9E3779B9`, `H1 = Pcg4d(seed ^ DYE_SALT, cx, cy,
  mtIndex).a`, per site `Pcg4d(H1, k, n, purpose)` with purposes `LABEL 0` (its `.a` is also the dye
  `id`), `LINK_U 1`, `LINK_PHI 2`, `LINK_R 3`; seam phase on `8000000 + mtIndex` as the JS view. Site
  `(k, n)` sits at `off_k + 8n` nm from the microtubule start and belongs to block
  `floor((off_k + 8n) / 1000)`, so blocks partition the lattice exactly. Param `labelEfficiency`
  (default 0.1). Only 1 um blocks whose midpoint lies within 0.5 um + 29.5 nm of the window are
  decorated. Measured: a 12.8 um FOV beside a default nucleus holds ~390k labelled dyes at 10%
  (~3.9 M sites; every microtubule of the cell starts there), far more than the estimate above.
* The JS preview uses a sequential `hashStream` for labels, so **JS and C++ dye-for-dye positions will
  not match**. The C++ hashing above is normative; only the *statistics* (ring at 12.5 nm, tip at 24.5 nm,
  linker in range, lattice angles/stagger) must agree. If exact match is ever wanted, change the JS to
  the same `H1/H2` scheme and update this file.
* Dyes whose `|z - focus|` lies beyond the PSF kernel's z range must be **culled**, not clamped (the
  existing renderer clamps to the end plane, which would draw a cell's out-of-range dyes as bright
  in-focus-looking blobs). Cull in the query, count them, and log once.

---

## 6. Blinking from dye positions

### 6.1 Per-dye schedule = pure function of the dye's hash
For a labelled dye: first activation `tAct = -ln(U) / activationRatePerSec` (a parameter, section 9; was `* activationMeanSec` until ABI 3); then repeat:
ON for `Exp(onLifetimeSec)`, then bleach with probability `blinkBleachProb`, else dark for
`Exp(offLifetimeSec)` and blink again. Per-blink brightness log-normal with CV `photonCV` (default 0.5 since 2026-10-01), mean 1.
Cap blinks per dye (e.g. 1000) so a tiny `blinkBleachProb` cannot loop forever. Reuse the existing
properties `FluoParam_OnLifetimeSec`, `FluoParam_OffLifetimeSec`, `FluoParam_BlinkBleachProb`,
`FluoParam_PhotonCV`, `FluoParam_PhotonsPerSecond`. This is the same three-state model as
`EmitterModel`'s rich path, but with **persistent identity**: the same dye is the same dye every time
the stage returns.

The whole lifetime is finite (bleaching), so a block's schedule can be generated **once**, for all
time, as a list of `{tOnSec, tOffSec, brightness, dyePosLocal}` sorted by `tOn`, plus `maxOnSec` for the
block. No time windows, no replay. Schedule times are simulated seconds (`frameIndex *
frameDurationSec`, as drift already does), not wall-clock time.

**Implemented (M3, `core/src/dyes.*`, `world.*`):** `DyeSchedule(H1, k, n, Kinetics)`, draws
`Unit(Pcg4d(H1, k, n, ch).a)` with `ch = ACT (4)` for the first activation and
`SCHED0 (16) + 8*j + {ON 0, BRIGHT1 1, BRIGHT2 2, BLEACH 3, OFF 4}` for blink `j` (Box-Muller for the
log-normal; `bleachProb` clamped to [0.01, 1] as `EmitterModel`; at most `DYE_MAX_BLINKS = 1000`). The
schedule is keyed by the dye's full lattice address, not by its 32-bit `id` (ids of ~400k dyes in a FOV
collide). `Kinetics` lives on the `World` (`SetKinetics`, ABI `isc_world_set_kinetics`); changing it
keeps the geometry and dye positions cached and rebuilds only the schedules. Defaults: activation mean
100 s, ON 0.05 s, dark 1 s, bleach 1, CV 0. Dye blocks (world positions, then events by `tOn` with
`maxOn`) are cached in an LRU bounded by dye count (2 M by default; `isc_world_set_dye_cache`, ABI 4:
the adapter keeps 16 M, the whole z column of its FOV and a few um around it). Eviction runs at the end
of a query and never drops a block that query used, so a moved window builds only the blocks it did not
have. `isc_world_prefetch` (ABI 4) fills the caches for a region within a time budget; the adapter's
live mode spends the wait before each frame on the FOV's z column plus 3 um in x/y. Persistent sites'
blink ranges are extended ahead of time at a per-block point of their last bin, so the blocks of a
window do not all rebuild on the frame entering a new bin; a query's first build of a block reaches only
one bin ahead (a jump or a kinetics change builds what it shows now, the extensions the rest). Checked in
`world_checks` (`KineticsStats`: Exp means, geometric blink count, log-normal mean/CV, time order).

**Threads (2026-09-28, `core/src/parallel.*`):** the world's independent per-item work runs on
`ParallelFor` (up to 16 threads, made per call; serial in WASM, nested, or while another world's
`ParallelFor` runs): missing packing blocks, missing cells' assets (geometry + microtubules), and per
cell the new dye blocks, then their schedules and persistent covers. Each item is a pure function of its
address writing only its own block, and the query then reads the blocks serially in the old order, so
the events -- and their order, which the renderer sums in -- are those of one thread (`world_checks`
`Threads`: 8 threads = 1 thread, events byte for byte in order and the same build counts). With the
loop-invariant `exp(-m)` of the persistent-bin Poisson draw and the per-protofilament `cos/sin` and
linker `pow` hoisted (same operands, same bits): a 1000-frame 128 px stack query 2.1 -> 0.35 s,
a 40 um stage jump 5.2 -> 1.2 s (4 cores).

**Worker pool and parallel generation (2026-10-04, output bit-identical):** `ParallelFor` runs on a
process-wide pool of sleeping workers (`WorkerPool`; `AcquireWorkerPool` hands every `World` the one pool
through a static `weak_ptr`, joined when the last world is freed, so a host DLL can still unload; the
`World` query methods install it with a `PoolScope`, and without one `ParallelFor` spawns per call as
before). Parallel now, each item a pure function of its address consumed in the serial order: `Relax`'s
displacement loop (Jacobi: items read the iteration's snapshot of poses, `cos/sin(packRot)` and the 16
collision points computed once per cell per iteration instead of per pair), `BuildCandidateMap`, the
microtubules of a cell (one item per microtubule on its own hash stream; the collision pass stays serial),
the mesh node fill. Hoisted (same operands, same bits): the nucleus trig in `EnvelopNucleus`, the cytoplasm
mesh and the microtubule clamp, the outline tail factors (a table per `tailExp`), the dye pose per cell, the
log-normal parameters per schedule call. `CellAssets` builds the microtubules lazily (`isc_cell_mesh` asks
for the geometry only) and keeps per-microtubule 1 um block midpoints (`BlockMid`), which `ForEachDyeBlock`
scans instead of walking arcs; `CellsInRect` hands out pointers; `dyeIndex_` is a hash map.
`isc_core_bench` (native, 12 threads): packing block 88 -> 19 ms, cold cells 207 -> 78 ms, a cell's
microtubules 232 -> 19 ms, cold dyes 225 -> 48 ms, steady events 0.9 -> 0.1 ms per frame; `tools/bench_core.mjs`
(WASM, serial): packing 741 -> 408 ms, assets 538 -> 286 ms, cold dyes 110 -> 67 ms.

### 6.2 Query, per frame
```
std::vector<BlinkEvent> CellFieldSource::EventsForFrame(long f, StagePose pose, double fovWUm, double fovHUm,
                                                        double frameDurSec, const ...& params);
```
1. World rectangle = FOV centred on `pose` (x,y) expanded by a PSF margin (`>= 3 * kernel half-width`
   is plenty; ~2 µm).
2. Enumerate cells/blocks intersecting it (cell footprint bounding circle `rOuter` first, then per
   microtubule bounding box, then per 1 µm block centre). Generate/fetch cached blocks.
3. With `tNow = f * frameDurSec` (simulated time, 6.1), binary-search each block's sorted schedule for
   `tOn` in `(tNow - maxOnSec, tNow + frameDurSec)`; keep events with `tOff > tNow`.
4. For each hit emit a `BlinkEvent`:
   * `xUm = xWorld - (pose.x - fovW/2)`, `yUm = yWorld - (pose.y - fovH/2)`  (FOV top-left origin, as
     every existing pattern uses; the renderer then adds drift),
   * `zNm = (zWorld - focusHeightUm) * 1000`  (the Z stage still adds `globalZOffsetUm` in the renderer),
   * `tStart = f + (tOn - tNow)/frameDurSec`, `tEnd = f + (tOff - tNow)/frameDurSec`,
   * `brightness` from the schedule.
   Because the FOV moves between frames, **emit a fresh event per frame with that frame's translation**;
   never keep one event across frames. The renderer's frame-overlap weighting then still works because
   `tStart/tEnd` keep their original meaning.
5. Cull events outside the FOV + margin and beyond the kernel z range (section 5.2).

**Implemented (M3):** step 1-3 and the z/xy cull are core's `World::EventsInWindow(rect, zMin, zMax, t0,
t1)` / `isc_events_in_window` (stride 7: x, y, z, tOn, tOff, brightness, id; world um and simulated
seconds); step 4 (translation into a `BlinkEvent`) is the adapter's. `world_checks` (`EventQuery`):
equal to brute force over `SitesInWindow` + `DyeSchedule`, union of per-frame queries = one multi-frame
query, identical after a 1 mm excursion, a kinetics round trip, dropped caches and tiny caches.
Measured on a 12.8 um FOV beside a nucleus (+2 um margin, z 0.5-3 um, ~210k dyes, ~1950 blinks per
30 ms frame at activation mean 30 s): first query ~100 ms native, steady state ~3 ms/frame native,
~6 ms WASM.

Cost budget: a rendered frame should touch a few thousand blocks and emit hundreds of events. Profile
the first-visit block generation separately from the steady state; the steady state must not
regenerate anything. Cache blocks in an LRU (bounded, like `MT_RESULT_CACHE_MAX`), and generate the
blocks of the next FOV-edge ring ahead of need if first-visit latency shows up in live mode.

---

## 7. Dummy XY stage

### 7.1 Shared state (`Simulation/SharedStageState.h`)
Keep `std::atomic<double> zPositionUm` untouched. Add an XY motion model behind a `std::mutex`:
committed position, target, `moveStart` (`std::chrono::steady_clock`), `speedUmPerSec`, `settleSec`.
Provide, all MMDevice-free:

* `void SetXyTarget(double x, double y)` (starts a move from the *current interpolated* position),
* `void PositionXyAt(steady_clock::time_point now, double& x, double& y)` (constant-velocity linear
  interpolation, clamped at the target),
* `bool XyBusy(now)` (true until arrival + settle),
* `void SetXyOrigin`/limits as plain fields.

The camera samples `PositionXyAt(now)` **once per produced frame** (live) and uses the pose at that
frame. Motion blur during the exposure is ignored on purpose; note it in the docs.

### 7.2 MM device `XYStage`
Derive from `CXYStageBase<XYStage>` (`DeviceBase.h`). It is in `third_party/mmCoreAndDevices`.
**Implement every pure virtual it declares**: an abstract class fails at the `new XYStage()` in
`InSiliScopeModule.cpp`, not in the stage's own files (same trap already recorded for
`IsStageSequenceable`). At minimum: `Initialize/Shutdown/GetName`, `Busy` (return
`XyBusy(now)` so MM waits for the move), `SetPositionSteps/GetPositionSteps`, `SetPositionUm/GetPositionUm`
(the base provides the step-based defaults; overriding the Um versions directly and using a step size
of 0.1 µm is simplest), `SetRelativePositionUm`, `Home` (0,0), `Stop`, `SetOrigin`, `GetLimitsUm`,
`GetStepLimits`, `GetStepSizeXUm/YUm`, `IsXYStageSequenceable` (false). Register it in
`InitializeModuleData`/`CreateDevice` as `MM::XYStageDevice`, name `"XYStage"`, and add to the
vcxproj/filters.

Properties (group prefix rule from `CLAUDE.md`: no standard keyword applies, so use `General_`):
`General_StageSpeedUmPerSec` (default 5000, i.e. a few mm/s like a real stage), `General_StageSettleMs`
(default 20), `General_StageLimitUm` (default 1e6, the field is effectively unbounded). Add both stage
and camera through the Hardware Configuration Wizard; they communicate only through the singleton, as
the Z stage already does.

### 7.3 Coordinates
The stage position is the **world coordinate of the FOV centre** in µm. World `(0,0)` is the origin of the
cell-field chunk grid. Increasing X moves the FOV to the right over the sample, so features move *left* in
the image; same for Y downward. Real setups vary (camera mirroring/rotation), so add
`General_StageInvertX/Y` (default off) rather than hard-coding a convention. **Drift** (section 16) is applied
once: SR adds it to each event's position, WideField/BrightField shift the frame's image (live WideField moves the
FOV centre, `stage - drift`); never both.

---

## 8. Camera integration

*2026-10-05 (issue 16): the other patterns, `SimType_Pattern`, `EmitterModel` and the haze/out-of-focus
background are gone; the cell field is the only specimen. The text below is the original integration plan.*

* Add `PATTERN_CELL_FIELD = 15` to `SMLMPatternType` and a `SimType_Pattern` value `CellField`. It is
  **not** an `IPatternGenerator` (it does not sample sites per blink); dispatch on
  `CurrentPatternType()` in the two places that produce events:
  `StackGenerationWorker` (instead of `model.GenerateAllEvents`) and `LiveProducerLoop` (instead of
  `liveEmitterModel_.AdvanceOneFrame`). The live loop already reads the Z stage per frame; read the XY
  pose the same way and call `EventsForFrame`. The out-of-focus population (`Background_OutOfFocusRatio`)
  is site-list based; for `CellField` disable it with a one-line log, or later feed it from culled dyes.
* **Live/MDA is the intended mode.** A multi-position MDA moves the XY stage between snaps, which is the
  use case. **Precomputed-stack mode** generates all frames up front, so it can only use one stage pose:
  snapshot the pose at generation start, apply it to every frame, and document that moving the stage
  afterwards does not change the stack (the Z stage behaves the same way today).
* `BuildShapingFields` calls `EmitterModel::SampleSitesForHaze` for the cell-contrast/haze background.
  For `CellField` either return an empty site list (background falls back to flat) or sample the dyes in
  the initial FOV. Check that function before deciding; do not let it dereference a null pattern.
* Any property that changes the field must call `InvalidateStack()` (existing convention) so live mode
  rebuilds; the block/cell caches must key on a signature of the parameters, like `mtResultSig`.
* Set the focus plane: `CellField_FocusHeightUm`-style property (default ~1.5 µm above the coverslip,
  inside the nucleus/cytoplasm range). Cells are 3-6 µm tall, so `PSFParam_PsfZRangeUm` needs to be
  >= ~7 µm to cover them; warn (like the existing structure z-extent warning) if it is not.

---

## 9. MM properties

Follow the group-prefix rule in `CLAUDE.md` and update that file's bullet list. Suggested curated set,
all with the defaults in 4.2:

* `SimType_Pattern` gains `CellField`.
* `SimType_CellFieldChunkSizeUm`, `SimType_CellFieldOccupancy`, `SimType_CellFieldPacking` (On/Off),
  `SimType_CellFieldCellDiameterMinUm/MaxUm`, `SimType_CellFieldMicrotubuleDensityPerUm2`,
  `SimType_CellFieldFocusHeightUm`.
* `General_LabelingEfficiencyPct` (existing; reuse for the dyes, default for this pattern 5-10).
* `FluoParam_...` (existing kinetics) plus `SimType_CellFieldMilliActivationRatePerDyePerSec` (6.1, in
  1e-3/s) and `SimType_CellFieldLabelingPctNonBleaching` (6.3).
* Stage: `General_StageSpeedUmPerSec`, `General_StageSettleMs`, `General_StageInvertX/Y`.

Seed: reuse `SimType_RandomSeed`; derive the cell-field seed as its own XOR-constant stream (like
`structureSeed`), never the arrival/noise stream. Do not add UI beyond properties.

---

## 10. Verification

1. **Parity** (section 3): hashes, cells, one microtubule set against the JS dump.
2. **Determinism**: same seed, stage moved away 1 mm and back, frames identical. Also the
   same after dropping every cache.
3. **Dye statistics** on a straight synthetic microtubule: attachment radius 12.5 nm, tip 24.5 nm, dye
   within `[2,5]` nm of the tip, radial CDF of the linker displacement ~ r^3, per-protofilament angles
   are multiples of 2*pi/13 + phase, axial stagger 3*8/13 nm, count ~ `1625 * length_um * efficiency`.
4. **Kinetics**: blink-count distribution geometric with mean `1/blinkBleachProb`.
5. **Stage**: extend `tools/test_insiliscope.py` (pymmcore-plus): add the XY stage, set positions, assert
   `Busy` transitions, that a `speed` move takes ~`dist/speed`, that a known feature shifts by the
   expected pixels between two snaps, and that position-in-image flips with `StageInvertX`.
6. **Performance**: report first-visit block generation time, steady-state frame time at the default FOV
   (128x128 @ 100 nm), and memory after panning across a 1 mm path.
7. **Visual**: someone must look at it in Micro-Manager Studio; headless checks do not replace that
   (the repo's other features are all marked "not visually verified" for this reason).

---

## 11. Suggested order of work

Mark each done here.

1. [x] Hash + golden-vector tooling; port `rawCandidate`/outline/nucleus; parity test on cells.
       Done in M0/M1, plus packing (`relax`/`prune`/`packMap`), all bit-identical JS/native/WASM.
2. [x] Cytoplasm mesh + microtubules (unpacked cells first); parity test on one microtubule set.
       `core/src/cytomesh.*`, `microtubules.*`; golden `cells` cases m00-m04 (13 cells, 2306
       microtubules incl. collision nudges/resampling) bit-identical native, near 1e-13 WASM.
3. [x] Dyes: block generation, schedule, `EventsForFrame`; static FOV; renders through the existing
       pipeline. First visual check still open (headless only). (Block generation + window query done in M2: `dyes.*`,
       `world.*`, `isc_sites_in_window`; schedules, cached blocks and the event query in M3:
       `DyeSchedule`, `World::EventsInWindow`, `isc_events_in_window`. Adapter `EventsForFrame` open.)
4. [x] Shared XY state + `XYStage` + camera wiring; stage test in `test_insiliscope.py`.
       `SharedStageState` XY motion model, `XYStage.*`, `Simulation/CellFieldSource.*`
       (core C ABI -> `BlinkEvent`s), `CellField` pattern in `StackGenerationWorker` (one query over
       the stack's whole time span, one stage pose) and `LiveProducerLoop` (pose + event query per
       frame, simulated time advancing one frame duration per frame). Checks:
       `tools/test_cellfield_stage.py` (run by `test_insiliscope.py`), passing on the Linux test build
       (`tools/build_adapter_linux.sh`); **MSBuild not yet run** (no Windows in the M3 session).
5. [x] Fixed-block packing (4.3). `core/src/world.*`, measured in 4.3; ctest `world_checks`.
6. [ ] Properties, `CLAUDE.md` update, performance pass, docs. (Properties + `CLAUDE.md` done in
       M3; see "M3 deviations" below. Performance numbers of 10.6 only for the core so far.)

**M3 deviations from sections 7-9 (deliberate):**
* No `General_StageInvertX/Y`: every MM XY stage already has `TransposeMirrorX/Y` (`CXYStageBase`),
  which flips the direction exactly as asked; the test checks it.
* Labelling is `SimType_CellFieldLabelingPctBleaching` + `SimType_CellFieldLabelingPctNonBleaching`
  (defaults 0 and 70 since 2026-09-25; were 10 and 0), not `General_LabelingEfficiencyPct`. 70% of a
  cell field is ~2 M dyes in the adapter's query window: fine since the dye cache keeps a query's
  working set and persistent blinks are cached per bin range (2.4 ms/frame steady, no per-second
  stall; focus and xy moves come from the prefetched cache, section 6.1).
* Drift: the query rect is the FOV shifted *against* the drift (the renderer adds the drift to each
  event), and events stay relative to the undrifted FOV origin; so drift is applied once.
* z: `zNm = (z - SimType_CellFieldFocusHeightUm) * 1000` and the renderer's defocus is
  `zNm/1000 - zStage` (since 2026-09-25; it used to add the stage), so the in-focus world height is
  `focus + zStage`: the Z stage is the focal plane's height above the coverslip (focus offset default
  0, ZStage starts at 0.5 um); dyes beyond `SimType_CellFieldZRangeUm / 2` (default
  7 um, 0 = no limit) of it are culled in the query, for every PSF model. With a diffraction model a
  slab wider than `PSFParam_PsfZRangeUm` (or 0) is logged: those dyes get the kernel's end plane.
* Margin 2 um around the FOV (`kCellFieldMarginUm`); haze sites and the out-of-focus population are
  off for `CellField` (logged).
* Cell-field world seed = `RandomSeed ^ 0x43454C4C` ("CELL"); dye kinetics reuse
  `FluoParam_OnLifetimeSec/OffLifetimeSec/BlinkBleachProb/PhotonCV` plus
  `SimType_CellFieldMilliActivationRatePerDyePerSec` (1e-3/s, default 1.43 with 70% non-bleaching
  sites; was `ActivationRatePerDyePerSec` 0.01/s with 10% bleaching dyes, before that
  `ActivationMeanSec` 100 s).

### 6.3 Non-bleaching sites (ABI 3, 2026-09-25)
Two labelled fractions of the lattice sites, decided by the one LABEL draw `u` per site:
`u < labelEfficiency` is a bleaching dye (6.1), `labelEfficiency <= u < labelEfficiency +
labelNonBleaching` a persistent (DNA-PAINT-like) site, so the bleaching dyes never depend on the
second fraction. Both switch on at `activationRatePerSec` per dark dye. A persistent site never
bleaches: its blinks are a Poisson process of that rate for ever, addressed per 1 s time bin
(`PersistentBlinks`: count, start, ON time Exp(onSec) capped at 20 onSec, brightness from
`Pcg4d(Pcg4d(H1, k, n, PERSIST=5).a, bin, j, purpose)`), so any window is answered without running
from t = 0 and a window equals the union of its slices. Overlapping binding events on one site are
allowed (independent Poisson events; fine while rate x onSec << 1). `world_checks`: constant rate at
0-100 s and 10000 s, ON mean, window slicing, bleaching set unchanged; the adapter test shows
bleaching-only signal collapsing and non-bleaching flat over a 20 s stack.

### 6.4 Z-resolved density per population (ABI 5, 2026-09-27)
`isc_density3d_in_window(w, x0,y0,x1,y1, zMin,zMax, nx,ny,nz, populations, out)`: labelled-dye counts
on an nx x ny x nz grid, `out[(k*ny + iy)*nx + ix]`, plane k spanning `[zMin + k(zMax-zMin)/nz, ...)`,
x/y binned exactly as `isc_density_in_window` (so nz = 1 reproduces it). `populations` is a bitmask,
`ISC_POP_BLEACHING 1 | ISC_POP_PERSISTENT 2` (the `WorldDye::persistent` flag of 6.3). `World::
Density3dInWindow` bins straight from `ForEachDyeBlock` (no copy through `SitesInWindow`), so millions of
cached dyes become voxels with nothing crossing the ABI per dye. Infinite z limits only with nz = 1.
Used by the WideField modality (section 13). `world_checks` (`Density3d`): z-sum = 2D query, bleaching
+ persistent = all per voxel, equals hand-binning `SitesInWindow`, nz = 1 slab = 2D query.

### 6.5 Packed blocks across worlds (ABI 7, 2026-10-04)
`isc_world_pack_block(w, bx, by, out, capRows)` writes the cells of packing block `(bx, by)` (8 x 8 chunks,
`bx = floor(cx / 8)`) as `ISC_CELL_STRIDE` rows in `isc_cells_in_window`'s layout -- the whole block, cx-major
then cy, the count returned with the usual cap convention. `isc_world_set_block(w, bx, by, rows, n)` installs
such rows in another world of the same seed and parameters: a packed cell is `RawCandidate(seed, cx, cy, p)`
with `x, y, packRot` overwritten (`Relax` changes nothing else, `Prune` only `alive`), so the receiver rebuilds
each cell from its address and takes only the pose from the row. Returns 1 installed, 0 when the block was
already cached, -1 when the rows are not that block's (a chunk outside the block, an order not strictly
increasing in (cx, cy), a non-finite value, a candidate absent at this seed), leaving the world unchanged.
Installed blocks count in `WorldStats::blocksInjected` (not `blocksPacked`) and `DropCaches` drops them like
any cache. The viewer packs each block of its padded window as one job (`block` -> `isc_world_pack_block`) on
whichever worker comes next (the pack worker and the cell workers, nearest the view first, two in flight per
worker), keeps the rows and grows its cell map as they arrive; cell and dye jobs carry the rows of the blocks
they touch and their workers install them instead of packing, so a block is packed once per page, not once
per worker (`web/index.html` `iscEngine.inject`, `web/lab/engine.js` `World.setPackedBlock`; the `?nw` path
still packs its window in one `pack` job). `world_checks` `BlockInjection`: a world fed another's rows answers cells, mesh,
microtubules, dyes and events byte-identically with `blocksPacked == 0`, still after `DropCaches`; foreign
rows are rejected; the C ABI round trip is checked too (`wasm_abi_smoke.mjs` under Node).

### 6.6 Packed blocks across runs (ABI 8, 2026-10-04)
`isc_world_set_cache_dir(w, dir)` gives a world a disk store of its packed blocks (`core/src/blockstore.*`):
`dir/packed_blocks.bin` holds, per block, five doubles per cell (cx, cy, x, y, packRot -- all that Relax and
Prune produce; the shape is RawCandidate's), under a 64-bit key of `ISC_WORLD_VERSION`, the seed and
`PackingFingerprint(params)` (every parameter except the `mt*` and `label*` ones, which only shape what hangs
off a packed cell -- the viewer's pack key makes the same cut). A block the store has is installed through
the same validation as `isc_world_set_block` (counted in `WorldStats::blocksFromStore`, `blocksPacked`
untouched); a row that fails it is dropped and the block repacked. Blocks packed later are added and the file
rewritten whole (temp file + rename) every 16 new blocks, at `isc_world_flush_cache` and in the world's
destructor; FIFO cap `BLOCK_STORE_MAX` = 4096 blocks (a few MB), one file per directory, so a world of another
key overwrites it ("keep the last one"). `isc_world_version()` returns `ISC_WORLD_VERSION` (the C header), the
date `spec/golden` was last re-frozen: bump it with every change that moves a cell; `web/prototype/scope/world.js`
carries the same string as `WORLD_VERSION` and `engine_check` compares the two. Nothing under Emscripten: the
viewer keeps the same five numbers per cell in the browser's `localStorage` (one entry, its pack key plus the
module's `ISC_WORLD_VERSION`, at most 3000 blocks) and hands them to its pack job (`inject` before the query),
which also makes `engine_check`'s "pack with remembered blocks" case. Hosts (CellFieldSettings::cacheDir,
`Simulation/CacheDir.h`): cli `--disk-cache` 0/1/2 (default 1; 2 adds the opt-in PSF kernel file, see
PsfGeneratorBridge.h), MM `General_DiskCache` Off/Cells/CellsAndPsf. `world_checks` `BlockStoreTest`: a second
world of the same key takes every block from the file (same cells, nothing packed), mt*/label* changes keep the
key, a packing parameter changes it, a damaged file is ignored and rewritten.

### 6.7 Nucleus surface (ABI 9, 2026-10-05)
The nucleus is a shaped ellipsoid (spec/ALGORITHM.md "Nucleus shape"; `core/src/cells.cpp` `NucShapeInit` and the
`Nuc*` functions, bit-exact with the prototype's `nucShapeInit`/`nucMapLocal`/...). `isc_cell_nucleus_rings(w, cx, cy,
slices, pts, out, cap)` returns its drawing rings (horizontal sections, both poles included, cell-local xyz) for the
viewer's `cell` job, which draws them instead of its ellipsoid (the pack record keeps the plain ellipsoid's numbers for
the fallback). `isc_optical_volume_in_window` takes the asymmetric nucleus chord. `ISC_WORLD_VERSION` 2026-10-05.
MM properties (one indexed handler, `g_CellFieldCoreParam` names the core parameter each sets):
`SimType_CellFieldNucBaseMinUm/MaxUm`, `NucIrregMin/Max`, `NucBendMin/Max`, `NucSmooth`, `NucThickIrreg`, `NucAsym`,
`NucWidestMin/Max`, `MicrotubuleStartDecayPct`, `MicrotubuleEndDecayPct`, `MicrotubuleDirKappa` (the core's defaults,
the viewer's slider ranges). The cli/viewer set every core parameter by name (`p.nucIrregMin=...`).

## 12. Known gaps to keep in mind (not for the first pass)

* Motion blur during an exposure while the stage moves; per-frame stage jitter.
* Dyes beyond the kernel z range are culled, not added as diffuse background haze.
* Packing near block borders can differ slightly from what the neighbouring block would decide.
* Dyes of a microtubule lying on the coverslip can end up a few nm below z = 0 (binder + linker point
  down); the JS does the same. Not clamped.
* Only microtubules carry labels; nucleus/cytoplasm labels (lamin, mitochondria, NUP) do not exist yet
  in the JS either.
* The JS prototype and this port are not validated quantitatively against real SMLM data.

## 13. WideField imaging modality (2026-09-27)

`General_ImagingModality = WideField` (cli/viewer `modality=1`): every labelled dye emits at once,
Code: `Simulation/WidefieldRender.{h,cpp}`,
`Fft2d`, `Illumination`; `cli/widefield_check.cpp` (ctest `widefield`).

* **Photophysics, physical units.** sigma = ln(10) 1000 eps / N_A (3.8235e-13 eps um^2), k_em = QY sigma
  Phi I, eta = (1 - sqrt(1 - (NA/n)^2)) / 2, surviving fraction exp(-D/B) with D the emitted-photon dose.
  Camera photons per frame: bleaching `nb eta B exp(-D0/B)(1 - exp(-dD/B))` (exact frame integral),
  persistent `np eta dD`. Defaults (eps 270000, QY 0.7, B 5000, Phi 4e8 photons/um^2/s; 1.6e9 until 2026-10-01) give
  t1/2 = 120 s and ~0.45 photons/dye/50 ms frame. B = 0 never bleaches. QE is applied by the noise chain.
* **Illumination** (`IlluminationPattern`): anchored to the objective, peak 1; for now `SquareIllumination`
  over the FOV. WideField reads k_em from it and deposits dose over its whole support. SR still uses
  `FluoParam_IllumProfile`.
* **Dye grid (world-anchored).** Upscaled grid, pitch = pixel / upscale, cell i = world [i pitch, (i+1)
  pitch): the same cells from every stage pose (the dyes bin the same way; `BleachField` cells map one to
  one). It covers the FOV plus the part of a 2 um margin the pattern still excites (none for the square),
  plus one cell for the sub-cell shift; z planes of `zPlaneNm`, world-anchored; slab =
  `SimType_CellFieldZRangeUm` around the focus (0 = [-5, 50] um), applied per focus to the cached planes.
* **Dye tiles** (`WidefieldDyeTiles`, C1). 64 x 64-cell world tiles holding the whole [-5, 50] um column
  sparsely ((cell, count) per plane and population), filled from `isc_density3d_in_window` (histogram,
  then each population over the occupied planes) and assembled into any rect of that pitch; LRU beyond
  1.5 GB (256 MB WASM). Shared by the stack worker, the live loop and its prefetch worker; keyed by
  (pitch, plane, world version). A cold region still costs the core's world generation (~1.7 s for 26 um
  at the defaults; the live loop's `Prefetch` warms the neighbourhood during the frame slack).
* **PSF planes.** Dye planes go to the two neighbouring PSF planes with linear weights (= a linearly
  z-blended PSF). Diffraction: the `PsfKernelCache` planes, each grid cell the sum of its (os/u)^2
  oversampled cells placed as `SplatPsfKernel` (Nearest) centres them; u must divide the oversampling.
  Gaussian (`PsfModel = Gaussian`, cli `psf-model=0`): planes every 100 nm, sigma from
  `WidefieldGaussianSigmaUm` (TODO(human): defocus ignored for now). Radius R capped by `PSFParam_PsfKernelHalfWidthNm` (cli `wf-kernel-um`), kept across focus moves.
* **FFT** (`Fft2d.h`, A3/A4). Real 2D r2c/c2r, sizes 2^a 3^b 5^c (mixed-radix 4/2/3/5 Stockham, batched
  16 rows/columns at a time, rows by the half-length complex trick; every row and column independent, so
  thread-count independent). N per axis = the smallest such size (multiple of 8) with the wrapped part of
  the linear convolution (source + kernel + 4 cells of cloud-in-cell + 1 of shift) missing the FOV cells:
  320 instead of 512 at 256 px.
* **Spectra and caches** (A2, B1). Kernel spectra per PSF plane (computed once per PSF, rect and R);
  plane spectra per (channel, world plane, level), LRU beyond 1 GB (256 MB WASM). A channel is a
  population with its per-column weights: persistent (eta dD) and the bleach basis maps. A focus change
  re-pairs cached plane spectra with kernel spectra (one complex multiply-add per frequency and dye plane,
  rows in parallel, planes in order) and does one inverse FFT per channel: ~10 ms at 256 px instead of a
  full rebuild. `FocusSeries` makes the images of many foci from one cache fill (parallel over foci;
  identical bits to one scene per focus).
* **Focus bands** (B2). A PSF plane may be convolved on a 2x or 4x coarser grid: dyes cloud-in-cell
  binned, the coarse product embedded in the fine spectrum with the coarse cells' phase and the binning's
  transfer function divided out. Only if the kernel energy outside the coarse band plus the binning's
  alias energy (white source; sum_m sinc^4(pi(u+m)) = 1 - 2/3 sin^2(pi u)) is <= 1e-6 of the plane's
  (<= 0.1% rms of its light). Measured on a scalar-diffraction NA 1.4 PSF: even 3 um out of focus keeps
  0.4% (2x) / 1-5% (4x) there -- the defocused disk's sharp rim carries frequencies up to the NA cutoff --
  so with sharp-pupil PSFs (all current models) no plane goes coarse. The mechanism is tested with a
  loosened criterion (160 of 200 planes coarse, 0.24% rms total image error).
* **Sub-cell pose** (C2). The camera sits at a fractional cell offset of the world grid; the image is
  resampled there by a Fourier phase ramp (Nyquist bins: cosine), exact for band-limited images (pitch <=
  lambda / (4 NA), 117 nm at 660 nm / 1.4). A stage move within a cell only redoes the inverse FFTs (~25
  ms at 256 px), one that changes the rect reassembles the dyes from tiles and redoes the spectra. The
  illumination is anchored to the objective, so a whole-cell move lights different dyes: nothing but the
  tiles is reused then.
* **Images and frames** (A1). Per focus the scene holds one real image per channel (FOV cells, shifted);
  a frame is bin(max(0, P + sum_j a_j B_j)) plus the SR background (map x illumination field x fade), then
  `ApplyNoiseChain`. No FFT per frame (0.2 ms at 256 px).
* **Bleach basis** (D2, D3). With the stage and illumination fixed the weights evolve as wb_anchor
  exp(-t dD / B). One basis map per distinct frame dose dD (<= 8; the square has one: the old scalar path),
  else 12 Chebyshev maps wb_anchor T_j(2 dD/dDmax - 1) with coefficients from exp(-tau x). Every frame is
  checked against the basis (1e-5 of the peak weight) and re-anchors when it does not fit, so a frame is
  always the image of its own weights.
* **Stack** = fresh sample (frame f starts at dose f dD), reproducible, never touches the live map; Z read
  per batch of frames; frames rendered in parallel between re-anchors (since 2026-10-04 the cli/viewer
  movie too: each frame's bleach coefficients serially, then a batch of frames in parallel, the frame
  that re-anchors alone; with no bleaching dyes the weights of `Begin` are reused, no `exp` per frame; and
  the cli/viewer keep the scene, PSF and world across movies as BrightField does). **Live**: `BleachField`,
  world-anchored dose in sparse 256^2 tiles of the grid pitch, deposited over the pattern support after
  each frame; reset on a world change or a pitch change. The producer renders the frame (a weighted sum)
  and hands background, noise and publication to a finisher thread that overlaps the next frame's scene
  work (D4). During a stage move a worker builds the destination's scene (own `CellFieldSource`, shared
  tiles) and the live loop swaps it in on arrival (C3). Frames are never stale: each is the image of its
  own pose, focus and dose.
* **Z sequence** (F2, B4). The `ZStage` is sequenceable (list and linear): positions go to
  `SharedStageState`; while armed, every frame of a camera sequence acquisition takes the next position
  (the TTL a real camera sends), starting at 0 when the acquisition starts; frames rendered before it
  started are skipped; stopping the sequence returns the stage. Live WideField makes the images of all
  positions at once (`FocusSeries`) and adopts them frame by frame; a precomputed stack is (re)made for
  the armed sequence (frame f at position f mod n) when an acquisition starts with a different one. SR
  frames take the positions too.
* **GPU** (E). One source, `Simulation/WidefieldGpu.wgsl` (kernels: clear, scatter, radix-2 FFT in
  workgroup memory (n <= 2048), Hermitian split into fp16 plane spectra scaled by 1 / sum|values|, the
  re-pairing MAC, expand with the separable sub-cell phase, crop, and frames with the counter-based
  noise chain). The scene hands a host a `WidefieldGpuJob` (GPU mode: power-of-two FFTs, no coarse bands)
  whose plane keys are process-wide unique, so hosts keep plane spectra resident across focus jobs.
  Viewer: `web/wf_gpu.js` on WebGPU (the WASM movie in steps: `isc_wf_begin`/`_job`/getters/
  `_set_images`/`_movie`; any failure or a software adapter falls back to the CPU images). Adapter:
  `Simulation/WidefieldGpuD3D11` on Direct3D 11 with HLSL generated from the WGSL by naga
  (`tools/gen_wf_gpu.mjs` -> `WidefieldGpuHlsl.inc`); focus work in live and stack mode, stack frames
  with noise on the GPU; `Create()` rejects software adapters and self-checks against the CPU; a failure
  mid-stream falls back to the CPU (`General_GpuStatus` says which). fp16 spectra cost <= 3e-4 rms of the
  image (measured 2.7e-5 .. 2.5e-4). Checks: ctest `widefield` (the job format through a CPU reference
  host: identical to the scene's own), `tests/web/wf_gpu_check.mjs` and `viewer_wf_movie.mjs` (headless
  Chromium, SwiftShader), ctest `wf_gpu_d3d11` (Windows) / `tools/wine_wf_gpu_check.sh` (Wine + lavapipe
  + Microsoft's HLSL compiler).
* Limits: drift (section 16) renders on the CPU.

## 14. The PSF in the cli and the viewer (2026-10-01)

`ScopeMovie` (cli and viewer) renders with the adapter's default PSF, `GibsonLanniZernike`, computed by
`Simulation/ZernikePsf.cpp` (a C++ port of webSMLM's chirp-Z code, the same as the Java class; no JVM),
through the same memoized `ComputePsfKernelCache`, `RenderPhotonImage(..., &kernel, z)` (SR) and
`KernelWidefieldPsf` (WideField) as the adapter. Options mirror the `PSFParam_` properties and their
defaults: `psf-model` (0 Gaussian, 3 GibsonLanniZernike; 1/2 need the JVM and are refused),
`psf-zernike-preset` (index or name), `zern.<j>` (one coefficient, waves, replacing the preset's),
`psf-mask`, `psf-mask-modes`, `psf-mask-waist`, `psf-oversampling`, `psf-kernel-half-width-nm` (rounded
and grown by the shared `PsfKernelHalfWidthPx`), `psf-z-range-um`, `psf-z-step-um`, `psf-sample-index`,
`psf-working-distance-um`, `psf-sample-depth-nm`, `psf-interp`; `wavelength-nm`, `na`, `immersion-index`
as before. `ScopePsfRequest` builds the same `PsfGeneratorRequest` as the camera's
`BuildPsfGeneratorRequest`. The SR movie computes the kernel while the cell field is queried (native).
The viewer sends `psf-kernel-half-width-nm=3000` (kernel stack ~75 MB instead of ~400 MB of WASM
memory); a cli run reproduces a viewer movie with that option. Check: ctest `zernike_psf`.

## 15. BrightField imaging modality (2026-10-01, exploration branch)

Spec, model, quality table and the list of missing structures: [BRIGHTFIELD.md](BRIGHTFIELD.md).
- Core ABI 6: `isc_optical_volume_in_window(w, x0,y0,x1,y1, zMin,zMax, nx,ny,nz, sub, out[3*nx*ny*nz])`, channel-major
  cytoplasm / nucleus / microtubule volume fractions (`World::OpticalVolumeInWindow`: cached cell assets, the smoothed
  cytoplasm mesh as the body height field, the nucleus ellipsoid's chord clipped to it, microtubule centrelines
  deposited as tube volume). Read-only: no new hash draws, goldens unchanged. Rows run on `ParallelFor` per cell.
  `World::PrebuildAssets` is factored out of `ForEachDyeBlock` (unchanged behaviour).
- Engine `Simulation/BrightfieldRender.*`: `BrightfieldScene::Update(src, spec, worldVersion)` (slices, sources,
  pupil, exit spectra), `Image(focusUm)` (cached per focus); `UpdateFromPhase` is the test hook.
- cli/viewer: `modality` 2 / `BrightField`; `bf-quality`, `bf-sources`, `bf-upscale`, `bf-sub`, `bf-slice-um`,
  `bf-margin-um`, `bf-condenser-na`, `bf-wavelength-nm`, `bf-photons-per-px-per-sec`, `bf-aberrations`, `bf-n-medium`,
  `bf-n-cytoplasm`, `bf-n-nucleus`, `bf-n-microtubule`, `bf-absorption-per-um`. The viewer has a quality slider.
- Adapter: `General_ImagingModality` gains `BrightField` (the modality is an int now: 0/1/2), properties
  `General_BrightField{Quality,Sources,Upscaling,GeometrySamples,SliceUm,CondenserNa,WavelengthNm,PhotonsPerPxPerSec,
  Aberrations}` and `SimType_CellField{IndexMedium,IndexCytoplasm,IndexNucleus,IndexMicrotubule,AbsorptionPerUm}`;
  precomputed stacks (`RenderBrightfieldStack`, one image per distinct focus, z sequences) and live mode (scene per
  pose, image per focus). No GPU path, CellField only; drift: section 16.
- [ ] Visual check in Micro-Manager Studio; [ ] MSBuild of the DLL (only the Linux test `.so` was built);
  [ ] waveorder weak-phase comparison; [ ] GPU path; [ ] stage-move prefetch.

**Worker split in the viewer (2026-10-04, output bit-identical).** `BrightfieldScene::Image` is, per source, an
independent propagation and inverse FFT, then a sum over the sources in source order. `BrightfieldMovie`
(ScopeMovie.h) and the WASM exports `isc_bf_begin` / `isc_bf_info` / `isc_bf_phase` / `isc_bf_atten` /
`isc_bf_begin_phase` / `isc_bf_source_image` / `isc_bf_set_sources` / `isc_bf_movie` / `isc_bf_end` let the viewer
spread that work: its movie worker builds the world and the scene with the per-source propagation deferred
(`Update(..., deferSources)`) and hands out the phase screens (`Phase`, `Atten`, slices, zTop, objectZ); every cell
worker builds a scene from those screens alone (`UpdateFromPhase`, no world) and computes its share of the sources
(`SourceImageAt`, a round-robin split); the movie worker takes all sources' images and forms the image exactly as
`Image` does (`SetImageFromSources`: the same float sum in source order), then renders the frames. ctest
`brightfield` checks the three paths bit for bit; `tests/web/viewer_bf_movie.mjs` checks the split movie equals the
single-worker one (`?bfsplit=0`). Not with the lab's JS engine.

## 16. Labels, dyes and the light path (issue 16, 2026-10-05)

The JS reference is `web/prototype/scope/` (`dyes.js` labels and schedules, `world.js` queries, `spectra.js`,
`dye_library.js`, `fluorescence.js`, `widefield.js` `meanFieldImage`, `scope_movie.js`); the C++ matches it number for
number (`tests/parity/label_parity.mjs`, `tests/parity/scope_parity.mjs`). Physics: `docs/physics/dyes-and-light-path.md`
and `photophysics.md`; the why: ALGORITHM.md "Labels, dyes and the light path".

**Core, ABI 10** (`core/src/dyes.*`, `world.*`, `capi.cpp`). Replaces `isc_world_set_kinetics` and the
`labelEfficiency`/`labelNonBleaching` params (6.1, 6.3 are history for those).

- `isc_world_set_label(w, structure, const double* v, n)`, `ISC_STRUCT_MICROTUBULE` 0. Layout `ISC_LABEL_*` (16
  doubles): density, fluorescent fraction, mode (`ISC_MODE_DSTORM` 0, `PALM` 1, `DNA_PAINT` 2, `WIDEFIELD` 3),
  activation rate /s, on s, off s, bleach probability, photon CV, initial ON s, pre state (0/1), orientation mode
  (Free 0, Fixed 1, Random 2), polar deg, azimuth deg, wobble deg, motion (0 = static), off-target count. Returns -2
  (not implemented) for motion != 0 or off-target > 0, -1 for invalid values.
- Dye channels (`DYE_CH`): FLUOR 6, INIT_ON 7, AUX 8, ORIENT_U 9, ORIENT_PHI 10, MOTION 11 and OFFTARGET 12 reserved.
  Earlier channels unchanged; packing and cells unchanged (`ISC_WORLD_VERSION` stays).
- `isc_events_in_window`: stride 10 = x, y, z, tOn, tOff, brightness, id, structure, state, aux. States
  `ISC_STATE_BLINK` 0, `PRE` 1, `INITIAL_ON` 2, `ALWAYS_ON` 3 (events are blinks, state 0).
- `isc_continuous_in_window(w, x0,y0,x1,y1, zMin,zMax, tMin, out, cap)`: the continuous windows (PALM pre state until
  the first activation with an aux draw, the dSTORM initial ON `[0, Exp(initialOnSec))`, WideField `[0, inf)` with an
  aux draw) still open after `tMin`, stride 10, dye order.
- `isc_sites_in_window`: stride 5 (+ structure). `isc_density3d_in_window(..., structureMask)`.
- Schedules: DNA-PAINT = the persistent bins at `kon x c`; PALM = the blink schedule (+ the pre window); dSTORM = the
  initial ON window then the blink schedule shifted by it; only blinks of `[tLo, 2 tMax)` are kept (`Schedule(b, tLo,
  tMax)`); packed per block (`PackedDye`, `kn = k << 24 | n`).

**Engine** (`adapter/inSiliScope/Simulation/`): `Spectra.*` (spectra on 300-900 nm / 1 nm, cross section, photon flux,
collection efficiency, detection), `LightPath.*` (lasers, dichroic, filter, QE curve from the options or a preset),
`DyeLibrary.*` (dyes, modes, overrides, `MakeEffectiveDye`, `StatePhotophysics`, `LabelPhysics`: rates, the dSTORM
intensity scaling, PALM activation, the imager background; data from `DyeLibraryData.inc`, generated by
`tools/gen_dye_library.mjs`, never hand-edited), `ScopeMovie.*`: `ScopeMovieOptions` = the JS `SCOPE_OPTIONS`
(names lists, dye overrides `<prefix>-dye.<field>` / `dye<N>.<field>`), `FluorescenceMovie` = `fluorescence.js`:
groups per (structure, state) with their own kernel (`KernelWavelengthNm`, 2 nm; memo groups + 1), background x QE at
the filter centre + the imager offset, blinks per group, continuous populations mean-field (`WidefieldScene`,
`FlatIllumination` over the FOV + 2 x 2 um, unit dose) or per dye (running image), noise at QE 1. Host hooks for the
adapter: `FluorescenceFrameOptions` (z per frame, drift, illumination field, background fade, photons out instead of
ADU), `SimplePlan` (one blink group, no populations: the GPU splat), a `WidefieldAccelerator` for the mean-field scenes
(D3D11), `SetScopePsfRequestHook` (the JVM models), `ScopeLabelState` (readouts), `PrefetchScope`, and `DyeClock`
(Begin's optional per-region clock: the blinks and per-dye windows are queried per clock region over its bounding box,
keeping the dyes whose position has that clock; a mean-field population goes to the scene's weighted channel with each
grid column's frame-0 mean photons, frame f = that x exp(-lambda f exposure); without a clock: one region at
start-sec, the JS path bit for bit).

**cli / viewer / block**: the options above; viewer module ABI 10 (labels on sites jobs, sites stride 5, the WebGPU
mean-field path per scene, `movie-progress` from the WASM); webSMLM block abiVersion 10 (density-1 label, same
`buildWindow`).

**Adapter** (`ScopeProperties.cpp`; CLAUDE.md lists the properties): a `ScopeSpec` from the properties drives
`FluorescenceMovie` with the illumination history (`Simulation/IlluminationHistory.*`) as its `DyeClock`: seconds of
light per 0.25 um tile, a place never lit at 0; a live frame adds its exposure over its lit rect (FOV + 2.5 um) when it
is taken (a snap takes a frame started after the call, a sequence only frames started after it began: never one in
flight at the old pose or clocks), a stack its duration after rendering; live renders one movie frame per tick; `General_ImagingModality` =
Fluorescence | BrightField.

Checks: ctest `world_checks` (label determinism, FLUOR nesting, modes, cache under load, threads), `widefield`
(`MeanFieldVsPerDye`), `label_parity.mjs`, `scope_parity.mjs` (every mode, PALM pre state, several lasers, per dye and
mean field, a Gaussian-spectrum slot dye, `light-preset=auto`, start 0 and 60 s), `engine_check.mjs`,
`tools/test_insiliscope.py` + `test_cellfield_stage.py`, `adapter_pixel_hash.py` (new baselines).

## 17. Sample drift (2026-10-06)

Directed part (added the same day): `DriftSettings` `xySpeedNmPerSec`, `zSpeedNmPerSec` (signed), `xyAngleDeg` (< 0:
random per seed, pixel 2 of frame 0xFFFFFFFF), `angleWanderDeg`, `speedWanderPct`, `wanderTimeSec`; `DriftWalker` steps
the path: velocity at the frame's start (v_xy = V max(0, 1 + w s_xy) along theta0 + alpha phi, v_z = V_z max(0, 1 + w
s_z)), d += v dt + the random-walk step, then the unit-variance OU states (pixel 1 of the frame's stream; started from
the stationary distribution at the walker's first frame) update exactly with a = exp(-dt / tau). With speeds 0 the path
is bit for bit the random walk alone. MM `SimType_DriftXySpeedNmPerSec`, `SimType_DriftZSpeedNmPerSec`,
`SimType_DriftXyAngleDeg`, `SimType_DriftXyAngleWanderDeg`, `SimType_DriftSpeedWanderPct`, `SimType_DriftWanderTimeSec`
(the speeds everyday, the rest advanced); cli/viewer `drift-xy-speed-nm-per-sec`, `drift-z-speed-nm-per-sec`,
`drift-xy-angle-deg`, `drift-xy-angle-wander-deg`, `drift-speed-wander-pct`, `drift-wander-time-sec`. Live mode keeps one
`DriftWalker` (settings may change between frames; the wander state carries on); a Live/MDA sequence start asks the
producer for a drift restart, applied at the start of its next frame (drift 0), and the sequence skips frames rendered
before it (`liveDriftRestart_`), so a live sequence follows the stack's path. Checks: ctest `drift` (constant velocity
exact, uniform random direction, wander RMS and correlation time, z sign, random walk unchanged), `scope_parity` (SR
directed + wandering), `tools/test_cellfield_stage.py --drift` (its own run; stack: random walk in Fluorescence (an mEGFP WideField
label); live Fluorescence and BrightField sequences = the stack path), the block check.

A random walk per axis, xy and z set separately (Cnossen et al., Opt. Express 29, 27961 (2021); Ma et al., Sci. Adv. 10,
eadm7765 (2024); docs/physics/camera.md). Replaces the linear `SimType_DriftNmPerSec` (constant speed, random direction
per seed, SR only), which is removed with `ComputeDriftOffsetPx` / `DriftAngleForSeed`.

- `Simulation/Drift.{h,cpp}` (JS twin `web/prototype/scope/drift.js`): `d(0) = 0`, `d(f) = d(f-1) + sqrt(frameSec) x
  (sxy g0, sxy g1, sz g2)`, `g` = `CounterGauss` on `CounterRng(seed ^ 0x44524654 "DRFT", f)`, pixel 0, draws x, y, z in
  that order (`DriftStep`, `DriftTrajectory`). nm, camera axes, +z away from the coverslip; the focal plane is `focus -
  dz`. The JS uses the same pcg4d and Box-Muller: the trajectories agree to the last digit.
- Options: MM `SimType_DriftXyNmPerSqrtSec`, `SimType_DriftZNmPerSqrtSec` (0-1000, default 0, `InvalidateStack`); cli /
  viewer `drift-xy-nm-per-sqrt-sec`, `drift-z-nm-per-sqrt-sec`; the seed is `SimType_RandomSeed` / `seed`. Default 0:
  every output unchanged. `ScopeMovieInfo::driftNm` (x, y, z per frame); the cli writes `<out>.drift.csv`.
- SR: `RenderPhotonImage(..., d/px, ..., zStage - dz)` per frame; the event query is widened by the trajectory's range
  (`CellFieldQueryFor` with the drift bounds; `DriftWidenZCull` grows the z cull window).
- WideField / BrightField (stacks, cli, viewer): the frame is the scene's full-grid image spectrum x a phase ramp
  (`ApplyShiftRamp`, shared with WideField's sub-cell pose; `ShiftedCamera`: inverse, crop, max(0, .), bin). WideField:
  `WidefieldScene::SetKeepSpectra` keeps each channel's spectrum before the sub-cell shift in `WidefieldImages`,
  `WidefieldDriftFrames` (`FocusSeries` on the focus grid around each base focus, refreshed when the images change,
  `RenderShiftedImages`); the square and the dye grid margin grow by `ceil(max|dxy| / pixel) + 1` pixels. BrightField:
  `BrightfieldScene::FineSpectrum` (whole-grid intensity, mean of the sources in source order, real FFT),
  `BrightfieldDriftFrames`; the grid margin grows by the same amount. z: `DriftFocusGrid`, foci `kDriftFocusStepNm` =
  10 nm apart over the trajectory's z range, linear interpolation of the spectra.
- On issue 16's `FluorescenceMovie` (merged 2026-10-06): `MakeScopeSetup` computes the path and widens the query
  rect and z window (the focus stays); blinks and per-dye windows move per emitter (`SplatUnit`/`RenderPhotonImage`
  with d/px, focal plane `z - dz`; the running image restarts when the drift changes); a mean-field population keeps
  its scene's spectra (no accelerator, no deferred images) and `DriftMeanField` renders each frame from the two
  focus-grid scenes around dz (`WidefieldScene::Update` at `zRef + z - dz`, the slab follows, the radius kept unless it
  grows), interpolated and shifted (`RenderShiftedImages`); with a host clock the bleach basis is anchored at frame 0's
  weights. The adapter passes the drift as spec options (`AddDriftToSpec`) for stacks and, live, poses each frame's
  spec at `stage - d`, `z - dz`. JS: `fluorescence.js`, `widefield.js` `meanFieldImage(..., marginExtraUm).imagesAt`.
  The WideField modality and its `WidefieldDriftFrames` paths below are history (WideField is a label mode now).
- GPU: a drifting WideField sample renders on the CPU (keeps spectra; the adapter skips the D3D11 host, the viewer the
  WebGPU job); a drifting BrightField movie runs on one viewer worker (no source split).
- Live (adapter): the steps of frames since `liveDriftOriginFrame_` are summed (the stack's path; with drift off no
  steps are taken, and switching it on starts from zero there). SR as stacks. WideField and BrightField: the scene stays at
  an anchor pose (`stage - anchor`; rebuilt when the sample moved 1 um from it; illumination/margin + 1 um) and each
  frame is its image shifted by the rest (`WidefieldScene::RenderFrameShifted`; BrightField: its fine image, foci on
  demand via `BrightfieldDriftFrames::Ensure`); the WideField dose goes where the camera-fixed light sits over the moved
  sample. Moving the WideField pose every frame instead re-weighted every dye plane (326 ms vs 74 ms per 50 ms frame).
- webSMLM block: `CellField.driftTrajectory(seed, frames, frameSec, xyNmPerSqrtSec, zNmPerSqrtSec)` -> [{x, y, z}] nm,
  generated from `drift.js` + `rng.js` (no second copy).
- Checks: ctest `drift` (Var d(1 s) = sigma^2 at 10 and 100 ms frames, mean 0, axes uncorrelated; live sum = trajectory
  bit for bit; SR drifting frame = the movie posed at the drifted sample, 100% identical ADU), ctest `widefield`
  (shifted frame = the scene posed at -d, 5e-7; focus grid vs exact focus, 2e-5 rms), ctest `brightfield` (whole-cell
  shift = the posed scene to its margin taper, 2e-3 of the contrast; focus grid 2e-4 of the contrast; a sub-cell posed
  scene differs by ~10% of the contrast through its own geometry sampling, printed only), `scope_parity` (SR, WF, BF
  drift cases), `tests/block/check_cellfield_block.mjs`.

## 18. Micro-Manager hub and devices (2026-10-06)

The adapter's device layout, tiers and naming are in `spec/MM_DEVICES.md`; this section lists what changed in the
engine (C++ `Simulation/ScopeMovie.*` and its JS twin `web/prototype/scope/scope_movie.js`, so the cli, the viewer and
webSMLM have it too). Every new option's default keeps the earlier output.

- Labels: `specimen` (0 = CellField, `data/specimens.json`), `mode` (the experiment's label mode, for targets whose
  `<prefix>-mode` is -2 Global; -1 = each target's own), `<prefix>-dye` -1 = Typical (`data/dyes/library.json`
  `typicalLabels.<target>.<mode>`), `<prefix>-label-pct` -1 = the typical labelling. New dye Cy3B.
- Excitation (laser clean-up) filters: `ex-filter` (`light_path.json` `excitationFilters`: None, ideal +/-5 nm band
  passes, an ideal quad (`multiband`), FPbase curves of Chroma ZET and Semrock FF01 clean-ups, Custom with `ex-lo-nm` /
  `ex-hi-nm`); each laser line's intensity x the filter's T at the line; light presets carry an `excitationFilter`
  (None). Parity case: the quad clean-up.
- Light from the shutters: `light-epi`, `light-trans` (1 open, 0 closed, -1 from `modality`). Both open: the
  fluorescence photons (its noise chain at QE 1) + the lamp's photons x the camera QE at `bf-wavelength-nm`, then one
  noise chain (`RenderCombinedMovie`, JS `renderCombinedMovie`); none: dark frames. Parity cases: both, both + drift,
  dark.
- The EMCCD ignores `gain-std-pct` / `read-noise-std-pct` (the sCMOS per-pixel spreads), in `MakeScopeSetup`, JS
  `scopeSetup` and the adapter's `SnapshotParams`.
- The adapter: pixel size = `Camera.SensorPixelUm` / (objective x `EmissionPath.EmissionMagnification`, default 0.667),
  written into the spec's `pixel-nm` (the cli/viewer default stays 100); `Renderer.WriteScopeSpecTo` writes the spec
  of the next frame and `insiliscope_cli --spec <file>` reads it (a precomputed MM frame with the GPU off is
  reproduced bit for bit).
