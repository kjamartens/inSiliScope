# Cell field simulation -- algorithm notes

The *why* behind every non-obvious decision in the cell-field generator. Written against the JS
prototype (`web/prototype/index.html` generator half, `web/prototype/microtubules.js`), which is the reference
implementation the C++ core (`core/`) is ported from; function names below are the JS ones (the C++
uses the same names in PascalCase). Read the relevant section before touching an algorithm, in either
language, and do not "simplify" what it says was fixed on purpose.

Open `web/prototype/index.html` directly in a browser to see it (the prototype; `web/index.html` is the viewer on the WASM core) -- no build step, no server.

## The idea being tested

A continuous, panable field of cells (later: populated with microtubules, lamins,
mitochondria, NUPs, ...) where **a structure's position is a pure function of a
world coordinate, not a stored array**. That means:

- No upper bound on field size — panning to `(1_000_000, -400_000)` µm costs the
  same as panning to the origin.
- The same seed always regenerates the same structure at the same coordinate, so
  revisiting a location after panning away reproduces it exactly (needed for both
  interactive exploration and, later, drift-corrected re-analysis of a
  demoCam_SMLM_MM-style simulated acquisition, where the sample is fixed and the
  stage/FOV moves under it).
- Nothing needs to be precomputed for the whole field — only the chunks currently
  intersecting the viewport (+1 chunk margin) are ever evaluated.

## Method

**Hash**: `pcg4d`, ported line-for-line from webSMLM's `makeSimNoiseRng` /
demoCam_SMLM_MM's `sim::Pcg4d` (`SMLMCounterRng.h`) — the same counter/address-based
hash both codebases already use for camera noise. A draw depends only on its own
`(seed, a, b, c)` address, never on how many draws came before it, which is exactly
what "seed → coordinate" placement needs. Reusing it (rather than a sequential PRNG
like `mulberry32`) keeps this exploration on the same primitive as the rest of the
simulation stack, in case it's worth porting back later.

**Cell placement**: the world is divided into a square chunk grid (`chunkSize`, µm).
Each chunk gets **at most one** candidate cell centre, placed by jittering within a
`jitter` fraction of the chunk (`rawCandidate()` in `index.html`) — the standard
"stratified / jittered grid" trick for cheap, roughly blue-noise point fields used in
procedural generation (one candidate per grid cell, then jitter it). `jitter < 1`
guarantees a minimum spacing of `chunkSize*(1-jitter)` between any two
adjacent-chunk centres; `jitter = 1` allows them to touch. This is weaker than a true
Poisson-disk process, but it's O(1) per chunk with no global state, which is the
actual requirement here — a true Bridson/Poisson-disk sampler needs the whole
previously-placed point set to reject a new candidate against, which doesn't compose
with "any chunk, any order, any thread."

An `occupancy`/`density` draw independently decides whether a chunk's candidate is
even realized as a cell, so the field can be sparser than "one cell per chunk"
without changing the spacing statistics of the cells that *are* present.

**Cell footprint**: an ellipse (`semiMajor`/`semiMinor`, random short/long ratio,
random rotation) whose *geometric mean* diameter — not either single axis — is drawn
from `cellDiamMin`/`cellDiamMax`, so "diameter ~25-30 µm" stays true regardless of how
elongated the cell ends up. That ellipse is then modulated by a few angular harmonics
(`cellRadiusAt()`, amplitude set by `cellBlob`, slider goes to 4) to get a wobbly,
non-convex outline closer to a real confluent-culture cell than a plain ellipse — see
the reference phase-contrast images this round was built against.

**Keeping "diameter" meaning the same size at any blobbiness took three separate
fixes, not one** — worth knowing before touching this code again:

1. The modulation clamp (`CELL_MOD_MIN`/`CELL_MOD_MAX`) is **symmetric around 1**: an
   earlier asymmetric range visibly enlarged the average cell as blobbiness went up
   (more clamp headroom above 1 than below biases the mean radius upward once the
   clamp saturates).
2. Even a perfectly symmetric clamp doesn't fix it on its own — adding *any* angular
   wobble increases enclosed area on average (Jensen's inequality: area is a sum of
   r², and distinct-frequency cosines are exactly orthogonal over a full period, so
   the pre-clamp E[mod²] = 1 + 0.5·Σ(harmAmp²) grows with blobbiness regardless of
   clamp symmetry). `rawCandidate()` corrects for this **numerically per cell**, not
   analytically: an analytic pre-correction based on the *unclamped* variance was
   tried first and failed once the clamp started saturating (routine by blob≈1.5),
   because a saturated signal's true mean-square looks nothing like the unclamped
   formula predicts. The actual fix samples the cell's own already-clamped outline
   (48 points) and rescales `semiMajor`/`semiMinor` by the exact ratio needed so the
   sampled mean-square area matches the unmodulated ellipse's — verified numerically
   flat (equivalent diameter within 0.1 µm) across blob 0–4 in isolation.
3. `envelopNucleus()`'s own lateral correction (below) used to scale the *whole*
   ellipse uniformly to fix any single problem angle — which re-inflated cell size
   exactly in proportion to how deep a lobed outline's dips got, undoing fix #2 by a
   different path (confirmed: with fix #2 alone but the old scale-based envelopment,
   equivalent diameter still grew 27→48 µm from blob 0→3). Fixed by raising a
   *per-cell modulation floor* (`c.modFloor`) instead — see its own comment.

`rOuter` (the radius packing/overlap checks actually use) is pinned to the true
clamped ceiling (`semiMajor * CELL_MOD_MAX`), not the unclamped worst-case sum of
harmonic amplitudes — the unclamped sum routinely overshoots what `cellRadiusAt()`
ever actually draws once the clamp saturates, which was quietly forcing packing to
space cells much further apart than their drawn outlines needed.

**Edge roughness (fractal tail, 2026-10-01)**: real adherent-cell outlines are wiggly at
every scale below the lobes (BF and phase images show sub-µm to few-µm ruffles), not
smooth arcs. Box counting on cultured epithelial cells gives a contour fractal dimension
D = 1.352 ± 0.044 that varies little with cell shape (Fractal analysis of cell boundary
ultrastructure, AFM, 2015); spreading fronts are self-affine with Hurst exponents ~0.6-0.86;
rougher edges go with more filopodia/ruffles. So the outline gets a **spectral tail**:
harmonics k = 6..64 (`TAIL_K0`, `N_TAIL`) with amplitude
`u_k · cellRough · cellBlob/5 · (k/5)^-(2.5-D)` and a uniform random phase, `D =
cellFractalDim` (default 1.35, clamped to [1, 2]), `cellRough` default 0.15.
- Why that exponent: a self-affine graph r(θ) with Hurst exponent H has harmonic
  amplitudes ∝ k^-(H+½) and box-counting dimension D = 2 − H, so the exponent is 2.5 − D.
  The existing k = 2, 3, 5 terms (∝ 1/k) are H = ½, D = 1.5, and the tail starts at the
  k = 5 term's scale, so it extends the same spectrum (world_checks fits the tail's power
  spectrum: slope −(5 − 2D) within 0.25).
- **Proportional to `cellBlob`**: a round cell (blob 0) stays smooth, a lobed one is
  rough — round cells in culture are not fractal either.
- **Multiplicative, after the coarse clamp**: `r = base · clamp(mod) · (1 + tail)`.
  Inside the clamp, the wiggle would be clipped flat wherever the lobes saturate it.
  The tail is soft-clamped, `t / sqrt(1 + (t/TAIL_MAX)²)` with `TAIL_MAX = 0.3`
  (smooth, only sqrt), and `tailBound = min(TAIL_MAX, Σ a_k)` bounds it: `rOuter`
  and the packing reach get a factor `1 + tailBound` (`1 + TAIL_MAX` worst case).
- **Size stays meaning the same**: the area correction samples 256 points with the
  tail on (48 without), and `envelopNucleus()` divides each nucleus sample's
  required modulation by `1 + tail` at that angle, over 256 samples. Dividing by
  `1 − tailBound` everywhere instead was tried first: it raised the floor far too
  much and often fell through to the whole-cell scale-up, making cells ~18% larger.
- **Evaluation**: `cellTailAt()` builds z^k = e^{ikθ} by complex multiplication from
  one `cos`/`sin`. Only `+` and `×`, so it is bit-exact JS ↔ C++. It runs as four
  independent chains (k = 6+q, step z⁴) with four partial sums, because one serial
  chain of 59 dependent multiplications was the microtubule builder's bottleneck.
  Amplitudes use `exp(-e·log(k/5))` (fdlibm on both sides), not `pow`.
- Draws: two `hashStream`s (bases 50 and 51), clear of every CH channel and
  microtubule stream. With `cellRough = 0` nothing changes (bit-identical outline).

**Nucleus** (the original ellipsoid; since 2026-10-05 a shaped ellipsoid, see "Nucleus shape" below): a true 3D ellipsoid per cell — long axis, short/long ratio, and height
(as a fraction of the long axis) are tunable in the sidebar (defaults: long axis
8–12 µm, ratio 0.6–1, height 0.3–0.6× long axis). **Nucleus size is correlated with
cell size, not an independent draw**: both reuse the exact same underlying hash draw
(`sizeT` in `rawCandidate()`), so a cell at the 80th percentile of `cellDiamMin`–
`cellDiamMax` gets a nucleus at the 80th percentile of `nucLongMin`–`nucLongMax` too —
same percentile, not just "correlated noise." Offset from the cell's own centroid by a
random amount up to `nucOffsetFrac × cell radius` (default 0.1 — subtle, "somewhere in
the middle" rather than dead centre).

`envelopNucleus()` guarantees the full ellipsoid stays inside the cell with at least
`nucMargin` µm of clearance (floored at 1 µm) on every side. Vertically this grows
`height` and re-clamps `nucZ` (cheap, no interaction with blobbiness). Laterally it
samples the nucleus's own boundary (32 points) and, for any point the cell's outline
would otherwise fall short of, raises a **per-cell modulation floor**
(`c.modFloor`, read by `cellRadiusAt()` in place of the global `CELL_MOD_MIN`) just
high enough to cover it — filling in a specific dip rather than scaling
`semiMajor`/`semiMinor` (see point 3 above for why that distinction matters). Only
falls back to whole-cell scaling in the rare case even maximum modulation can't cover
the nucleus. Rendered as a stack of horizontal ellipse slices (`drawNucleus()`), sized
by the true ellipsoid equation at each slice height, coloured by absolute z with a
small blue→red depth ramp (`depthColor()`) — a deliberate echo of the "colour by
depth" convention in this project's own reference imagery, so the 3D-ness reads at a
glance instead of looking like a flat blob.

**Cytoplasm height: relaxation instead of smoothing (2026-10-01)**. The raw profile
`cytoHeightAt(dEdge, dNuc)` is a min/max of functions of two distance fields. That
gives it "folds":
- a distance field to a non-convex outline has creases on its medial axis;
- each min/max switch adds a kink.

The old fix was 12 passes of a 3×3 binomial filter on the polar (ring × angle) mesh.
That filter works in *index* space, which is anisotropic in µm: angle neighbours are
~1 µm apart near the edge, ring neighbours tens of nm. The rings are also scaled
copies of the outline, so lobes printed inward as radial spokes, and the folds stayed.

Now `buildCytoHeightGrid()` solves, on a Cartesian grid in the cell's local frame
(`CYTO_GRID_UM` = 0.25 µm), the screened Poisson problem

    h − ℓ²∇²h = h_raw,   ℓ = cytoRelaxUm (default 1 µm; 0 = raw profile)

This is the minimiser of ∫|∇h|² + (h − h_raw)²/ℓ²: a membrane under tension pulled
toward the target profile. It is isotropic and has no kinks off the obstacle; features
wider than ℓ survive, so every rim/mid/dome parameter keeps its meaning.
- **Boundary**: h = 0 on the true outline. Shortley–Weller weights put the boundary
  at its exact position along each grid edge (from the crossings of a 2048-point
  outline polygon with the grid lines), so the edge has no staircase.
- **Obstacle**: h ≥ nucleus top + `nucMargin` over the nucleus footprint (projected
  Gauss–Seidel), so the envelope guarantee survives the smoothing.
- **Solver**: fixed sweep counts (40/20/20) on 1, 0.5 and 0.25 µm grids, with bilinear
  prolongation, lexicographic order: deterministic and bit-exact in C++. Converged to
  ~5 nm against 400 sweeps per level.
- **`dEdge`**: an exact squared Euclidean distance transform of the outside nodes
  (Felzenszwalb–Huttenlocher), minus half a step.
- **`dNuc`**: only computed where the profile depends on it.
- **Edge nodes**: outside nodes next to the outline hold the linear extrapolation
  through 0 at the boundary, so bilinear sampling (clamped at 0) reaches 0 at the true
  edge.

`sampleCytoMeshHeight()` samples this grid; that covers the optical volume,
microtubule ceilings and dye heights. The polar mesh (`cytoRings` × `cytoTheta`,
default 256 angles since the tail goes to k = 64) only carries samples of it for
drawing and contours.

Effect: world_checks measures the 99th percentile of |∇²h| off the nucleus and edge
at 0.69 µm⁻¹, against 4.25 for the raw profile. ~10 ms per cell native.

**3D view**: a simple oblique projection (`project()`, one `tilt` parameter, no
azimuth) — x is untouched, y is foreshortened by `cos(tilt)` and z lifts the point on
screen by `sin(tilt)`. `tilt = 0` is exactly the original top-down view (verified: the
same seed/view produces pixel-identical cell footprints at tilt 0 as the pre-3D pass
did). The cell body itself is drawn as a simple vertical extrusion of its 2D outline
(flat top/bottom, connecting edges at a subsample of vertices) — not a dome, a
known simplification, see below.

**Packing — move, don't shrink** (this round's main ask): cells are never resized to
avoid overlap. Instead, `relax()` runs a fixed number of **Jacobi** iterations: every
candidate looks at its neighbours within a fixed chunk radius (`interactionChunks()`)
and, if overlapping, moves away by half the overlap. Because each candidate only ever
updates *its own* position from a shared same-iteration snapshot, a pair pushes apart
symmetrically with no double-counting and no dependency on Map iteration order —
see the comment above `relax()` for why that's true. If a handful of cells are still
badly stuck after relaxation (e.g. three candidates landing in the same tight
corner), `prune()` drops the lower-priority one of any pair still overlapping beyond a
stricter threshold — "moved apart, or removed entirely," never shrunk. A fixed
per-candidate priority hash means both members of a stuck pair agree on which one
gets dropped regardless of evaluation order. The HUD's "removed (stuck)" count makes
this visible; tune `Density`/`Chunk size`/`Relaxation iterations` until it's ~0 for a
demo, or crank density up to see it kick in.

Two safety nets, both real fixes found by stress-testing, not just precautions:
`CHUNK_CAP` skips packing (falls back to raw, unresolved positions) once the
relaxation window gets too large; `RENDER_CAP` skips drawing entirely once the
*visible* chunk count would mean tens of thousands of detailed polygons per frame —
without it, zooming out far enough genuinely hangs the tab (found by an automated
stress test, not a theoretical concern). Zoom in if either message appears.

**Microtubules** (start, end and the over/under-nucleus rule superseded on 2026-10-05, see "Microtubule starts, ends
and the obstacle envelope" below; `microtubules.js`, kept independent of the cell/nucleus/cytoplasm
generator in `index.html` on purpose — see that file's own header comment): rebuilt
from scratch, genuinely 3D and anchored to the real nucleus/cytoplasm geometry rather
than a free-wandering flat random walk. Per cell: a discrete azimuth-weight table is
built once around the nucleus centre (`buildMtDirectionTable()`), weighted by how much
cytoplasm sits in each direction (a ray-marched distance from the nucleus edge to the
cell's own blobby footprint edge) — directions with more room are proportionally more
likely. Each microtubule draws a weighted azimuth from that table, then a genuine 3D
start point on the nucleus ELLIPSOID's own surface (azimuth + a polar angle drawn
uniform-on-sphere, so starts land anywhere over the nucleus — including straight above
or below it — not only on its equatorial ring, which is what an earlier version did and
which turned out to be a real cause of the whole population sitting flat: with every
start pinned near one height, a path only reached the cytoplasm dome's own mid-height
slope if its wobble happened to climb there, routinely leaving that slope's mid-region
uncovered), pushed outward by a settable random distance plus a settable small XY
jitter (so multiple microtubules starting near the same nucleus region don't all emerge
from exactly one point and can cross above/below each other), an end direction (the
start direction plus a settable jitter), and an end position a settable random offset
inside the cell edge. Between them, a free correlated random walk in XY only (heading
nudged by a random perpendicular kick each step, magnitude "Wobble turn strength") is
generated with a settable path-length budget ("Wobble path length ×", larger than the
straight-line distance so there's real room to loop/double back — genuine U-bends, not
just a perturbed straight line), then corrected via a Brownian-bridge-style drift
subtraction so it lands exactly on both endpoints regardless of how far it wandered.

**z is not part of that walk.** It's tracked as a FRACTION of the LOCAL cytoplasm
ceiling (0 = floor, 1 = the actual rendered height right at that point's own x,y),
interpolated smoothly between the start and end fraction (plus a little tapered
AR(1)-ish noise for roughness) and only converted to an absolute height using the local
ceiling at *that specific point*. An earlier version derived z from the same free walk
as x/y — a roughly constant rate of descent per path-step — while the true ceiling
(`sampleCytoMeshHeight()`) stays near full nucleus height for a while, then drops over a
short band near the nucleus, then goes flat at the rim for the remaining, usually much
longer, distance to the cell edge. That mismatch meant a path's z was typically still
near its starting value once its x/y had already crossed the drop-off band, so the
containment clamp had to snap it down hard and hold it near the floor for the rest of
the path — a real, reported "hard transition, not a slope" artifact. Re-deriving z from
the local ceiling at every point instead makes it ride whatever slope is actually there,
by construction, rather than needing a late external clamp to fix it up.

**Over/under-the-nucleus crossings**: the fraction-of-local-ceiling z model has no idea
the nucleus sits in the way, so on its own a path whose x/y happens to swing across the
nucleus's footprint would just interpolate straight through solid nucleus volume — a
real, reported gap ("basically no microtubules cross over/under the nucleus"), since
every microtubule's end azimuth used to be a small jitter away from its own start azimuth
(so paths stayed radial, never actually needing to cross the nucleus at all). Fixed two
ways together: **End direction jitter now ranges up to 180°** (from a 90° ceiling), so an
end can genuinely land on the far side of the nucleus from where a microtubule started,
not just a nearby azimuth on the same side; and any INTERIOR path point (start/end
themselves are left alone) whose x/y falls within, or close to, the nucleus's own lateral
elliptical footprint (`mtNucleusFootprintBlend()`, ignoring z, smoothly blending to 0 by
40% of the nucleus's own radius beyond its edge) has its z pulled toward clearing the
nucleus vertically — above its top or below its bottom, whichever side that specific
microtubule committed to (drawn once per path, not per point, so it doesn't flip-flop
mid-crossing; "below" is only offered when there's real clearance beneath the nucleus to
begin with). The clearance itself is half of the existing **Cell margin around nucleus**
(`nucMargin`) slider — the same margin the cytoplasm height field already guarantees
above the nucleus everywhere nearby — rather than an unrelated hardcoded value, so
raising/lowering that slider scales this too. This needed no change to the containment
clamp below: a point cleared this way already sits outside the nucleus ellipsoid by
construction (z alone puts it there), so the ordinary nucleus-push clamp simply never
triggers for it.

**This crossing rule shipped with its own real bug — a "sudden, fully vertical drop"**
right where a crossing began or ended, found by the user against a real screenshot. Three
compounding causes, all fixed:

1. The clearance target was blended toward as a FIXED absolute height (`nucTopZ +
   clearance`) regardless of what the ordinary fraction-based height happened to be right
   there — so leaving the blend zone meant snapping from that fixed value back to a
   (often very different) natural one, squeezed into the blend zone's short lateral
   width. Fixed by making the clearance a FLOOR (over) / CEILING (under) applied ON TOP
   of the normal fraction-based height instead of an independent target — it now equals
   the normal height whenever that height already clears the nucleus (no snap at all),
   and only pulls upward/downward when it doesn't.
2. Which side to go — `goOverNucleus` — used to be an INDEPENDENT coin flip, disconnected
   from where the microtubule's own start point actually sits on the nucleus ellipsoid. A
   start drawn on the nucleus's underside combined with a flip that said "go over" forced
   an enormous jump right at the path's very first step, while it was still essentially
   sitting on the nucleus surface. Fixed by tying the choice to `cosPsi` — the same draw
   that already placed the start point above or below the nucleus's equator — so the
   override's first application already agrees with where the path actually begins.
3. The END point was excluded from the override entirely (reasoned, at the time, that its
   `fracEnd` draw was "a specific, meaningful construction" not to be second-guessed —
   true for the START, which is genuinely anchored to the nucleus surface, but NOT true
   for the end, which has no structural tie to the nucleus at all). So whenever the path
   was still inside the blend zone right up to its last interior point (correctly pulled
   to clear the nucleus), the true end simply reverted to its own unrelated
   `fracEnd*ceilH` with no continuity — the same class of bug as #2, just at the finish
   instead of the start. Fixed by routing the end point through the same override too.
A final safety net (`MT_MAX_Z_SLOPE`, `microtubules.js`) also caps z's own rate of change
per nominal step (not per REALIZED lateral distance — a persistent random walk can
occasionally double back to near-zero net lateral movement between two consecutive
points, and bounding purely by that would mask a genuine height change rather than
spread it out) via a few back-and-forth smoothing passes, the same "relax toward
neighbours" idiom `mtNudgeRoundGrid()`'s own collision resolution already uses elsewhere
in this file — catches whatever the three fixes above don't, rather than relying on them
being exhaustive. `tools/check_cellfield_microtubules.mjs`'s own step-slope check (see
below) exists specifically to catch a regression in this; its own ratio metric additionally
needs an absolute `|dz|` floor (`STEEP_MIN_DZ`) before flagging a step, since the RATIO
alone spikes on plenty of genuinely tiny, invisible height wiggles whenever the walk's
own realized lateral step happens to land near zero — an expected occurrence, not a
defect, given the z cap above is deliberately sized against the nominal step length
rather than the realized one.

Every point of every microtubule — including its own start/end — is then clamped to
stay inside the cell's actual cytoplasm volume (inside the blobby footprint, below the
cytoplasm height-field, and outside the nucleus ellipsoid), not just checked at the
endpoints; with the fraction-of-local-ceiling z model this is mostly a safety net for
the collision-nudge pass (which moves points after generation) rather than the primary
height mechanism. **The clamp's own height ceiling is the ACTUAL rendered (relaxed)
height (`sampleCytoMeshHeight()`, bilinearly sampled from the relaxed grid the drawn
mesh is sampled from), not the raw analytic `cytoHeightAt()`** — smoothing (then the
mesh's Laplacian passes, now the relaxation) lowers the sharp nucleus-adjacent dome peak below its raw
analytic value, so clamping against the raw function used to let a point sit above the
surface actually drawn (a real, reported "pokes out of the dome" bug, most visible right
near that peak). **The cell's own outer edge is a cutoff, not a clamp**: the free walk
has genuine excursions past the footprint (more of them the larger Wobble path
length/turn strength are set), and radially clamping every such point back onto the
boundary — the original behaviour — let a single microtubule cross the edge many times,
each crossing pulled onto almost exactly the same rim radius, tracing a dense ring right
along the outline (a real, reported "microtubules hugging the edge" artifact, not a real
cytoskeletal structure). A microtubule is now simply truncated at its first exit from
the footprint, scanning forward from its start — a growing microtubule reaching the
cortex reads as ending there, not bouncing along it, and there's no way for a second or
third crossing to build up a hugging run once the first one already cut the path short.
A per-cell minimum-separation pass (`mtResolveCollisions()`,
spatial-hash-grid accelerated — see its own comment for why) then nudges — and, if still
stuck after a capped number of rounds, regenerates — any microtubule within a settable
minimum 3D distance of another (2D overlap is allowed; only true 3D closeness counts).
Density is expressed as microtubules per µm² of the cell's own footprint area (up to
2/µm²), not a per-cell count.

**Minimum turn radius** (`p.mtMinTurnRadius`, **Min turn radius (µm)**, default 0.35 µm, 0
disables it): the free walk's own per-step heading kick (`heading_new =
normalize(heading_old + kick*perp)`) rotates the heading by `atan2(kick, 1)` — unbounded
as `turnMag` (Wobble turn strength) rises, so a high-wobble path could bend tens of
degrees in a single step, several such steps in a row producing an implausibly tight
hairpin/near-180° kink (a real, reported artifact — "very tight turn radii", not the
intended loopy-but-plausible wobble). Fixed WITHOUT touching Wobble turn strength's own
meaning, as asked: the heading update is recast as an explicit rotation by that same
angle (mathematically identical to the old vector-add-then-renormalize for any unclamped
value), which makes the angle a first-class value that can be clamped — `stepTurnCap` (per
path) derives from the slider via the chord-angle relation for that path's own Step
length, expressed as a radius rather than a flat degrees-per-step number specifically so
it stays a genuine geometric floor regardless of Step length.

**That cap on the raw walk alone was NOT enough — a real, reported "this hasn't happened,
I still see tight turns" follow-up, measured and confirmed**: ~16% of a real run's
interior turns still violated the intended radius, some down to a radius of ~0.001 µm
(essentially a full reversal). The raw-walk cap only bounds the RAW random walk's own
curvature; the Brownian-bridge drift correction added afterward (a separate, additive,
fixed-direction pull growing smoothly from 0 at the start to its full value at the end, no
curvature bound of its own) can still make the REALIZED point-to-point direction bend
sharply wherever the two don't point the same way, however gently the raw walk itself
curves. Fixed by `mtEnforceMinTurnRadius()`, a postprocess that runs on the FINAL rendered
(x,y) — after drift is already added — directly in segment-direction space: it reads off
each segment's own direction and length, clamps the ANGLE step between consecutive
segments to the cap EXACTLY (an explicit rotation, not an iterative pull — a first version
tried softly nudging an over-curved point toward the midpoint of its neighbours, which
needed far too many repeated passes to unwind anything but a mild violation), rebuilds
every point from the fixed start by summing the clamped segment vectors, then re-pins the
true end point with a fresh Brownian-bridge-style drift correction (which can reintroduce
a little curvature of its own, so the whole clamp+redrift cycle repeats a few rounds — each
round's own shortfall is much smaller than the last, so it converges quickly). Reduced the
measured violation rate from 16% to ~0.05% (residual attributed to the independent
containment clamp below, which moves a point's position without any awareness of its
neighbours and runs after this pass).

**Line thickness**: a settable per-cell draw thickness (**Line thickness (µm)**,
`mtLineWidth`, default 0.1 µm — the stroke width sent to Canvas2D is `mtLineWidth *
pxPerUm`, so it's a genuine physical µm thickness that scales with zoom rather than a
fixed screen-pixel width) replaces what used to be a hardcoded `0.1 * pxPerUm` in
`draw()`.

**Path smoothing** (`p.mtSmoothLen`, **Path smoothing (µm)**, default 0.3 µm, up to 3 µm)
controls a PERSISTENCE LENGTH shared by both the xy heading walk and z's fraction noise —
a real, reported follow-up ("it seems to be only in z — is the logic different here?").
It was: z's fraction noise was a flat AR(1) with a fixed 0.8-per-STEP decay coefficient,
decorrelating after a roughly constant NUMBER of steps regardless of how physically long
each step was, so at a smaller Step length (more, smaller steps over the same real
distance) the same few-step correlation window covered much less real distance and z got
visibly MORE jagged per µm exactly where a smaller Step length was chosen to look more
realistic. The xy heading walk has the identical "redraws every step regardless of step
length" shape underneath, but looked far less affected for a specific reason: its
minimum-turn-radius cap (above) already ties a large share of ordinary steps to Step
length via the chord-angle relation, incidentally giving xy a physically-anchored scale
z never had. Both are now reparametrized properly: z as a discretized Ornstein-Uhlenbeck
process in ARC LENGTH (`decay = exp(-stepLen/corrLen)`, the exact relation keeping its
correlation length fixed in real µm regardless of how many steps a given distance is
chopped into, with the innovation's own `sqrt(1-decay²)` scaling — the standard OU
identity — keeping the STATIONARY variance constant so the slider controls only frequency,
never amplitude); xy's heading kick magnitude is scaled by `sqrt(stepLen/corrLen)`, the
worm-like-chain relation for a persistent random walk's tangent angle (variance grows
linearly with arc length, so per-step variance must scale with stepLen for the total over
a fixed real distance to come out step-length-invariant). Both share ONE persistence
length (`corrLen`, floored at Step length so the slider's own minimum, 0, maps to
`corrLen=stepLen` — z's decay ≈ 0.37/step, xy's kick scale = 1 — a deliberately modest,
not zero, "off" floor, not an ever-shrinking blow-up).

Path smoothing and Min turn radius are two DIFFERENT, complementary constraints, not the
same knob under two names: smoothing shapes the TYPICAL, everyday wiggliness (how far the
walk wanders per µm, all cases, including the ones far under the turn-radius cap), while
minimum turn radius is a hard FLOOR only the rare tail ever hits (the worst-case single
step/segment). Verified together (`tools/check_cellfield_microtubules.mjs`'s own scenarios
and a direct single-cell, low-density Playwright screenshot comparison): off (both 0) shows
visibly sharp kinks and zigzags; defaults (0.3 µm / 0.35 µm) already read as gently curving
strands with no visible sharp turns; pushed further (1.5 µm / 0.6 µm) reads as long, sweeping
curves.

**z still had a lot of small-scale jitter even at Path smoothing's own maximum — a real,
reported, separately-investigated follow-up, and a genuinely different bug from the
step-length asymmetry above.** The OU reparametrization keeps fracNoise's STATIONARY
amplitude constant regardless of `corrLen`, shaping only its frequency — but its own
per-step innovation scale (`sqrt(1-decay²)`, `decay=exp(-stepLen/corrLen)`) only shrinks
as `sqrt(stepLen/corrLen)` once `corrLen` exceeds `stepLen` — a SLOW rate. At the default
0.1 µm Step length, measured directly: even at the slider's own maximum (3 µm), ~26% of
the full noise amplitude was still injected fresh every single step (getting that down to
a visually-smooth ~5% would need `corrLen` roughly 800× Step length — tens of µm, far past
any practical slider range). Fixed with a genuinely separate, complementary mechanism: a
plain box-filter MOVING AVERAGE directly over z (window size in points derived from the
same Path smoothing slider and this path's own Step length, so it stays a real-µm window
regardless of Step length; endpoints excluded), applied after the frac/override
computation and before the slope limiter. This directly suppresses whatever
high-frequency content survives the OU process's own frequency-shaping, independent of how
small Step length is set — it does not replace the OU fix (which is still what keeps z
from getting WORSE at a smaller Step length in the first place), it catches what that fix
structurally cannot on its own. Measured: average local z curvature (`|z[i+1]-2z[i]+z[i-1]|`
across a real run) dropped from 0.18 (Path smoothing off) to 0.009 at the slider's maximum
(3 µm) — a ~95% reduction — while the worst single case (the deliberately steep
over/under-nucleus crossing transitions) stayed a comparable, expected magnitude, i.e. this
smooths the everyday wiggle without flattening genuine large crossings.

**Verification**: `tools/check_cellfield_microtubules.mjs` (Playwright) checks three
things independently after generation, across several parameter scenarios including one
with widened End direction jitter/Wobble turn strength specifically to exercise the
over/under-nucleus crossing rule: every point against the same containment bounds the
clamp is supposed to enforce (footprint, nucleus, and `sampleCytoMeshHeight`); a coarse
histogram of z-as-fraction-of-local-ceiling across every point, which should have real
density in the middle third rather than being bimodal (pinned near 0 and 1) — the
signature of the hard-snap artifact above; and, for every consecutive pair of points on a
path, the ratio of |dz| to the lateral distance moved, which should only rarely exceed a
generous multiple — the signature of the "sudden, fully vertical drop" artifact above.
Run it after touching microtubule or cytoplasm-height-field code (`cd web/tools && node
check_cellfield_microtubules.mjs`; needs `web/tools/node_modules` — `cd web/tools && npm
install` once).

**Scale bar**: bottom-right, fixed in screen space (untouched by pan/tilt), picks a
round 1-2-5 length that renders near a target on-screen size at the current zoom
(`drawScaleBar()`). Uses the projection's x-axis scale specifically, which stays
`view.scale * devicePixelRatio` at any tilt (only y is foreshortened) — metrically
correct regardless of the tilt slider.

**Dragging while packing is on**: `relax()` always runs its full configured iteration
count now, including on every `pointermove` during an active drag. It used to drop to
6 iterations while dragging for perceived responsiveness, but that meant packing
looked visibly broken (still-overlapping, under-relaxed cells) for the whole drag and
only "snapped" correct on release — worse than just paying the full cost, which turned
out cheap enough at the chunk counts `CHUNK_CAP` allows through anyway.

## Nucleus shape (2026-10-05, issue 12)

Supersedes the plain ellipsoid above (it remains the special case with every shape term off). Real nuclei have smooth
egg-, bean- or rounded-triangle-shaped outlines with radius deviations of a few to ~10 % (e.g. the control nuclei in
PMC5625896, Fig. 1), and adherent nuclei are wider at the base.

**Shape coordinates**: footprint radius `s` (1 = the outline), direction `t`, height `zeta` in [-1, 1]. A point maps to
the footprint `(u, v) = s R(t) (cos t, sin t)`, then the bend `x = a u, y = b (v + bend (u^2 - 1/4))` (a shear: exactly
invertible, and -1/4 keeps the centroid, the mean of u^2 over the disc), then `z = nucZ + rz k H zeta`, in the nucleus
frame (`nucRot`, `nucOff`; a, b, rz = nucLong/2, nucShort/2, nucHeight/2).

- `R(t) = 1 + soft(sum_{k=2..8} Rc_k cos kt + Rs_k sin kt)`, soft clamp at 0.5; `H(s, t) = 1 + soft(sum_{k=1..8}
  s^k (Hc_k cos kt + Hs_k sin kt))`, soft clamp at 0.6 (`soft(t) = t / sqrt(1 + (t/max)^2)`, as the cell's fractal
  tail). The `s^k` makes H single-valued at the centre. `cos kt`, `sin kt` and `(s e^{it})^k` come from complex-multiply
  recurrences: only + - * /, so the C++ is bit-exact with the JS without any trig per query.
- `k = 2w` below the widest section, `2(1 - w)` above it: `w` (`nucWidestMin/Max`) is the widest section's height as a
  fraction of the nucleus height, and the total height stays `nucHeight` (times H).
- Inside: `|zeta| < 1` and `s < W(zeta) = q + (1 - q) f`, `q = sqrt(1 - zeta^2)`: the ellipsoid's section, widened
  toward the mid-section by the fraction f of the gap (`f = nucAsym` on the bottom half when positive, `-nucAsym` on the
  top half when negative, `|nucAsym| <= 0.9`; 1 would be vertical walls). No section is wider than the mid-section, so
  the footprint outline (s = 1) is the widest extent and every column is one interval: on a side with widening f its
  |zeta| extent is 1 for `s <= f`, else `sqrt(1 - qs^2)`, `qs = (s - f) / (1 - f)`.
- Coefficients: unit-variance uniform draws `(2u - 1) sqrt(3)` times a `k^-nucSmooth` spectrum (`exp(-gamma log k)`)
  normalised to `sum w_k^2 = 1`, so the footprint's rms relative radius deviation is the per-cell irregularity and the
  edge thickness's is `nucThickIrreg`. One hash stream per cell (base 52; the tail uses 50/51), drawn in the order
  irregularity, bend, widest point, Rc/Rs k = 2..8, Hc/Hs k = 1..8.
- All amplitudes 0, `nucAsym` 0 and `w` 0.5 leave `nucShaped` false: every caller then runs its old ellipsoid code.

**Placement.** The nucleus sits on a thin basal layer of cytoplasm: its bottom is `nucBase` (`nucBaseMin/Max`, the old
`NUC_ZFRAC` channel) above the coverslip, `nucZ = nucBase + nucDown`, and the dome top is `nucBase + nucDown + nucUp +
nucMargin` (then floored by the rim/mid heights as before). `nucUp`/`nucDown` are `rz kUp`/`rz kDown` times the largest
`H x column extent` on a 64 x 16 sample grid. This replaced a provisional `nucZ = height x [0.4, 0.6]` with a separately
drawn cell height, which left the nucleus floating; `cellHeightMin/Max` and the `HEIGHT` hash channel (6) are gone.

**Users.** The lateral envelope walks the 256-point footprint polygon (the mid-section is the widest, so the asymmetry
does not change it); the signed distance for the cytoplasm profile is the nearest point on that polygon, the sign from
`s < 1`; the relaxed height grid's obstacle is the nucleus top + margin, maximised over 3 x 3 samples at +-g/2 around a
node so the bilinear surface between nodes clears a steep rim (a wide top); the optical volume (BrightField) takes the
asymmetric chord `[nucZ - below, nucZ + above]`; the microtubule push-out works radially in (s, zeta): the boundary scale
by bisection (24 steps), divided by the containment margin. Drawing uses horizontal sections at `s = W(zeta)`,
`zeta = -cos(pi i / (n - 1))`, both poles included (ABI 9 `isc_cell_nucleus_rings`).

## Microtubule starts, ends and the obstacle envelope (2026-10-05)

Supersedes the direction table, the start on the nucleus ellipsoid, the end-direction jitter and the per-point
over/under-nucleus override described above: none of them fit a shaped nucleus, and the override made paths jagged
where they met it (a footprint blend, a nominal-step slope limiter and per-point clamps fighting each other).

- **Decay lengths** are a percentage of the cell's equivalent diameter `2 sqrt(area / pi)` (48-point outline), floored
  at 0.02 um, so one setting fits small and large cells (defaults 1.6 % and 20 %: ~0.5 and ~6 um).
- **Start**: a point of the cytoplasm volume with density `exp(-d / lambda)`, `d` = an approximate distance to the
  nucleus surface (`mtNucleusGap`: in shape coordinates, the lateral gap `(s - W(zeta)) rDir` and the vertical gap to the
  column, combined as `gl gv / hypot(gl, gv)`, exact for a locally flat surface). Rejection sampling in the nucleus
  bounding box grown by `min(5 lambda, rMax)`, four draws per try always (so the stream position never depends on the
  geometry), at most 128 tries, then the closest valid candidate.
- **End**: a footprint point outside the nucleus footprint with density `exp(-d / lambda)`, `d` the radial gap to the
  outline: theta uniform, `d` exponential by inverse CDF, accepted with probability `r / rMax` (the polar area element).
  Twelve candidates, one picked with weight `exp(kappa (cos a - 1))`, `a` the angle between start -> end and centre ->
  start (`mtDirKappa`; 0 = any direction, so paths cross over and under the nucleus).
- **z**: the fraction-of-ceiling profile with its noise, box-smoothed with a symmetric window that shrinks toward the
  ends (a window cut on one side pulled the first points toward the inner values: a jump next to the fixed endpoint),
  then the **obstacle envelope**: per point a lower bound (top + clearance when riding over an obstacle) and an upper
  bound (ceiling x margin; bottom - clearance when going under); the shortfall is spread into a ramp of slope 1 (forward
  and backward running maxima), box-smoothed and raised back to the shortfall, lift first, then lower. The clearance
  fades in over the first 1 um of arc, so a start next to the nucleus leaves it gradually. Over or under is decided once
  per path (over when the start is above `nucZ` or there is no room below). Obstacles are a generic interface
  (`column(x, y)`, `clearance`, `goOver`), so the ER or other structures can join later.

## BrightField (2026-10-01)

Transmitted light is computed from the world's own geometry only (core `isc_optical_volume_in_window`: cytoplasm,
nucleus, microtubule volume fractions), never from added texture: a structure the world does not simulate must not
appear in brightfield. Multislice + Abbe source sum, thin screen at the phase-weighted height, margin taper against
wrap-around: see [BRIGHTFIELD.md](BRIGHTFIELD.md) for the why of each step.

## Labels, dyes and the light path (2026-10-05, issue 16)

- **Draws nest, so labelling is monotone.** A site is labelled iff its LABEL draw `u < density` -- the threshold the old
  bleaching + persistent split used, so `density = e + p` reproduces the old dye set exactly. The fluorescent fraction
  is a second draw (FLUOR) taken only for labelled dyes, and skipped at fraction 1, so a lower fraction removes dyes
  from a fixed set instead of reshuffling it. Initial ON, aux (bleach budget / pre-state budget) and orientation have
  their own channels: changing one never moves another.
- **Schedules are windowed.** A blinking dye's schedule is generated only for the blinks of `[tLo, 2 tMax)` around the
  query (a 70 % AF647 field is millions of dyes; whole lifetimes ran the JS out of memory). Determinism holds because
  the schedule is still a pure function of the dye and the window bounds only decide which blinks are emitted; the
  cover check (`horizon >= tMax && tLo <= t0`) lets a cached schedule answer a later query. DNA-PAINT keeps its 1 s
  bins (any window is O(window)).
- **Continuous populations are a separate query** (`continuousInWindow`), made per call and only for populations that
  reach the per-dye path, so the dense case (mean field) never materialises per-dye windows.
- **Mean field vs per dye is a cost switch, not a model switch.** Both give the same mean (the grid spans the FOV + the
  2 um margin since issue 16 so the PSF tails match the per-dye path; checked within 1 % total, ~4 % per pixel at low
  labelling); the per-dye path keeps shot noise per dye where it matters (sparse), the mean field keeps dense frames
  affordable. The switch is per frame on the expected emitters (density in the focal slab, or the count in the z range).
- **The per-dye running image** adds and removes unit splats only where windows start or end (a double accumulator,
  float-rounded unit splats) instead of re-splatting every dye every frame; partial frames are added by overlap.
- **The QE lives in the detected fraction**, so a dye's detected rate already holds QE(lambda) and the noise chain runs
  at QE 1 (BrightField, which has no dye spectrum, uses the curve at the lamp wavelength). The flat background is
  multiplied by the QE at the filter's centre.
- **One PSF per effective wavelength, rounded to 2 nm**: two dye states closer than that share a kernel (the kernel memo
  holds groups + 1), which keeps multi-state movies to a few kernels without a visible difference.
- **dSTORM times scale with the excitation rate** relative to the measurement's (Dempsey et al. 2011), so photons per
  blink and duty cycle stay as measured whatever the laser power; PALM activation is linear in the 405 nm intensity
  (plus the primed term).
- **Movies start at 60 s** (`start-sec`), past the dSTORM initial ON phase: the default movie shows the steady state
  without the user knowing about initial ON. The windowed schedules make the late start free.
- **A change acts from now on: the past is kept as epochs, and the core walks each dye's hazards piecewise**
  (2026-10-08, the rate history; since 2026-10-15 inside the clock map, "Exact light history" below). The past is
  segments, each in an *epoch* (the light path's and labels' kinetic inputs then: lasers, excitation filter, dichroic,
  imager concentration, dye overrides). The core walks a dye's hazards with the *same* exponential draws, consumed
  segment by segment (activation `t + E/a_i` while it lands in segment i, else `E -= a_i (s_{i+1} - t)`; ON/OFF/initial
  ON the same in scale form; the blink cycle keeps the old `t += on + off` arithmetic when both end in one segment).
  Why: re-reading the whole past at the current rates made every change act backwards -- PALM with the 405 off emptied
  the field at once (no dye had ever been activated at the new rate), a power step jumped to the look after T seconds
  at the new power. With the walk, every event before a new boundary is bit-identical to the schedule without it and
  the future follows the new rates; one segment is the old code bit for bit. DNA-PAINT changes per 1 s bin (a bin with
  a change takes each part at its own rate, 2026-10-12), so bins already shown never change. A change of dye or mode
  re-reads the past light for the new dye (the epochs' light with the current dye), since the old dye's states mean
  nothing to the new one. Light counts when a frame is *published* if it lights the sample (shutter open, power at the
  sample, and an acquisition takes it or `SampleHolder.TimeWhileIdle` = Running with a hand-opened shutter), so the
  next frame always sees it; a snap outside those counts at its take.
- **The imager background ignores depletion and exclusion from cells**: a flat offset from the concentration and the
  illuminated chamber height. Documented as such rather than modelled half-way.

## PSF pupil sampling (2026-10-07)

- **The pupil grid follows the kernel window** (`ZernikePupilSamples`, JS `pupilSamples`). The chirp-Z evaluator
  samples the pupil disc on an M x M grid with spacing dk = 2 kMax / (M - 4), so the PSF it returns is periodic with
  period 2 pi / dk = (M - 4) lambda / (2 NA). With webSMLM's fixed M = 64 that was ~14.6 um (680 nm) / ~10.9 um
  (510 nm) at NA 1.4, shorter than the 14 um wide default kernel: defocused light folded back into the window (axes
  4.7x the diagonals at 3 um defocus in the green; stepped bands of out-of-focus light in WideField, up to 3-4 sigma
  per frame). Rule: the smallest multiple of 4 in [64, 512] whose period covers the window's diagonal (light can fold
  in only from beyond it), then raised to fill the chirp-Z length (power of two >= M + size - 1, up to 512), which is
  free and shrinks the pupil-edge staircase error. Default window: 160; viewer: 140.
- **Why not more.** The geometric defocus disc (z tan theta: 18 um at 3.5 um defocus and NA 1.49) asked for 512 and
  2.5x the kernel time; measured against 768 samples, 160 is already within 9e-5 of an emitter's photons per camera
  pixel in the core, 1.3e-5 at 2-4 um and 8e-6 beyond, at NA 1.4 and 1.49, 510 and 680 nm (512 gets 1e-6 beyond 4 um).
  At 64 it was 7e-4 to 2.9e-3, 1.5e-4, 1.5e-4. `psf-pupil-samples` > 0 fixes M (64 = webSMLM, the `zernike_psf`
  fixture).

## PSF halo cut (2026-10-07)

- **What.** A blink's splat skips every camera pixel whose share of the emitter's photons is below `psf-halo-cut`
  (`WithHaloCut`, JS `withHaloCut`; MM `Renderer.PsfHaloCut`, set by `Renderer.Quality`: Fast 1e-5, Realistic 3e-6,
  Exhaustive 0). Per kernel plane and splat row dy a column span [lo, hi] (camera pixels from the emitter's pixel): the
  pixels whose largest readable block sum -- block-sum rows/columns [kc - 2 + d os, kc + os + 1 + d os], every sub-pixel
  phase and tap -- is >= threshold, divided by 1.5625 for Cubic/Fft (Catmull-Rom's sum |w| <= 1.25 per axis). The CPU
  splat (`SplatKernel.inl`), the D3D11 gather (`GpuSimD3D11`, a span buffer) and the JS clamp each row to its span.
- **Why no renormalization.** Kept pixels stay exact and every left-out one is off by < threshold x photons; a
  renormalized cut would move the halo's light into the core instead (~0.5 % of the blink at 3e-6, 1-2 % at 1e-5,
  measured 2026-10-08 with `insiliscope_cli --splat-out`; biasing every pixel).
- **Why blinks only.** A WideField or mean-field image sums the halos of many dyes, so a per-pixel bound per dye is no
  bound on the image: cut squares or circles showed as edges of out-of-focus light (1-2 sigma per frame in the
  2026-10-07 study). Single blinks are sparse: at most 0.09 sigma per frame at 1e-5 in the same study.
- **Cost.** The spans are memoized per (kernel, threshold, interpolation); computing them is one pass over the block
  sums (~50 ms for the default kernel). On the default kernel the splat keeps ~58 % of the square's pixels at 3e-6, ~39
  % at 1e-5 (all planes alike; emitters near focus keep less).
- **Checks.** ctest `sr_render`: threshold 0 = the uncut splat bit for bit, kept pixels exact, left-out pixels <
  threshold (Nearest, Linear, Cubic, Fft). `scope_parity`/`engine_check` run the default (3e-6) on both sides.

## Dim blinks (2026-10-14)

- **What.** A blink whose photons in a frame (`CollectFrameEmitters`: photons per frame x its ON share of the frame x
  brightness x the illumination) are below `blink-min-photons` is not splatted in that frame (MM
  `Renderer.BlinkMinPhotons`, set by `Renderer.Quality`: Fast 100, Realistic 50, Exhaustive 0; cli/viewer default 50).
  CPU: `RenderPhotonImage` drops it after collecting (`RenderExtras::minPhotons`, set for the blinks only); GPU:
  `CollectGpuEmitters(..., minPhotons)` (`FluorescenceSimpleGroup::minPhotons`); JS: `renderPhotonImage`'s
  `o.minPhotons`.
- **Why.** Speed (asked for by the user, 2026-10-14): a frame's dimmest blinks are mostly those that only start or end
  in it, each a whole kernel's splat for light that no localization would use. The count is known before the splat.
  The thresholds are an *estimate* of a localizable blink, not a measurement.
- **Not.** Binned frames (approximate SMLM: the same events, one FFT, no saving per event), blinks rendered mean-field,
  and the continuous populations (WideField labels, pre states, the dSTORM initial ON, per dye or mean field) keep every
  dye: dropping dim members of a dense population would darken it as a whole. Nothing is renormalized.
- **Checks.** `scope_parity` (the JS and the C++ cut the same blinks: 100 % identical ADU), ctest `cli_probes`
  (Realistic = the option defaults).

## Blink render regimes (2026-10-08)

Blinks render per label (main group) and per frame in one of three regimes; the continuous populations keep their two
(mean field, per dye).

- **SMLM (splat).** Every event through `RenderPhotonImage` (or the adapter's GPU splat). Cost per event x footprint:
  256 px x 100 frames of DNA-PAINT took 2.9 / 22.9 / 240 s at 1 / 10 / 100 nM imager (12 threads).
- **Binned (approximate SMLM)**, `BinnedBlinks.*` (JS `binned_blinks.js`). The *same* events and the same per-event
  arithmetic as the splat (`CollectFrameEmitters`, JS `frameEmitters`: overlap, photons, illumination at the undrifted
  site, drifted position, `NearestZIndex`), deposited at the nearest cell of a grid of u = `blink-binned-upscale`
  cells per pixel (a divisor of the PSF oversampling, `KernelWidefieldPsf::ValidUpscale`) anchored at the FOV's pixel
  (0, 0) corner, one density per PSF plane; each occupied plane forward FFT x that plane's kernel spectrum, summed,
  one inverse, max(0, .), binned u x u. Margin R cells (no wrap-around) = the kernel radius, capped by the halo cut's
  largest radius over the planes (the cut splat draws nothing beyond it; inside it the grid kernel keeps every cell).
  FFT size `FastSize(W u + 2R, 2, no5)`: radix-5 stages were 2-3x slower per point. The kernel spectra are made on first
  use per plane and kept process-wide for the last two grids (live frames reuse them). Exact: which dyes blink, their
  photons and z plane, the fluctuation; lost: the position within a cell. Cost per FOV area: 3-4 s (u = 1), 13-20 s
  (u = 2), 30-38 s (u = 3) for the movie above at any density. Accuracy (photons): sum within 0.04 %, image relative L2
  4.3 / 2.1 / 1.8 % at u = 1 / 2 / 3 against the splat of the same events; ADU movies 0.5-2 % in the mean image (u =
  2). The C++ FFT is float (`RealFft2d`), the JS float64: `scope_parity` still gives 100 % identical ADU.
- **Mean field**, `BlinkExpectation.*` (JS `blink_expectation.js`). No events: each dye's expected ON seconds in the
  frame, `ExpectedBlinkOnSeconds(mode, segments, t0, t1)`, x the detected rate x the structure's dye density image (the
  continuous populations' mean-field machinery: `BuildMeanField`, drift, per clock region with a host clock).
  - dSTORM / PALM: the chain the core walks (`dyes.cpp`): I (initial ON) -> D (first dark, activation rate) -> ON
    (1/onSec) -> B (p/onSec) | OFF ((1-p)/onSec) -> ON (1/offSec), p clamped to [0.01, 1]; piecewise constant per
    history segment; an I with initialOnSec <= 0 in a segment moves to D at once. The ON-time integral is the integral
    row of exp(Q T) for the 6x6 generator augmented with an integral state (scaling and squaring to ||A|| <= 0.5,
    Taylor order 16, `std::exp` only in DNA-PAINT; no jsm needed: the same double operations in the same order on both
    sides).
  - DNA-PAINT: per 1 s bin and per part of it in one history segment (the core's pieces, below), rate x the exact
    integral over a uniform start in that part of the overlap of a
    min(Exp(tau), 20 tau) blink with [t0, t1) (piecewise closed form at the cut points t0, t1, t0 - 20 tau, t1 - 20 tau).
  - Not modelled: the core's 1000-blink cap per dye and its normal approximation above 30 counts per bin (both
    negligible at real rates).
  - Checked against the core's own blinks (ctest `blink_regimes`, ~2e5 dyes; a first run on 1e7 dyes gave -0.18,
    +0.08, -0.05, +0.05 %): dSTORM initial ON / steady / A-B-A history, PALM +- history, DNA-PAINT +- a history step
    inside a bin, all within 2 sigma (per-dye sums: one dye's blinks are correlated).
- **Why blink mean field is off by default.** Continuous mean field is exact up to shot noise (a sum of independent
  Poisson emitters is one Poisson draw around the summed mean). Blinks are not: which dyes are ON is the signal, and the
  blink fluctuation's variance is about (photons per event) x the shot-noise variance. A mean-field blink movie has the
  right total (+0.1 % at 20 nM DNA-PAINT) but only shot noise frame to frame (std 41 vs 126 ADU): a speed / visual
  approximation, never SMLM data. Thresholds 1e9 = off.
- **Decision.** Mean field first, before the events query (so a mean-field frame draws no events and a movie
  mean-field throughout queries none), from the expected ON emitters at the reference clock (start-sec, or the host
  clock at the FOV centre): `nSlab x onFrac / FOV area > blink-mean-field-density-per-um2` or `nZ x onFrac >
  blink-mean-field-max-emitters` (nZ the structure's dyes in the query rect and z range, nSlab in the FOV within
  `mean-field-slab-nm`, `CountDyes`, half-open). Else binned when the frame's ON emitters (overlap-weighted, what the
  splat would draw) per um^2 of the *FOV* (not the slab: the splat costs per event wherever it is, the FFT per area)
  exceed `blink-binned-density-per-um2` (12.5; the measured splat/binned break-even at u = 2 is ~10 nM DNA-PAINT, ~10 ON
  per um^2: 12.5 sits just above it, 2026-10-09) or
  in all `blink-binned-max-emitters` (1e9). Unlike the continuous populations the regime can go both ways in a movie.
  Not available: blink mean field with a host clock *and* drift (the weighted scene does not follow drift).
- **Host GPU.** `FluorescenceMovie::HostSplatsBlinks(f)`: the adapter's GPU splats only the SMLM frames; the binned
  and mean-field frames' blinks come with the populations (`populationsOnly`) as the extra photon image.
- **Faster FFT** found on the way: `RealFft2d` allocated its line buffers per block of lines; with 12 threads the
  Windows heap serialized them (a 432^2 transform 75-100 ms in a movie vs 5-10 ms alone). Per-thread scratch
  (`LineScratch`), bit-identical; the WideField / mean-field path gains too.

## DNA-origami nanorulers (2026-10-12)

A second specimen (`specimen` 1, `data/specimens.json`): a field of DNA-origami rods on the coverslip
(`core/src/origami.*`, JS twin `web/prototype/scope/origami.js`, bit-exact; designs `data/origami/designs.json` ->
`tools/gen_origami_designs.mjs`). The model reference is the user's `tools/origami_reference/origami_bend.py`; the public
description is `docs/samples/nanorulers/index.md`.

- **Why a specimen switch in the core, not a second world type.** The photophysics (labels, schedules, persistent
  DNA-PAINT cover, rate history, events / continuous / density3d / site queries) needs per site only `(h1, k, n,
  structure)`. So the origami world yields its sites as dye blocks with their own address (`BlockKey` cx, cy, mtIndex
  = tile x, tile y, molecule i; k = spot, n = ring position) and structure 1; everything downstream is unchanged and a
  cell field world is bit-identical to before (golden CellField lines, `adapter_pixel_hash`). Structures are global
  (`ISC_STRUCT_COUNT` 2); a world yields only its specimen's.
- **Field.** 2 um tiles, a Poisson count per tile (`density` x 4 um^2, inversion of one draw, cap 4096), Matern type
  II thinning against the 3x3 tiles (priority, then ty, tx, i break ties), so the result is a pure function of the tile
  and its neighbours: any window, any order. Placement draws are on `seed ^ ORIGAMI_SALT`, bending and landing on the
  molecule hash `h1`: changing Lp never moves or turns a rod.
- **Bending: a 2D worm-like chain** (the rod lies on the glass). n = max(8, ceil(L / (D/2))) segments, dtheta = z
  sqrt(ds / Lp) per segment, exact circular arcs (sinc form; half-angle 0 handled), placed with the chord along the
  yaw and its midpoint at the position (the "aligned" frame of `origami_bend.py`). The normals are Box-Muller on
  `pcg4d(h1, K_BEND, i >> 1, 0)` (cos for even i, sin for odd) with `jsm::` math, so C++ = JS bit for bit; the
  cross-check (`tools/origami_reference/crosscheck.py`) feeds `origami_bend.py` the same normals: same sites to 1e-14
  nm. Statistics (checked in `origami_check.mjs` and `world_checks.Origami`): end-angle variance L/Lp, tangent
  correlation e^(-s/2Lp), <R^2> of the 2D chain. Fold-over (|kappa| D/2 >= 1) counted, kept.
- **Default Lp 1500 nm** (*estimate*): measured bending persistence lengths of origami bundles are 0.74-1 um (4HB),
  1-3 um (6HB), 3-8.2 um (8HB) (chhabra2020); a 12-helix rod is stiffer, so 1500 leans soft.
- **Sites.** Each spot is a ring of 5 strand positions (radius D/2, a modelling choice: no published count per spot);
  `origamiStrandsPerSpot` N keeps ring positions 0..N-1 (the same subset on every rod, so a lower N is a design change,
  not a labelling one). Incorporation is the label's density draw (`DYE_CH::LABEL`, the label %), the linker an
  outward half-ball (radius `origamiLinkerNm`). **v1 is 2D:** every strand at the axis height, attach + D/2 = 13.5 nm;
  `origamiHeights` 1 keeps the ring heights (the 3D path, unvalidated). The engine's default focus on a nanoruler world
  is that height (`isc_origami_heights`).
- **BrightField.** The optical volume of an origami world is empty (13 nm of DNA gives no contrast at these NAs; the
  BRIGHTFIELD.md rule), so BrightField frames hold lamp and noise only.
- **Site spacing and 2D origami (2026-10-13).** Spots are stored in lattice units (u along the body, v across, from its
  centre; the generator expands them) and placed at units x `origamiSpacingNm` (default 80, one value for all designs).
  The body follows: length = lengthUnits s + 2 endNm, in-plane width = widthUnits s + widthNm, height fixed. The rod
  (`Nanoruler`; was `Nanoruler80`) has spots at u = -1, 0, 1 and 20 nm ends: at s = 80 these are the old doubles
  exactly (x = -80, 0, 80, L = 200), so every 80 nm output is unchanged; another s is another L, hence another segment
  count and other bend draws (a different molecule, placement unchanged). A plate (`TUDelftLogo`) is a flat-topped hex
  map (odd-q-down, neighbour distance s; generator constants sqrt(3)/2 printed shortest-round-trip so C++ = JS), the
  bounding box of all cols x rows hexagons, rigid (`ORIGAMI_MIN_SEGMENTS` straight segments: transform = identity,
  outline = the rectangle), landing `flat` (roll 0, never mirrored: image rows = +y as on the camera and screen), one
  strand per cell on the top face (surface point (u s, v s, H/2), linker half-ball about +z: the rod formula at a =
  90 deg written out, so no cos(pi/2) rounding).
- **Not implemented** (schema slots, validated as "not implemented"): cuboid / mesh bodies, 3D bending, stiffness
  patches, built-in curvature, explicit strand lists, face-down plates; broken or misfolded rulers.

## DNA-PAINT rate changes act at once (2026-10-12)

The persistent (DNA-PAINT) blinks are drawn per site and 1 s bin (`PersistentGen`, `core/src/dyes.cpp`, JS twin
`persistentGen`). Under a rate history a bin used to take the kinetics of the segment holding its start, so a change of
imager concentration showed from the next whole second (the free-imager background, computed per frame, changed at
once: users saw the background move and not the blinks). Now a bin with a segment start inside it is cut in pieces:

- **Piece 0** = the bin's own draws (count, starts, ON times, brightness: the old addresses and arithmetic) at the
  bin-start segment's kinetics, keeping only the blinks starting before the next segment's start. So everything before
  a change stays as it was shown (earlier bins and the bin's first part), and a bin without a change is bit for bit
  the old one (golden, `adapter_pixel_hash`, every movie without a history unchanged).
- **Piece i** (each later segment starting inside the bin): its own Poisson count at its rate x its length, starts
  uniform in its part, ON time and brightness from its segment, on draw channels 8 i + purpose and ids j | i << 24.
- Why not one count over the bin at the integrated rate (inverse-CDF starts)? That redraws the part before the
  change: a blink already on screen would vanish or move when the concentration is changed.
- A union of Poisson processes on disjoint intervals is the inhomogeneous process: exact. The mean-field expectation
  (`BlinkExpectation`) integrates per piece the same way. Checks: `world_checks.kinetics_history` (x10 at 60.4 s: the
  blinks before it unchanged, 1612 -> 16204 in [60.4, 61)), `label_parity` (C++ = JS), ctest `blink_regimes`
  (expectation vs the core across straddled bins, -0.2 sigma). In MM a 1 -> 10 nM step shows in the next frames; only
  the imagers bound at the change finish their ON time (~50 ms).

## Nuclear pore complexes, a second cell-field target (2026-10-12, issue 15)

The cell field's second labelled structure (`ISC_STRUCT_NPC` 1; the nanorulers' docking strands moved to 2), imaged
together with the microtubules, each with its own label. `core/src/npcs.*`, JS twin in `web/prototype/index.html`
(`nucSurfaceMesh`, `buildNpcsForCell`), bit-exact (golden `npcs` lines, `label_parity`).

- **One pore = webSMLM's Nup96 model**: 2 rings at -+ sep/2 along the pore axis x 8 corners x 4 arc points (phi =
  -+22.5 / -+67.5 deg on an arc of radius spread/2 around the corner at R), 64 attachment points, ring 0 the
  nucleoplasmic one (inside). R 53.5 nm (webSMLM; thevathasan2019 measured 53.7), sep 50 nm, spread 12 nm. webSMLM's
  TopDown/Sideways orientation and bowl curvature are dropped: the real envelope gives every orientation.
- **Placement: uniform by area over the whole envelope.** A fixed parametric surface mesh (128 angles x 64 side bands
  in alpha, 16 rings per widened cap) with flat triangle areas and a running CDF, not rejection sampling: the side's
  area element is unbounded at zeta = -1 when the base is widened (`nucFBot` > 0, the default). A candidate is a
  triangle by area, a uniform barycentric point mapped back to surface parameters and evaluated exactly, so the centre
  lies on the membrane (apex triangles use the polar rule: no bunching at the poles). The pore axis is the outward
  normal (central differences, delta 1e-5; the face normal where they vanish), the ring orientation a Duff et al. 2017
  frame turned by a uniform angle. Candidate i is `pcg4d(seed ^ 0x4E504331, cx, cy, i)`: address-based, no stream.
- **Count and spacing.** N = round(density x area), not Poisson: a lower density keeps a subset of the same pores.
  A candidate closer than `npcMinDistNm` (180 nm; a pore is ~120 nm across: maeshima2006) to an accepted centre is rejected
  and the next one drawn, up to 50 N candidates; above the jamming density (~17 /um^2 at 200 nm) fewer pores result.
  Default 8 /um^2 (the user's choice, 2026-10-13; was 12; measured interphase densities are 5-8.6 /um^2, maeshima2006,
  dultz2010).
- **Dyes.** Per cell `H1 = pcg4d(seed ^ 0x4E504344, cx, cy, 0).a`, site (k = attachment point, n = candidate index):
  the microtubules' `DYE_CH` channels, so labelling, schedules and orientation work unchanged and the pores' dyes are
  independent of the microtubules'. Dye blocks are the pores whose centre lies in one cell-local 1 um cube, walked
  after the cell's microtubule blocks (`BlockKey` {cx, cy, structure, element, block}).
- **No cell moves**: `PackingFingerprint` and the pack keys skip `npc*`, `ISC_WORLD_VERSION` stays, the block caches stay
  valid. Pores add no BrightField contrast (BRIGHTFIELD.md).

## Label linkage: binder, linker, dye (2026-10-12)

One model of the dye-to-structure distance for every structure (`LinkageOffset`, `core/src/dyes.*`, JS `linkedDye`):

- **Binder** (the probe: antibodies, a nanobody, SNAP/Halo, a fusion FP, a docking strand): its rigid end is the binding
  site + a (attachNm + binderNm) along the structure's axis a (microtubules: the protofilament's radial direction,
  attachNm 12.5; pores: the corner's direction, attachNm 0), as **one product**, so the old numbers are bit-identical.
  Its **flexibility** (binderFlexNm > 0 only) adds a point uniform in a ball (`DYE_CH` BIND_U / BIND_PHI / BIND_R).
- **Linker** (chemical, probe to dye): uniform direction, length uniform in volume in [min, max] (cubes as `l*l*l`).
- **Presets** (`library.json` `linkagePresets`, with sources): antibody shells from fruh2021 Table 1 (binder = shell
  radius - 12.5 nm, flexibility = FWHM / sqrt 2, linker 0.5 nm), nanobody, nanobody + docking strand, SNAP/Halo
  (nieves2019), FP; the splits within the measured bounds are estimates. `Legacy` = the numbers before the model
  (microtubules binder 12, linker 2-5; pores 0, 2-5); a label without a linkage (the core's default, NaN in the ABI)
  keeps them. Defaults (the user's, 2026-10-12): microtubules IgG2-PAINT, pores SNAP-Halo. This moved every default
  microtubule dye (new reference images, `adapter_pixel_hash`).

## Excitation geometry: epi, HILO, TIRF (2026-10-13)

One slider moves the laser spot across the objective's back focal plane (option `illum-bfp`, u): 0 = the centre
(epi), 1 = the critical angle, > 1 = TIRF up to the pupil edge u_max = NA / n_s; beyond it the spot is clipped and no
light reaches the sample (dyes and imager dark; the autofluorescence background, a camera-side term, stays). JS
`web/prototype/scope/excitation.js` (the header has the formulas), C++ twin `Simulation/Excitation.*`.

- **Why u = r / r_crit.** The sine condition makes the BFP radius proportional to n_i sin(theta_i), so moving the spot
  linearly is linear in u = n_i sin(theta_i) / n_s, and the critical angle sits at u = 1 for every objective and
  medium (n_s = `bf-n-medium`, 1.337). The 1.40 objective's edge is 1.047, the 1.49's 1.114, a water objective's < 1.
- **The beam.** The in-plane profile P (Flat, Gaussian, FlatTop: the logistic-edged disc of the SMLM Challenge
  simulator, sage2019), FWHM as % of the FOV width (default Gaussian 100 %; FlatTop until 2026-10-20), centred on the optical axis. On a plane at
  height z it lands as P((x - (z - z_p) tan theta_s) cos theta_i, y): stretched along x by 1 / cos theta_i, and below
  the critical angle moving with z, so an inclined beam is an inclined **sheet** (HILO, tokunaga2008) crossing the axis
  at the focal plane z_p (the movie's focus). Its thickness on the axis is W / (tan theta_s cos theta_i).
- **The field factor F** = |E|^2 in the sample over its epi value, so the lasers' kW/cm^2 "at the sample" keep their
  meaning at u = 0: below the critical angle the Fresnel transmissions (bornwolf1999 1.5.2), above it Axelrod's
  evanescent intensities I_s, I_p (axelrod2001 eqs. 2-3) decaying as exp(-z / d), d = lambda / (4 pi n_s sqrt(u^2 -
  1)) (axelrod2001 eq. 1; ctest `excitation` reproduces axelrod1981's 120 nm example). The mean of s and p (*estimate*:
  unpolarised or circular light). F is continuous at u = 1 and peaks there at ~4 (the interface enhancement is a gain,
  not an artefact); the plane-wave limits diverge at u = 1 (a grazing sheet, an infinite depth), and a real spot has a
  width in the BFP, so u keeps 1e-3 away from 1.
- **Several lines.** d depends on lambda: each dye state's factor mixes its lines' factors by its excitation share per
  line (`lineShares`).
- **Uniform = the old paths.** Flat at u = 0 is exactly 1 everywhere and every renderer skips the factor: outputs are
  bit for bit the old ones. The default (Gaussian 100 %, FlatTop 100 % before 2026-10-20) is not uniform: it moved every default image.
- **Where it acts.** Blinks: photons x the factor at the emitter (`CollectFrameEmitters`, the running images' unit
  splats, the per-dye windows). Mean-field populations: per WideField plane and grid cell F x lateral(cell, plane
  centre) x the plane's exact mean of the depth decay (a 25 nm plane at d = 100 nm would be biased by its centre value);
  planes below the coverslip take z = 0's factor. The DNA-PAINT free imager: the background per pixel is the imager rate
  per um of lit column x the **collected column** over the chamber (`chamber-height-um`, now a real 100 um,
  *estimate*: tape and flow chambers): the factor at each height, convolved with the spread of a dye's light at that
  defocus (|z - z_f| tan theta; emission isotropic, weighted by the Fresnel power transmittance into the glass,
  bornwolf1999 1.5.3, collected at equal power per object-space solid angle up to the NA, richards1959). Fixed on
  purpose: the plain column (the factor straight above the pixel) counted the whole 100 um of an epi chamber, ~8x too
  much, because the light from far above the focus spreads over a disc much wider than the beam: epi's column is about
  the beam's radius (isotropic, on the axis of a disc of radius a: int 1 - d / sqrt(a^2 + d^2) ~ a), TIRF ~ F d, HILO
  about the sheet. Quadrature (excitation.js `imagerMap` = Excitation::ImagerMap): angles at 16-point Gauss-Legendre
  quantiles of the collected distribution in cos theta (equal-weight midpoints were 3 % low: the tail of wide rings),
  24 directions per ring, z intervals growing 1.15x from 50 nm around the focus (TIRF: 64 over 30 decay lengths, each
  with its exact depth integral), on a node grid a third of the profile's edge (or sigma) apart, at least a pixel, at
  most 64 per side, bilinear between; a Flat beam keeps the plain column (a laterally infinite beam: the spread changes
  nothing). **Focus grid:** the map (both parts: the flat background x the footprint, and the imager) is made at foci
  k x 0.25 um and blended linearly between the two around the focus (exact on the grid, so the default 0.5 um is
  unchanged); fixed on purpose: a map per focus cost ~0.2 s per z step in MM's live mode, plus a full GPU re-setup
  (now: the per-pixel statics alone, `PrepareLiveGpu`). The adapter caches the grid's maps (`BgMapCache`, 12 slices /
  192 MB) and makes the neighbours ahead on a worker (`SetBackgroundMapPrefetch`, native only). Left out: the medium / glass index mismatch, near-field emission, imager depletion. That is why
  DNA-PAINT is done in HILO or TIRF: the light presets of the DNA-PAINT modes set u = 0.95 (HILO), the nanorulers'
  specimen u = 1.03 (TIRF, under the 1.40 objective's pupil edge; both *estimates*); in epi the nanorulers' 20 nM
  imager over 100 um saturates the camera.
- **The kinetics follow the local light**, every dye at the dose at its own (x, y, z), on every consumer (MM, cli,
  viewer, the JS reference): the clock map, "Exact light history" below. DNA-PAINT binding does not depend on the light
  (photons x the factor only). The check: a Flat HILO beam (one F everywhere) = epi with every laser x F, per mode
  (`excitation_check.mjs`, ctest `excitation`: 100 % identical ADU).
- **Live under drift** (MM, fixed on purpose): the drift's z part enters a frame's focus on a 10 nm grid (the stage's
  own z stays exact), and the mean-field scenes stay posed at an anchor within 1 um of the FOV, their images shifted to
  it by a phase ramp (`FluorescenceMovie::AnchorMeanField`, the drift movies' `RenderShiftedImages`; the beam's axis
  offset in the plane factor). Before, each frame's new pose and focus refocused every mean-field scene and rebuilt its
  bleach basis, ~0.6 s per frame (dSTORM's initial-ON dyes under HILO: 1.5 -> 12 fps at 10 ms).
- **Known approximations.** The factor is read at the sample's undrifted place (drift is small against the beam);
  supercritical-angle emission (TIRF's collection gain near the interface) and the beam's angular spread in the BFP
  are not modelled; the incidence plane is the camera x axis.

## Exact light history (2026-10-15/16)

Every dye's kinetics run at the dose at its own (x, y, z): one core query per movie (core ABI 14, the **clock map**;
`core/src/clockmap.*`, `World::EventsInMap` / `ContinuousInMap`; JS `web/prototype/scope/clockmap.js`, `world.js`),
the map made by the host's light history (`Simulation/LightHistory.*`; JS `light_history.js`) or, for a cli/viewer
movie, by the same builder from the movie's own past. No levels, z bands, clock regions or dark rule.

- **Why (fixed on purpose).** The stepped history it replaced (`IlluminationHistory`, removed 2026-10-16) counted the
  beam in levels -- 1 at >= half its peak, 1/4 down to 1/8, else 0 -- in 0.25 um tiles x 12 z bands, because each
  distinct (clock, past) was a clock region and each region a core query scanning every dye block it met: the exact
  light made ~800 regions in epi and ~2600 in HILO, seconds per live frame (`tests/parity/map_bench.cpp`: 2601 regions
  578 ms per frame, the map 6 ms). With the levels the blinks and the per-dye populations stepped at the contours (the
  fraction kept after 2 s of dSTORM 0.68 -> 0.85 across the half contour, after 0.7 s of WideField per dye 0.27 ->
  0.48; the mean field had been patched with an "excess" dose), TIRF's first band made a 2.7x jump at 0.1 um and HILO's sheet was
  read at the band's middle (up to ~2 um off).
- **Clock classes (per structure, by mode).** GLOBAL (DNA-PAINT and the off targets): the sample's time T, the
  kinetics history in T -- binding needs no light, so a mixed scene's DNA-PAINT label never reads a dose. DOSE
  (dSTORM, WideField, the initial ON): a dye's clock is its dose tau(x) = total(x) by tOpen, then rate(x) per second.
  Every dSTORM rate is linear in the excitation (the 405 return falls off like the main line, `DyeLibrary`), so the
  kinetics per unit of dose are constant while the *kind* of light is (a chapter, below), and a power or pose change
  moves only the dose: it rebuilds no schedule. Epochs (chapters with other rows) start at the dye's own dose
  bound[e - 1](x). ACTIVATION (PALM): the activation hazard -- steps (the merged past, each recent record) from 0 to
  tOpen, then rate(x) -- reaches the dye's draw; its blinks keep their times in T (the reference model scales only the
  activation, spont + lin a + quad a^2). The pre state's bleach and the photon budget of a WideField dye are the map's
  **bleach fields** (format 2, 2026-10-16): the core ends a budgeted window where the hazard bleachTotal(x) + bleachRate(x)
  (t - tOpen) reaches its draw.
- **Fields.** A field is a sum of terms scale x grid(x - ox - (z - zp) sx, y - oy - (z - zp) sy) x depth(z): bilinear on
  the grid in lerp form (equal nodes give exactly that value, so a Flat beam in epi and a fresh session answer as the
  ABI 11 queries, bit for bit and in order), the depth tables linear at 5 nm (TIRF's decay per line).
- **The history.** T advances by the exposure whenever the sample's time runs (a counted frame; a dark step while
  `TimeWhileIdle` = Running), and starts at the sample's first light (before it every dye is fresh and DNA-PAINT
  stationary: fresh-device stacks stay deterministic). **Records**: maximal intervals of one light -- chapter, magnitude
  (power at the sample over the chapter's reference), pose (the axis within 25 nm of the record's first frame, which it
  keeps; a drifting stack's record holds its frames' poses weighted, so the history records the light the stack used);
  a frame costs O(1) (the open record grows). **Chapters**: maximal intervals of one kind of light -- the geometry but
  the HILO pivot, the kinetic env but the laser powers, the lines' power ratios at the sample. A chapter folds its
  closed records into sparse world-anchored grids (G = sum mag x footprint x dt; G2 = sum mag^2 footprint^2 dt, the
  primed PALM conversion's) on its lattice -- 0.25 um, halved while the profile's curvature would make the bilinear
  reading err by more than 1e-3 of the peak (`LightLatticeUm`: a 64 px FOV's FlatTop takes 0.125 um; on a fixed
  0.25 um lattice it read 2-3 % off between nodes) -- in its own sheared coordinates u = x - z tan theta_s (HILO: the
  footprint at height z is the pivot plane's moved along x). The footprint is the profile at the nodes, out to 1e-4 of
  its peak, evaluated at the record's exact pose. The 16 most recent records stay apart (PALM's steps; past 16 the
  oldest 8 merge, or those over 30 s old), totals exact. A map is built only when a record, chapter or label changes
  (256 px HILO: 4 ms; a 60 s stage drag: fold 0.6 ms per frame, 27 MB held; ctest `history_check`).
- **cli and viewer movies** (no host history) get a map from the same builder: the past = `start-sec` at the movie's
  light and pose, then the movie, over the query box +- 2 um. An MM frame reproduced with `insiliscope_cli --spec`
  follows the MM path (defaults, dSTORM, PALM, mean field, drift: 100 % identical pixels). It replaced `localEvents`
  (every dye queried raw from t = 0 to the brightest factor x the movie's end, its times divided by its factor; PALM by
  a first-blink shift) and the mean field's dose nodes, whose spacing depended on the movie's length (the first 40
  frames of a 1000-frame FlatTop dSTORM movie decayed x0.54 in the corners, a 40-frame movie's x0.78).
- **Core caches.** Per dye block: the schedules on the dye's own clock, keyed by the schedule's fingerprint (a pose
  change: 0 rebuilt); each dye's (tau, rate) per present (the clock's `fpNow`); PALM: each dye's resumable walk and a
  lazy min-heap of activation lower bounds, the live dyes' schedules in an arena; the continuous windows per
  fingerprint sorted by their end on the own clock; with a bleach, their ends in T per present, sorted, so a query costs
  the windows still open. Fixed on purpose: the bleach was the host's (`ClockMapView::BleachEnd` per window), so a
  sample bleached down to the per-dye regime (~3000 of 3.2M dyes left) still pulled every window through the query each
  frame: 332 ms -> 0.9 ms per live frame. Order: by block, then (tOn in T, the own clock's time, dye, blink) -- the ABI 11
  order under a uniform map. JS twin per query, no caches, the same numbers and order (`label_parity.mjs`).
- **Mean field on the map.** Per structure the map's entries on its finest lattice x height bands (`MapEntries`; bands
  merge the scene's dye planes while the photons per dye at Tq, Tq + span and Tq + 2 span stay within 1 % of the
  largest, at most 64): per entry the hazard H at the movie's start and its rate h now; frame 0's weight rate e^-H
  (1 - e^(-h exp)) / h, frame f wb0 e^(-h f exp) (`SetWeightRates`), the bleach basis by groups (<= 8 rates) or
  Chebyshev in the rate, a rate population's frame from the basis by its decay alone (`AnchorDecay` /
  `RenderDecayed`: 9 -> 0.45 ms per frame at 256 px HILO). Each grid cell reads its weight log-bilinearly between its
  four nodes (`MappedWeight`), fixed on purpose: a weight constant per 0.25 um tile biased a bleached Gaussian centre by
  +1.3 % (light and survival anti-correlated inside a tile). Blink populations (the blink mean field, off by default)
  compute their Markov ON expectation only for the entries a dye cell reads (`UsedWeights`: 8.5 % in HILO; 2.6 s ->
  ~0.08 s per frame at 40 px). The mean-field switch: the survival averaged over the lit entries x the lit dyes; no
  windows query when fewer than 1e-4 dyes are expected in the state.
- **Semantics (documented).** PALM's spontaneous activation and DNA-PAINT binding run on T, not only while lit (with
  `TimeWhileIdle` Paused only counted frames advance T); a light-driven dye where no light reaches now is frozen and
  emits nothing (the beam's dim rim is lit and bleaches at its own rate); PALM activation older than ~30 s is merged
  (totals exact); dSTORM and WideField are exact at any age. Resolution: bilinear on the lattice (<= 1e-3 of the peak),
  depth tables at 5 nm, the pose per record (25 nm).
- **Measured** (`tools/test_history.py`, 128 px FlatTop, the fraction kept by radius in 0.03-FOV-width rings): WideField
  mean field 0.27 ... 0.86, largest step 0.10; per dye (3 % labelling): dSTORM 0.06 (the levels: 0.17), WideField 0.08
  (0.16). Live (Basic config, 256 px): where the levels made many regions it is faster (HILO / TIRF dSTORM and PALM,
  0-11 fps with the levels, 6-46 with the map, `bench_bfp`), elsewhere on par (stage moves and z scrubs within ~10 %);
  a WideField sample bleached into the per-dye regime 332 -> 0.9 ms per frame for its windows.

## Known limitations / not yet done

- **Wobble path length ×**'s own slider now goes down to 0.9 (from a 1.0 floor), but
  `params()` still floors the EFFECTIVE value at 1 (`Math.max(1, p.mtWobbleFactor)`,
  where the path-length budget is computed) — a value in `[0.9, 1.0)` currently behaves
  identically to 1.0, since the budget can't meaningfully go below the straight-line
  distance without a larger redesign (removing that floor would need pathBudget<straightLen
  to be handled deliberately, not just left to fall out of the math). Not fixed here since
  it wasn't asked for — flagging it so the 0.9–1.0 range doesn't read as broken.
- Microtubule collision avoidance checks point-to-point distance along each path
  (sampled ~one step length apart) rather than true segment-to-segment distance — an
  adequate proxy at the step lengths this is tuned for, not an exact geometric test.
- The minimum-separation pass is deliberately bounded to a small number of rounds
  (`MT_NUDGE_ROUNDS`/`MT_RESAMPLE_ROUNDS`, `microtubules.js`), not run to convergence —
  measured directly: many microtubules' own FIXED start points end up packed tighter
  than a typical minimum separation once density gets even moderately high (every start
  is confined to a thin band right at the nucleus edge), which can never actually
  resolve since neither point is allowed to move; running more rounds against that just
  burns time for no gain. A residual violation is expected background, not a bug to
  chase, at anything beyond a modest density/separation combination.
- Collision resolution is skipped entirely (microtubules stay individually contained,
  just not mutually separated) once a cell's total point count crosses
  `MT_COLLISION_MAX_TOTAL_POINTS` — real headroom is needed for `mtDensity`'s own
  2/µm² ceiling, which can put hundreds of thousands of points in one large cell.
- (Obsolete since 2026-10-05: the direction table is gone.) Direction weighting only accounts for lateral room (cytoplasm thickness in a given
  azimuth), not the cytoplasm's own height at that azimuth — a direction that's wide
  but very thin (close to the tapered cell edge) is weighted the same as an equally
  wide but tall one.
- (Obsolete since 2026-10-05: ends are picked by `mtDirKappa`.) Over/under-the-nucleus crossings (see above) need **End direction jitter** pushed well
  past its old 90° range to become common — at the default 20° almost every microtubule
  is still structurally radial (start and end azimuths close together) and rarely needs
  to cross the nucleus's footprint at all, so the new clearance rule mostly sits idle
  until a user actually widens the jitter (or pushes Wobble turn strength/path length
  high enough for the free walk to swing across on its own).
- Cell body extrusion is a flat-topped prism, not a dome — real adherent cells are
  much thinner at the edges than the centre. Would need a height *field* over the
  footprint rather than one scalar per cell.
- Packing resolves circle-equivalent (`rOuter`) overlap, not true polygon-vs-polygon
  intersection, so two wobbly outlines can still graze each other slightly even when
  their bounding circles don't overlap — and conversely the relaxation is sometimes
  more conservative than the true outlines would need. Good enough for "close but
  mostly non-overlapping"; not an exact tessellation.
- Relaxation uses a fixed local neighbourhood radius rebuilt from the current
  viewport + a margin, not a globally fixed window — so a cell right at the edge of
  that margin could in principle resolve to a very slightly different position
  depending on how far the margin extends, unlike the raw jittered position (exactly
  reproducible) or the microtubules (exactly reproducible). Not visible in practice
  at the margins currently used (checked: pan-away-and-return reproduces identical
  HUD/positions), but worth knowing if this is pushed further.
- Only one structure type (microtubules) plus now the cell body + nucleus. Lamins,
  mitochondria, NUPs etc. are not started — the intent is that each becomes another
  per-cell generator keyed off the same `(seed, chunkX, chunkY, purpose-channel)`
  address, same pattern as everything else here.
- Not validated against demoCam_SMLM_MM's own structure builders
  (`SMLMStructures.cpp`) or webSMLM's `buildStructure()` in any quantitative way —
  this is a placement-and-navigation prototype, not a density/parameter match yet.

## History

Started as an exploration on webSMLM's `cell-field-simulation` branch (`cell_field_sim/`), kept out of
`webSMLM.html` while the paradigm (infinite, coordinate-addressed field vs. webSMLM's finite, whole-FOV
`buildStructure()`) was open. Its history was imported into this repo under `web/`; the decision taken
is to port it once to C++ (`core/`) and consume that everywhere (see [PLAN.md](../PLAN.md)).

## Microtubule surface labels (debug preview)

`buildMicrotubuleLabelPoints()` (microtubules.js) treats a centerline as a 25 nm-diameter cylinder
carrying the 13_3 protofilament lattice (13 sites per 8 nm dimer ring, neighbouring protofilaments
staggered by 3/13 dimer). Each site becomes surface attachment point -> binder tip (12 nm radial stalk,
nanobody/antibody) -> dye, displaced from the tip by a uniform-in-volume 2-5 nm linker (ported from
`displaceByLinker()` in webSMLM.html). Constants (`MT_RADIUS_NM`, `MT_BINDER_NM`, `MT_LINKER_*`) sit at
the top of that section. Full-network labeling can reach millions of points, so it is not done in
`draw()`; **Debug: view MT labels** labels only 1 µm of the longest microtubule in view.

## Port spec (keep in sync)

[`PORT.md`](PORT.md) is the spec for the C++ port and the inSiliScope integration (cells,
microtubules, dyes/blinks, dummy XY stage). **While that file exists, update it in the same commit as
any change to the prototype that it describes** (hash channels, defaults, geometry, dye model, function
names), and re-freeze the golden vectors (`node tests/parity/golden.mjs --freeze`).
