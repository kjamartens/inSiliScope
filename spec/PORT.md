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

All in `web/prototype/` (moved there from `web/` in M3, when the viewer switched to WASM; history imported from `C:\GitHub\websmlm\cell_field_sim\`). Line numbers drift; grep
the function names. The *why* of every algorithm is in [ALGORITHM.md](ALGORITHM.md) (the prototype's
README).

| File | What to port |
|---|---|
| `index.html` `pcg4d`, `hashUnit`, `hashStream` | Address-based RNG. Same primitive as demoCam's `Pcg4d` in `SMLMCounterRng.h`. |
| `index.html` `CH`, `rawCandidate`, `cellRadiusAt`, `envelopNucleus`, `cellOutlineLocal`, `nucleusSignedDistLocal` | Per-chunk cell candidate: position, ellipse + angular-harmonic outline, nucleus ellipsoid. |
| `index.html` `cytoHeightAt`, `buildCytoMesh`, `getCytoGeometry`, `sampleCytoMeshHeight`, `smoothCytoGrid` | Cytoplasm height field. Microtubules are clamped against the *smoothed mesh*, not the analytic function, so the mesh must be ported too. |
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
Cell: diameter 25-35, elongation (short/long) 0.5-1, `cellBlob 1.75`, height 3-6.
Nucleus: long axis 8-12, short/long 0.6-1, height 0.3-0.5 x long axis, offset 0.1, margin 1.5.
Cytoplasm: rim height 0.1-0.3, edge rise 0.1-0.5, mid height 1-2, mid distance 0.1-0.3 x cell radius, `nucMargin` 0.6. `cytoMaxSlope` (default 1 µm/µm, slider 0-2, 0 = off) caps how fast the cytoplasm may rise outside the nucleus dome, and `cytoDomeSlope` (default 3, slider 0-4, 0 = off) separately caps the dome flank over the nucleus (falls from `c.height` to mid height along a smoothstep of reach `1.5·(H-mid)/cytoDomeSlope`, ≥ `nucMargin`). Cytoplasm cap: the edge rise is `Hc(1-exp(-s·dEdge/Hc))` with `Hc = 2·cytoMidHeight` (saturating, so no linear pyramid), the rim→mid ramp is stretched to keep its peak slope ≤ s, and `envelopNucleus(c, margin, cytoSlopeRunout)` pushes the outline out so the nucleus is always ≥ dome reach + `2·mid·ln2/cytoMaxSlope` from the edge. The four `cyto*` per-cell fields are now sampled before `envelopNucleus`. `cytoSmoothPasses` (default 12, slider 0-12) is the number of 3x3 binomial smoothing passes `smoothCytoGrid` applies to the mesh; `cytoRings` (default 60, slider 4-60) is the mesh's radial ring count; `cytoTheta` (default 128, slider 32-128) is its angular sample count.
Packing: enabled, min gap 1.0, relax iterations 80 (slider to 150), step (damping) 0.55, rotation allowed.
Microtubules: density 0.9 /µm², start offset 0-0.3, start XY jitter 0, end offset 0.01-0.4,
end direction jitter 145 deg, wobble turn 0.8, wobble path x1.05, step length 0.05, path smoothing 1.5,
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
`Exp(offLifetimeSec)` and blink again. Per-blink brightness log-normal with CV `photonCV`, mean 1.
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
window do not all rebuild on the frame entering a new bin. Checked in `world_checks` (`KineticsStats`:
Exp means, geometric blink count, log-normal mean/CV, time order).

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
`General_StageInvertX/Y` (default off) rather than hard-coding a convention. **Drift** composes as an extra
offset on the FOV centre: `effectiveCentre = stage + drift`, using the existing `ComputeDriftOffsetPx`
result converted to µm (do not shift dye positions separately or you will double-apply it).

---

## 8. Camera integration

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
  7 um, 0 = no limit) of it are culled in the query, for every PSF model. With a vectorial model a
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
CellField only (other patterns log once and render SR). Code: `Simulation/WidefieldRender.{h,cpp}`,
`Fft2d`, `Illumination`; `cli/widefield_check.cpp` (ctest `widefield`).

* **Photophysics, physical units.** sigma = ln(10) 1000 eps / N_A (3.8235e-13 eps um^2), k_em = QY sigma
  Phi I, eta = (1 - sqrt(1 - (NA/n)^2)) / 2, surviving fraction exp(-D/B) with D the emitted-photon dose.
  Camera photons per frame: bleaching `nb eta B exp(-D0/B)(1 - exp(-dD/B))` (exact frame integral),
  persistent `np eta dD`. Defaults (eps 270000, QY 0.7, B 5000, Phi 1.6e9 photons/um^2/s) give t1/2 =
  30.0 s and ~1.8 photons/dye/50 ms frame. B = 0 never bleaches. QE is applied by the noise chain.
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
  z-blended PSF). Vectorial: the `PsfKernelCache` planes, each grid cell the sum of its (os/u)^2
  oversampled cells placed as `SplatPsfKernel` (Nearest) centres them; u must divide the oversampling.
  Gaussian: planes every 100 nm, sigma from `WidefieldGaussianSigmaUm` (TODO(human): defocus ignored for
  now). Radius R capped by `PSFParam_PsfKernelHalfWidthNm` (cli `wf-kernel-um`), kept across focus moves.
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
  per batch of frames; frames rendered in parallel between re-anchors. **Live**: `BleachField`,
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
* Limits: no drift.
