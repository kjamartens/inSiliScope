# World model

The world is a function, not a data structure: given a seed and an address it returns what is there. Nothing is
stored for the whole field, only the blocks around the window being imaged are ever evaluated and cached.

## Address-based randomness

All randomness comes from `pcg4d`, a counter-based hash on four `uint32` words:

$$ (a, b, c, d) = \mathrm{pcg4d}(\text{seed},\, x,\, y,\, \text{channel}) $$

A draw depends only on its own address, never on how many draws came before it. Consequences:

- Panning to \((10^6, -4\cdot 10^5)\) um costs the same as the origin; revisiting a place reproduces it exactly.
- Queries can run in any order and on any number of threads with the same result (tested: 8 threads = 1 thread).
- A fixed *channel* (purpose) number per kind of draw keeps independent decisions independent.

Caches exist for speed only. The test `world_checks` verifies results are independent of query history, cache size and
dropped caches.

## Cells

The plane is divided into chunks (default 26 um). Each chunk has at most one candidate cell centre, jittered within the
chunk (a stratified-jitter point field: O(1) per chunk, no global state, unlike Poisson-disk sampling). An occupancy
draw decides whether the candidate becomes a cell.

Cell outline: an ellipse (random elongation, rotation) whose *geometric mean* diameter is drawn from a range, modulated by
a few angular harmonics ("blobbiness") to look like a confluent-culture cell. Keeping the diameter meaningful at any
blobbiness needed three separate fixes (a symmetric clamp, a numeric area correction per cell, and a per-cell modulation
floor instead of whole-cell scaling); they are described in `spec/ALGORITHM.md` and must not be "simplified" away.

**Packing - move, don't shrink.** Cells are never resized to avoid overlap. A fixed number of Jacobi relaxation
iterations moves overlapping neighbours apart by half the overlap using a same-iteration snapshot, so a pair separates
symmetrically and independent of evaluation order. Cells still stuck afterwards are removed by a per-candidate priority
hash, never shrunk. Packing is *chaotic*: a \(10^{-15}\) difference in a position changes which cells survive, which is why
the geometry has to be bit-exact between implementations.

Packing runs on fixed 8x8-chunk blocks, so the result for a cell does not depend on the viewport.

## Bit-exactness rules

The core is bit-exact with the original JS prototype (the reference implementation, `web/prototype/`):

- `isc::jsm::sin/cos/asin/atan/atan2/exp/log/cbrt/hypot` are V8's fdlibm routines (generated, never hand-edited); the
  C++ standard library's transcendental functions are not used for anything the JS computes with `Math.*`.
- Types follow the JS: `double` where JS has numbers, `float` exactly where JS uses `Float32Array`; operand order is kept.
- No fast-math, no FMA contraction.
- JS `Math.pow` is the host's `std::pow` in current V8 and cannot be matched across libms: the cytoplasm mesh is bit-exact
  only on the libm Node was built with, and "near" elsewhere (accepted).
- Golden vectors (`spec/golden`) are frozen from the prototype and checked in CI.

## The C ABI

Consumers see only `core/include/insiliscope/insiliscope.h`: `extern "C"`, flat buffers, opaque handles, no exceptions or
STL across it. Queries return cells, assets (cell geometry and microtubules), dye sites, dye events in a time window
(`isc_events_in_window`), z-resolved dye density (`isc_density3d_in_window`, used by widefield) and the optical volume
(`isc_optical_volume_in_window`: cytoplasm, nucleus and microtubule volume fractions per voxel, used by brightfield).
