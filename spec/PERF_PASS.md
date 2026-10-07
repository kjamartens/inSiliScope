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
prefetch vs budget, action latencies; `--json`) and its `--profile` (below), or `ISC_TIMING=1` (phase times on stderr).
Report numbers from an idle machine: Defender and other load inflated render times 2-3x during the 2026-10-07 runs
(`MsMpEng` at 6 of 12 cores while builds and checks write files).

## Profiling (done)

Every phase timed with `TimingLog` (`Simulation/Timing.h`) is also collected in memory when profiling is on
(`ISC_PROFILE=1` from process start, or Camera `Test_ProfileCollect`): count, total, mean and longest per phase name, any
thread. Camera `Test_ProfileWriteTo <file>` writes them as JSON and starts afresh (a file: MMCore property strings stop
at 1024 characters). Phases of a live frame, in order on the producer: `live.state`, `live.spec+clock`, `live.begin`
(the movie setup `fl.*`), `live.render` (`fl.frame.*` on the CPU, `gpu.collect` + `gpu.splat+noise`), `live.lamp`,
`live.noise`, `live.publish`; `live.frame` = all of them; then `live.prefetch`, `live.idle`, `live.zseq-wait`. The
camera side: `mm.wait-frame (sequence|snap)`, `mm.frame-age`, `mm.copy+history`, `mm.insert-image`, `mm.snap`; startup
`init.camera`, `init.preload (background)`, `psf.*`. `tools/bench_live.py --profile` records the profile of the load
and first image, every live case, every Channel switch and every action; `--history benchmarks.json --version vX` adds
the run to the release's benchmarks file, and `release.yml` job `mm-bench` (Windows runner, pymmcore 12.5.0.75.0)
runs it on the DLL just built. `tools/benchmarks_page.py` renders fps per version, the latest frame breakdown (stacked
bars, every phase in a collapsible table) and the latencies with their largest phases.

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

Done so far (2026-10-07, every output unchanged: 7 cli frames byte-identical, `adapter_pixel_hash`, ctest 29/29):
- the phase split found the "not yet split" time: `fl.dye-counts` (counting a continuous population's dyes in the FOV's
  z column, every frame: dSTORM 9, PALM 29, WideField 105-220 ms) and `fl.mean-field-weights` (a clock lookup and two
  `exp` per mean-field grid column: 8-18 ms); DNA-PAINT has no continuous population, hence its smaller setup;
- the dye counts are memoised in the shared `MovieCache` by (box, structure mask) at the dye version (a count is a
  pure function of the world, the labels and the box);
- the column weights reuse the value of the previous column's clock (the 0.25 um history tiles span ~2.5 grid columns);
- a single-frame movie splats in bands of rows on all cores (`RenderExtras::parallel`, bit-identical to serial);
- `FluorescenceMovie::Render` no longer builds the camera's three noise maps when the host adds the noise (`onPhotons`);
- `ParallelFor` uses a persistent pool (an empty call: 1.2 -> 0.02 ms; the noise of a 256 px frame 3.0 -> 1.7 ms).

Measured (256 px, 10 ms, loaded laptop, `bench_live.py --profile`): WideField 3.4 -> 50-80 fps, dSTORM 18 -> 25-40,
PALM 5.5 -> 17-30, DNA-PAINT ~22-30 (GPU splat 22-27 ms per single frame on the Iris Xe; the CPU splat of its many
emitters takes 39-44 ms even on all cores).

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

Done (2026-10-07, `adapter/inSiliScope/LiveAhead.cpp`, every output unchanged: 7 cli frames byte-identical,
`adapter_pixel_hash`, `test_insiliscope.py`, `test_cellfield_stage.py` incl. `--drift`):
- In a sequence acquisition whose state (settings, light, pose, focus, exposure, acquisition) equals the previous
  slot's, a helper thread renders the next K = clamp(150 ms / exposure, 2, 8) frames as one `FluorescenceMovie` of K
  frames (one setup, one events query; the GPU splat as one dispatch); the producer hands them out one per slot. Each
  batch frame is labelled with its slot (the live frame counter); its dye clocks are the history snapshot advanced by
  the frames before it not yet taken (`ClockSnapshot::Advance`), its background fade and noise counter its own.
- Not with the lamp (BrightField), drift, a shaped illumination profile, a snap, or when stale frames are skipped;
  `ISC_RENDER_AHEAD=0` turns it off.
- Any change of state drops the queue and cancels the batch; while the producer waits for a batch it checks the state
  every 2 ms, so a stage move or setting change does not wait for the batch (`live.ahead-flush`).
- The producer and the helper share the GPU hosts (`LiveGpu`, kept across live starts: a D3D11 device, its shader
  compile and self-check took 2-3 s per thread) and the movie cache; whoever renders holds them. A frame on demand
  waits for the batch in flight rather than switching the shared mean-field scenes to their CPU mode (that rebuilt
  them: 1-2.5 s per switch, the first version's stalls).
- Batches slower than the exposure pace the slots (frames spread over a batch's time, not a burst after it); batches
  slower per frame than a frame on demand turn render-ahead off until the next setting change.
- Fewer frames than cores (a batch, a live frame): the blinks of each frame splat in bands of rows on all cores, one
  frame after the other, instead of a frame per core (bit-identical; 8 frames used 8 of 12 cores).
- Measured (256 px, loaded 12-thread laptop, `bench_live.py --profile`, render-ahead on vs `ISC_RENDER_AHEAD=0`, 10 /
  20 ms exposure): dSTORM 67 / 29 vs 36 / 24 fps, DNA-PAINT 32 / 27 vs 27 / 12, PALM 28 / 37 vs 22 / 28, WideField
  100 / 45 vs 78 / 49 (the 20 ms WideField pair within this machine's noise), BrightField unchanged (not rendered ahead).

## Phase 3: GPU for single-molecule frames

Today the D3D11 splat runs only for one blink group without continuous populations (DNA-PAINT); dSTORM and PALM
render on the CPU.
- Splat every blink group on the GPU (one kernel set per group), the continuous populations as a photon image (the
  WideField D3D11 path), then noise on the GPU; one upload of events per frame, one readback.
- Batch the frames of the render-ahead queue in one dispatch (as the precomputed stack already does).
- Check: GPU = CPU on >= 99.8 % of pixels (the existing criterion), CPU fallback on any failure.

Done (2026-10-07; `adapter_pixel_hash` unchanged, it renders on the CPU):
- `FluorescenceSimplePlan` covers any movie whose blinks are one group (one blinking label): dSTORM with its initial-ON
  population, PALM with its pre group and pre-state population, DNA-PAINT. `Render` with
  `FluorescenceFrameOptions::populationsOnly` gives the continuous populations alone (in frame order, mean-field or
  per dye as before); `GpuSimulator::RenderFrame(s)` takes them as an extra photon image per frame, added after the
  blinks and before the noise. The lamp's image (both lights) goes into the same image, so fluorescence + BrightField
  renders on the GPU too. Live frames, render-ahead batches (one dispatch) and the Test-tier stacks use it.
- The shader stages each frame's emitter list through group-shared memory, 256 records at a time (pixels identical to
  the per-pixel reads; dSTORM batch 61 -> 48 ms).
- The live GPU path reads the blinks at the spec's focus (stage z less the z drift), as the CPU path; it used the stage
  z.
- Measured: GPU = CPU splat on 99.96 % (dSTORM), 99.9994 % (PALM), 99.935 % (dSTORM + lamp) of pixels, max 1-4 ADU,
  with the same mean-field images (`test_insiliscope.py` checks PALM: 99.998 %). UseGpu On vs Off differs more for a
  fresh dSTORM sample (the WideField host's fp16 mean-field image of the ~10^4 photons/px initial ON), as before.
- The 8-frame batch on the Iris Xe at 256 px, 10 ms: dSTORM 48 ms, PALM 89 ms, DNA-PAINT 173 ms (the splat of the 7000 nm
  kernel: ~145^2 px x 16 block-sum reads per emitter is the cost now, on GPU as on CPU; see the kernel-size item).
  Live (loaded laptop, 10 / 20 ms): PALM 28 / 37 -> 83 / 39 fps, dSTORM 67 / 29 -> 80-85 / 26-43 fps.

## Phase 4: interactive latencies

- First image (3.7-4 s): split it (world, packing the FOV's blocks, PSF kernel, first frame) and start all of them at
  `Initialize()`. The PSF preload exists, so check that it covers the default channel's wavelength.
- Channel switch (up to 4.7 s): a new dye means a new emission wavelength and so a new PSF kernel (~0.8 s each, memo
  of 2). Raise the memo to the channels of the config, precompute the typical labels' kernels in the background after
  init, and keep only what the splat reads (the polyphase block sums, not the full 200 MB of planes).
- Objective switch (0.6-1.9 s): the same, per objective (NA, immersion).
- Stage move (0.5-4 s on a loaded machine): packing and dye blocks of the new region; prefetch along the motion
  direction (phase 2's background thread); the disk cache already keeps packed blocks.
- BrightField focus steps (done 2026-10-07, pixels unchanged): at 20x a new focus re-imaged a ~1350^2 grid for 24
  sources (0.3-0.8 s stall per step; 100x: 50-300 ms). `Simulation/BrightfieldLive.*` keeps the live scene, its last
  4-24 focus images (96 MB) and a thread on half the cores that computes the next foci: an armed z sequence's positions,
  else the last step continued, back, two steps (0.5 um before any step); a scene rebuild or a focus it has not got
  stops it before its next source, and a frame whose focus is in flight waits for it. Below normal priority instead,
  the prefetch starved behind this machine's background load (1-2 s per image, most steps missed); on all cores at
  normal priority it slowed every live frame (20 -> 70-80 ms). The image's inverse FFT also skips the columns outside
  the detection pupil (spec/BRIGHTFIELD.md Cost). Measured (20x/0.75, 256 px, 20 ms, loaded laptop, 0.5 um steps):
  pausing 2 s per focus, most steps show the new focus within 70-180 ms (were 500-800 ms); stepping every 1 s, about
  two thirds do; faster stepping still waits for the image (0.5-0.9 s). XY moves at low magnification rebuild the scene
  (seconds, up to hundreds of MB of exit spectra), so neighbouring xy scenes are not prefetched.
- Kernel size: the default 7000 nm half-width kernel (841 x 841 x 71) is expensive to build, hold and splat; let
  `Renderer.Quality` choose it (Fast: 3000 nm), with `scope_parity` unchanged at Realistic.
- Per-plane kernel extent (the largest SMLM lever, measured 2026-10-07): an emitter's splat covers the whole 145 x 145
  px window of the 7000 nm kernel at every z, while an in-focus plane has nearly all its light within ~1 um. Truncate
  each plane at the radius outside which less than ~1e-4 of its energy lies (far below shot noise), in the C++ and the
  JS twin alike (`scope_parity` against the truncated JS); about 20x fewer taps for in-focus emitters on the CPU and
  the GPU. A deliberate output change: new `adapter_pixel_hash`, gallery and docs numbers.

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
