# Performance pass: live Micro-Manager speed (2026-10-07)

Goal: the live adapter as fast as the hardware allows, single-molecule imaging first. A frame should cost what its
physics costs, never what bookkeeping, waiting or rebuilding costs.

**Targets** (12-thread laptop, Basic config, 256 px):
- every mode reaches 1000 / exposure fps whenever its render time allows: 100 fps at 10 ms for DNA-PAINT, dSTORM and
  PALM; >= 50 fps at 512 px;
- frames arrive on the exposure's clock (jitter < 1 ms) while the render time per frame stays below the exposure;
- a snap takes its exposure plus one render;
- a stage move, focus change, laser or channel change shows within 1-2 frames;
- an objective or camera switch takes < 300 ms;
- the first image after loading a config takes < 1.5 s.

Measure with `tools/bench_live.py` (fps per Channel preset x exposure, `Test_LiveRenderMs`, worst frame gap, longest
prefetch vs budget, action latencies; `--json`) and `ISC_TIMING=1` (phase times on stderr). Report numbers from an idle
machine: Defender and other load inflated render times 2-3x during the 2026-10-07 runs.

## Baseline (2026-10-07, idle machine, before phase 0)

| Channel (256 px) | render / frame | fps at 5 / 10 / 20 / 50 ms | of which (ISC_TIMING, 20 ms) |
|---|---|---|---|
| ATTO655 DNA-PAINT | 25-32 ms | 40 / 36 / 32 / 16 | setup 11.5 ms, events query 8.9 ms, GPU splat |
| AF647 dSTORM | 39-156 ms | 28 / 20 / 12 / 6 | setup 31 ms, query 4.4 ms, ~65 ms not yet split |
| mEos3.2 PALM | 113-141 ms | 9 / 9 / 9 / 7 | setup 73 ms, query 5.8 ms, ~105 ms not yet split |
| mEGFP WideField | 76-113 ms | 10 / 13 / 12 / 13 | setup 258 ms (loaded machine) |
| BrightField | 5 ms | 65 / 64 / **32** / 16 | |

Latencies: first snap 3.7-4.0 s, snap at 20 ms 250 ms, Channel switch + snap 0.1-4.7 s, Objective switch 0.6-1.2 s,
30 um stage move + snap 0.5 s, 1 um focus + snap 126 ms.

## Phase 0: frame pacing (done, this commit)

The producer slept (exposure - render time) with Windows `Sleep()`, which rounds up to the 15.6 ms timer tick, and
the sequence thread polled `Sleep(1)` (also 15.6 ms) and then waited one more exposure from its own start: live ran
at 64 fps for 10 ms, 32 fps for 20 ms, whatever the render time. Now `LiveClock.h` `PreciseWaiter` (high-resolution
waitable timer, 0.1 ms) on an absolute schedule (frame k+1 one exposure after frame k was due; no catch-up burst), a
condition variable from producer to consumer, no second wait in live sequences, and a snap wakes the producer.
The BrightField prefetch is gone from the producer: one dye-column work item overran its 3-5 ms budget by 500 ms
and stalled frames. Result, BrightField: 99.9 fps at 10 ms, 50.1 at 20, 30.3 at 33, 20.0 at 50; snap at 20 ms 21 ms;
focus + snap 52 ms.

## Phase 1: a persistent live session (largest win, single-molecule)

Every live frame builds a spec, a new `FluorescenceMovie` and runs `Begin` (setup: light path integrals, labels,
camera, kernels, dye clocks, populations) before rendering one frame: 11-73 ms per frame in SR, 258 ms in WideField.
- `sim::LiveFluorescence` (ScopeMovie, with a JS twin only if the viewer uses it): built from a spec once, then
  `Frame(pose, focus, tSec, clock)` per frame. Three invalidation levels: settings (rebuild: the live config and
  light versions), pose/focus (re-query; the core's dye blocks are cached), time (advance only).
- Split `fl.setup` and `fl.total` into named phases first (`ISC_TIMING`), so the ~65-105 ms not yet attributed in
  dSTORM/PALM is known before it is optimised.
- The events query (4-9 ms per frame): query the next K frames' time window once per pose and slice it per frame
  (the query is address-based, so the slices equal per-frame queries; check).
- Continuous populations (dSTORM initial ON, PALM pre states, WideField): they only decay; keep the convolved image of
  a population at the current pose and scale it per frame instead of rebuilding it.
- Check: frames of the session = frames of today's per-frame path for the same state (bit-identical on the CPU),
  `adapter_pixel_hash` unchanged (the precomputed stack is the movie path), `scope_parity`.
- Expected: DNA-PAINT ~25 -> ~8 ms, dSTORM ~100 -> ~30 ms, PALM ~190 -> ~40 ms (to be confirmed by the phase split).

## Phase 2: render-ahead queue and frame-parallel rendering

A ring of up to N rendered frames ahead of the clock (N adaptive: up to 8 frames or ~150 ms ahead, whichever is less),
filled while the CPU/GPU would idle, delivered exactly on the exposure schedule:
- each queued frame carries its state stamp (settings and light versions, stage pose, focus or z-sequence position,
  acquisition epoch, frame index); any change of state flushes the queue, so a frame never shows old settings and a
  change shows after one render, as today;
- when the render time exceeds the exposure, render K frames at once on K workers (each frame is a pure function of
  its state and index: counter-based noise, the drift walker stepped ahead in order, events from the query slices),
  so throughput scales with idle cores instead of being one frame's render time;
- illumination history: a frame rendered ahead reads its dye clocks as the snapshot plus the light of the frames
  queued before it (exactly what taking them adds), and its light is counted only when it is taken (as now);
- a continuous stage move changes the pose every frame: the queue stays empty and frames render on demand;
- background prefetch (stage-move margins, the z column) runs only when the queue is full, on a low-priority thread,
  in work items small enough to be cancelled (replaces the in-loop prefetch).
- Check: fps = 1000 / exposure while N x render < the time ahead; worst gap < 1 ms + jitter; the z-sequence and stage
  checks of `tools/test_cellfield_stage.py`; a settings change never delivers an old frame.

## Phase 3: GPU for single-molecule frames

Today the D3D11 splat runs only for one blink group without continuous populations (DNA-PAINT); dSTORM and PALM
render on the CPU.
- Splat every blink group on the GPU (one kernel set per group), the continuous populations as a photon image (the
  WideField D3D11 path), then noise on the GPU; one upload of events per frame, one readback.
- Batch the frames of the render-ahead queue in one dispatch (as the precomputed stack already does).
- Check: GPU = CPU on >= 99.8 % of pixels (the existing criterion), CPU fallback on any failure.

## Phase 4: interactive latencies

- First image (3.7-4 s): split it (world, packing the FOV's blocks, PSF kernel, first frame) and start all of them at
  `Initialize()`. The PSF preload exists, so check that it covers the default channel's wavelength.
- Channel switch (up to 4.7 s): a new dye means a new emission wavelength and so a new PSF kernel (~0.8 s each, memo
  of 2). Raise the memo to the channels of the config, precompute the typical labels' kernels in the background after
  init, and keep only what the splat reads (the polyphase block sums, not the full 200 MB of planes).
- Objective switch (0.6-1.9 s): the same, per objective (NA, immersion).
- Stage move (0.5-4 s on a loaded machine): packing and dye blocks of the new region; prefetch along the motion
  direction (phase 2's background thread); the disk cache already keeps packed blocks.
- Kernel size: the default 7000 nm half-width kernel (841 x 841 x 71) is expensive to build, hold and splat; let
  `Renderer.Quality` choose it (Fast: 3000 nm), with `scope_parity` unchanged at Realistic.

## Phase 5: CPU per-frame costs

After phases 1-3, profile again and take what remains: the noise chain (a counter-based Poisson draw per pixel, SIMD),
photon-image and frame copies (`photonImg = photons`, the consumer's full-frame copy, `CropFullFrameIntoImg`), the
splat at large FOVs (memory-bound), thread-pool overheads at small FOVs. Bit-identical rules of the 2026-10-03/04
performance pass apply (no reassociation, no FMA, results consumed in serial order).

## Order and checks

Phase 0, then 1 (with its phase split), then 2, then 3, then 4 and 5 by measured size. Each step: `tools/bench_live.py`
before/after (idle machine, JSON kept), `adapter_pixel_hash` unchanged, `scope_parity`, `test_insiliscope.py`,
`test_cellfield_stage.py`, every run < 5 min. Live frames are not seeded-reproducible, so a live change is checked
against the same state rendered by today's path, not by hash.
