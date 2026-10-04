// Wrapped in a function ONLY so index.html can read this file's own source text
// (Function.prototype.toString) and hand it to the generator Web Workers: a page
// opened straight from disk (file://) cannot fetch() or importScripts() a sibling
// file, but it can always read a function's source. index.html re-runs the body
// as a plain global script, so everything below behaves as if it were top-level.
window.__MT_SRC = function () {
'use strict';

// ---- Microtubules ------------------------------------------------------------
// Kept in its own file, deliberately independent of the cell/nucleus/cytoplasm
// generator in index.html's own inline <script> -- so this whole module can be
// swapped, deleted, or (eventually) ported on its own. It is a PURE
// computational core: every function here takes plain numbers/plain cell-
// geometry fields (plus a (seed,cx,cy) chunk address) and returns plain
// {x,y,z} point arrays. Nothing in this file touches canvas/DOM -- only
// index.html's own draw() loop (which calls buildMicrotubulesForCell below)
// projects/strokes the result.
//
// Units: micrometres (um) for every length, radians for every angle, unless
// named "Deg". Coordinate frame: the cell's cached LOCAL frame -- centre-
// relative (origin at the cell's own centre, i.e. c.x/c.y already subtracted),
// pre-packRot -- the exact same frame cellRadiusAt()/nucleusSignedDistLocal()/
// cellOutlineLocal()/sampleCytoMeshHeight() already use in index.html. The
// caller (draw()) rotates/translates each returned point into world space via
// the existing localToWorld(cell, x, y) before projecting, exactly like the
// cytoplasm mesh and nucleus already do -- this file never needs to know
// about c.x/c.y/c.packRot at all.
//
// Depends on a small, fixed set of helpers/fields defined in index.html's own
// script (shared global scope, plain <script> tags -- see that file's own
// comment on load order): hashUnit(seed,cx,cy,k), hashStream(seed,cx,cy,base),
// lerp(a,b,t), smoothstep(t), cellRadiusAt(cell,thetaLocal), cellOutlineLocal(cell,n),
// nucleusSignedDistLocal(cell,lx,ly),
// sampleCytoMeshHeight(cell,p,x,y) -- the ACTUAL rendered (smoothed) height
// field, not the raw analytic cytoHeightAt(), so containment matches what's
// drawn; and reads cell.semiMajor/semiMinor/rot/
// harmAmp/harmPh/modFloor/rOuter/nucOffX/nucOffY/nucLong/nucShort/nucRot/
// nucZ/nucHeight (all already resolved by rawCandidate()/envelopNucleus()).
//
// RNG: every random draw goes through the same address-based hashUnit/
// hashStream(seed,cx,cy,channel) convention the rest of the file uses (ported
// from demoCam_SMLM_MM's sim::Pcg4d / SMLMCounterRng.h -- see index.html's own
// top-of-file comment) -- never a mutable-state PRNG. A draw depends only on
// its own address, so a given seed always reproduces byte-identical
// microtubules regardless of pan/zoom/draw order. Channel numbering below is
// namespaced well above the cell generator's own CH.* (which tops out at 44)
// and its hashStream bases (none currently used elsewhere) so nothing collides.
//
// Ground-up portability note: this file is written so the SAME algorithm could
// be reimplemented in demoCam_SMLM_MM's own C++ simulation engine (which
// already shares this project's pcg4d hash) without a redesign -- plain
// scalar math, no JS-only idioms, named constants for every channel/bin-count/
// iteration-cap, and this header documenting units/frame/RNG convention
// explicitly since a C++ port won't have this repo's plan file to refer back
// to.

// Direct hashUnit channels this file uses (never multiplied by hashStream's
// own *4096 addressing -- see hashStream's own comment in index.html) --
// picked well above the cell generator's CH.* (max 44).
const MT_CH_COUNT = 500;

// hashStream base for a single microtubule's OWN generation draws (direction,
// start/end offsets, per-step wobble, priority) -- every draw for microtubule
// `mtIndex` on `resampleRound` attempt comes from one continuous stream, same
// pattern the removed buildMicrotubule() used (hashStream(seed,cx,cy,100+i)),
// just re-spaced (mtIndex directly, resampleRound*100000) so a resample
// attempt draws genuinely different numbers rather than repeating the first
// attempt's own sequence.
const MT_STREAM_BASE = 1000;
const MT_RESAMPLE_SPACING = 100000;

/// Start and end sampling (mtSampleStart / mtSampleEnd): rejection sampling, at most MT_SAMPLE_TRIES candidates
// per point (then the closest valid one); the sampled region reaches MT_DECAY_SPAN decay lengths (exp(-5) = 0.7%
// acceptance at its edge); decay lengths are floored at MT_MIN_DECAY_UM. An end is chosen from
// MT_END_CANDIDATES sampled ends by its direction from the start (mtDirKappa).
const MT_SAMPLE_TRIES = 128;
const MT_DECAY_SPAN = 5;
const MT_MIN_DECAY_UM = 0.02;
const MT_END_CANDIDATES = 12;

/ Minimum-separation pass: bounded like the cell generator's own relax()/
// prune() (see index.html's own comments on PRUNE_ROUNDS) -- best-effort, not
// a hard guarantee, so a pathological configuration can't hang the tab.
// Kept deliberately SMALL (measured, not guessed): near the nucleus, many
// microtubules' own FIXED start points are packed tighter than a typical
// mtMinSeparation by construction once density gets even moderately high
// (mtStartDecayUm concentrates the starts in a thin shell around the nucleus)
// -- those specific conflicts can never actually resolve (neither
// point is allowed to move), so a round involving them never converges to
// "no violation found" and always burns its FULL round budget. Measured
// directly: at mtDensity=0.2 (well under half of the new 2/um^2 ceiling) the
// original MT_NUDGE_ROUNDS=15 + a 3-round resample fallback froze the tab's
// very first draw() call for well over 30s. A handful of rounds is enough to
// fix the realistic case this pass targets (a few stray, genuinely
// resolvable crossings), not the structurally-infeasible one.
const MT_NUDGE_ROUNDS = 5;
const MT_RESAMPLE_ROUNDS = 2;
const MT_POST_RESAMPLE_NUDGE_ROUNDS = 3;
// Per-round pairwise-check ceiling for the grid-based nudge pass (see
// mtNudgeRoundGrid's own comment) -- bounds a single round's cost even when
// many points end up crammed into one crowded neighbourhood (routine near
// the nucleus at high mtDensity), same idiom as CHUNK_CAP/RENDER_CAP
// elsewhere in index.html. This bounds the CANDIDATE-COMPARISON cost once a
// bucket is found; it does NOT bound how many buckets get looked up in the
// first place (every point checks its own 3x3x3 neighbourhood regardless) --
// that cost is what MT_COLLISION_MAX_TOTAL_POINTS below is really for.
const MT_COLLISION_OP_BUDGET = 300000;
// Skips collision resolution entirely for a cell whose microtubules add up to
// more total points than this. The grid (mtBuildSpatialIndex) turns the
// per-round cost from O(M^2*S^2) into O(N), but N*27 bucket lookups per
// round, repeated over several rounds, is still real work -- measured
// directly: 40000 was NOT low enough on its own (see MT_NUDGE_ROUNDS's own
// comment on the actual hang this produced) until the round counts above
// were also cut down; the two fixes together are what keeps this fast.
// Every microtubule is still individually contained inside the cell volume
// (mtClampIntoCytoplasm runs regardless of this cap), just not necessarily
// separated from its neighbours once it's hit -- a documented best-effort
// limit, not silent data loss.
const MT_COLLISION_MAX_TOTAL_POINTS = 8000;
// Hard ceiling on a SINGLE microtubule's own step count (see mtGenerateOne) --
// independent of the per-cell collision caps above, so a small Step length
// combined with a large Wobble path length multiplier can't alone balloon
// one path's point count without bound.
const MT_MAX_STEPS_PER_MT = 1500;
// Hard ceiling on microtubule COUNT per cell -- mtDensity's own 2/um^2
// ceiling times a large blobby cell's real footprint area can otherwise ask
// for many thousands of paths; this bounds generation cost directly rather
// than relying only on the collision-pass caps above (which apply to
// SEPARATION, not to whether the paths get built/drawn at all). Same
// "just cap it" idiom as index.html's own CHUNK_CAP/RENDER_CAP.
const MT_MAX_PER_CELL = 5000;

// Containment clamp keeps a nudged/wobbled point just inside the cell's own
// blobby footprint/nucleus surface rather than exactly on it (a point sitting
// EXACTLY on a boundary can re-trigger the same clamp on the next pass due to
// floating-point rounding).
const MT_CONTAIN_MARGIN = 0.98;

// Slope of the ramp by which mtNucleusEnvelope lifts a path over (or lowers it under) the nucleus: z changes
// at most this much per um moved in xy before the box smoothing rounds the ramp off.
const MT_NUC_RAMP_SLOPE = 1;
// Arc length (um, in xy) over which the nucleus clearance grows from 0 at the start to its full value.
const MT_NUC_CLEAR_RAMP_UM = 1;

// How much vertical clearance a microtubule keeps from the nucleus surface
// while riding over/under it (see mtGenerateOne) -- half of the cell's own
// `nucMargin` (the SAME margin envelopNucleus()/cytoHeightAt() already
// guarantee the cytoplasm keeps above the nucleus everywhere within that
// margin band), not an independent hardcoded value, so raising/lowering
// nucMargin in the sidebar scales this too rather than the two drifting out
// of relation to each other. Floored at 0.05 um so a user-set nucMargin of
// exactly 0 (its slider minimum) still leaves a non-zero, visible gap.
// Minimum radius of curvature (um, `p.mtMinTurnRadius`, "Min turn radius" --
// 0 disables the constraint) a microtubule's own path is allowed to bend at,
// regardless of Wobble turn strength -- see mtEnforceMinTurnRadius's own
// comment below for why this has to run on the FINAL rendered geometry, not
// just the raw random walk that feeds it.

// Slope limiter ceiling -- the settable `p.mtMaxZSlope` ("Max height slope
// (xxy)", slider 1-20 step 0.5, default 5, both wired in index.html): z may
// change at most this many times the lateral distance moved between two
// consecutive points. `mtLimitZSlopeRealized` (below) enforces it once per
// path in buildMicrotubulesForCell, after collision resolution, on the
// REALIZED lateral distance between the final points -- the ratio a viewer's
// own eye reads as "how steep is this segment", and the same ratio
// tools/check_cellfield_microtubules.mjs's own step-slope check measures.

function mtNucleusClearance(p) {
  return Math.max(0.05, 0.5 * p.nucMargin);
}

// Moving average over `vals` of every interior point; set(i, mean) receives the results (the endpoints are left
// alone). The window i-h..i+h is symmetric, h = min(half, i, n-1-i): it shrinks toward the ends, so a slope there
// is kept (a window cut on one side only pulled the first points toward the inner values, a jump next to the
// fixed endpoint). Prefix sums, in index order.
function mtBoxSmooth(vals, half, set) {
  const n = vals.length;
  if (n < 3 || !(half > 0)) return;
  const cum = new Array(n + 1);
  cum[0] = 0;
  for (let i = 0; i < n; i++) cum[i + 1] = cum[i] + vals[i];
  for (let i = 1; i < n - 1; i++) {
    const h = Math.min(half, i, n - 1 - i);
    set(i, (cum[i + h + 1] - cum[i - h]) / (2 * h + 1));
  }
}

// Keeps a path's interior z within [lo[i], hi[i]] (the nucleus column it rides over or under, and the
// cytoplasm ceiling) with smooth corrections instead of per-point clamps, which turned every rim crossing into
// a kink. For each side: the shortfall need[i] is spread into a ramp of slope MT_NUC_RAMP_SLOPE per um in xy
// (forward and backward running maxima: the smallest correction >= need that changes no faster than the
// ramp), box-smoothed over `half` points to round its corners, then raised back to need where the smoothing
// cut a peak. Lift first (lo), then lower (hi). Endpoints never move.
function mtNucleusEnvelope(pts, lo, hi, half) {
  const n = pts.length;
  if (n < 3) return;
  const ds = new Array(n).fill(0);
  for (let i = 1; i < n; i++) ds[i] = Math.hypot(pts[i].x - pts[i - 1].x, pts[i].y - pts[i - 1].y);
  const need = new Array(n), ramp = new Array(n);
  for (const sign of [1, -1]) {
    let any = false;
    for (let i = 0; i < n; i++) {
      need[i] = i === 0 || i === n - 1 ? 0 : Math.max(0, sign > 0 ? lo[i] - pts[i].z : pts[i].z - hi[i]);
      if (need[i] > 0) any = true;
    }
    if (!any) continue;
    ramp[0] = need[0];
    for (let i = 1; i < n; i++) ramp[i] = Math.max(need[i], ramp[i - 1] - MT_NUC_RAMP_SLOPE * ds[i]);
    for (let i = n - 2; i >= 0; i--) ramp[i] = Math.max(ramp[i], ramp[i + 1] - MT_NUC_RAMP_SLOPE * ds[i + 1]);
    const corr = ramp.slice();
    mtBoxSmooth(ramp, half, (i, v) => { corr[i] = Math.max(v, need[i]); });
    for (let i = 1; i < n - 1; i++) pts[i].z += sign * corr[i];
  }
}

// Post-process curvature limiter, run on the FINAL rendered (x,y) of a
// microtubule's own path -- see mtGenerateOne's own comment on why the
// per-step heading-turn cap in the raw random walk isn't enough on its own
// (the Brownian-bridge drift correction added afterward is a separate,
// additive, fixed-direction pull with no curvature bound of its own, and the
// REALIZED point-to-point direction can still bend sharply even when the raw
// walk's own curvature is well within bounds).
//
// Works in SEGMENT-VECTOR space, not by nudging point positions directly: a
// first version tried pulling an over-curved point toward the midpoint of
// its two neighbours, a soft correction needing many repeated passes to
// converge on anything but a mild violation -- measured directly, it left
// ~3% of turns still violating the cap after 20 passes, some barely reduced
// from a near-total reversal at all. This version reads off each segment's
// own direction+length, clamps the ANGLE step between consecutive segments
// to the cap EXACTLY (one shot, not an iterative pull -- same "rotate to an
// explicit clamped angle" idiom mtGenerateOne's own raw-walk heading cap
// uses, just applied to the realized geometry instead of the walk that
// produced it), then rebuilds every point from the fixed start by summing
// the (now-clamped) segment vectors. That reconstruction generally no longer
// lands exactly on the true end point, so a fresh Brownian-bridge-style
// drift correction (same smoothstep taper as mtGenerateOne's own) pins it
// back -- which can reintroduce a little curvature of its own, so this whole
// clamp+redrift cycle repeats a few rounds; each round's own drift shortfall
// is much smaller than the last (most of the curvature was already fixed),
// so it converges quickly. Endpoints (i=0/i=steps) are never moved.
// `minRadius<=0` (the slider's own "off" position) skips this entirely.
function mtEnforceMinTurnRadius(pts, minRadius) {
  const n = pts.length;
  if (!(minRadius > 0) || n < 3) return;
  const endX = pts[n - 1].x, endY = pts[n - 1].y;

  for (let round = 0; round < 8; round++) {
    const segX = new Array(n - 1), segY = new Array(n - 1), segLen = new Array(n - 1);
    for (let i = 1; i < n; i++) {
      segX[i - 1] = pts[i].x - pts[i - 1].x;
      segY[i - 1] = pts[i].y - pts[i - 1].y;
      segLen[i - 1] = Math.hypot(segX[i - 1], segY[i - 1]);
    }

    let changed = false;
    for (let i = 1; i < segX.length; i++) {
      const prevLen = segLen[i - 1], curLen = segLen[i];
      if (prevLen < 1e-9 || curLen < 1e-9) continue;
      const prevAng = Math.atan2(segY[i - 1], segX[i - 1]);
      const curAng = Math.atan2(segY[i], segX[i]);
      let dAng = curAng - prevAng;
      while (dAng > Math.PI) dAng -= 2 * Math.PI;
      while (dAng < -Math.PI) dAng += 2 * Math.PI;
      const avgLen = (prevLen + curLen) / 2;
      const cap = 2 * Math.asin(Math.min(1, avgLen / (2 * minRadius)));
      if (Math.abs(dAng) <= cap) continue;
      const clampedAng = prevAng + Math.sign(dAng) * cap;
      segX[i] = Math.cos(clampedAng) * curLen;
      segY[i] = Math.sin(clampedAng) * curLen;
      changed = true;
    }

    for (let i = 1; i < n; i++) {
      pts[i].x = pts[i - 1].x + segX[i - 1];
      pts[i].y = pts[i - 1].y + segY[i - 1];
    }

    const dxErr = endX - pts[n - 1].x, dyErr = endY - pts[n - 1].y;
    if (Math.abs(dxErr) > 1e-9 || Math.abs(dyErr) > 1e-9) {
      for (let i = 1; i < n - 1; i++) {
        const t = i / (n - 1);
        const s = t * t * (3 - 2 * t); // smoothstep
        pts[i].x += dxErr * s;
        pts[i].y += dyErr * s;
      }
      pts[n - 1].x = endX; pts[n - 1].y = endY;
    }

    if (!changed) break;
  }
}

// A steep FIRST/LAST segment is a genuinely different case from a steep
// INTERIOR one, and `mtLimitZSlopeRealized` below (which only ever nudges
// i=1..n-2) structurally cannot fix it: i=0 and i=n-1 are never moved (their
// z is a deliberate, meaningful value -- a real point on the nucleus
// ellipsoid's surface, or an independently-drawn end fraction), so when the
// offending segment IS the endpoint one, the interior relax pass is bounded
// by that fixed z no matter how many rounds it runs -- it can only ever pull
// the NEIGHBOUR closer, and the neighbour's own budget is itself limited by
// ITS other neighbour. Measured directly: this is exactly why a single-digit
// mtMaxZSlope could still leave triple-digit realized slopes after the
// interior-only relax pass -- every worst offender traced back to the very
// first or last segment of its path. Fixing an impossible endpoint jump by
// deleting the endpoint (promoting its former neighbour to be the new,
// effective start/end) reads as the microtubule simply not growing that one
// extra bit -- a small, usually invisible truncation -- rather than forcing
// a still-too-steep segment to exist regardless, or fighting to nudge a
// value that structurally can't move far enough to matter. Run BEFORE
// mtLimitZSlopeRealized so that pass's own interior relax works from
// already-valid endpoints. Capped at MT_MAX_END_TRIM points per end so a
// pathological path (e.g. its whole first quarter genuinely near-vertical)
// can't be trimmed down to nothing.
const MT_MAX_END_TRIM = 50;
function mtTrimSteepEnds(pts, maxSlope) {
  if (!(maxSlope > 0)) return;
  for (let n = 0; n < MT_MAX_END_TRIM && pts.length > 2; n++) {
    const a = pts[0], b = pts[1];
    const lateral = Math.hypot(b.x - a.x, b.y - a.y);
    if (Math.abs(b.z - a.z) <= maxSlope * Math.max(lateral, 1e-6)) break;
    pts.shift();
  }
  for (let n = 0; n < MT_MAX_END_TRIM && pts.length > 2; n++) {
    const last = pts.length - 1;
    const a = pts[last], b = pts[last - 1];
    const lateral = Math.hypot(a.x - b.x, a.y - b.y);
    if (Math.abs(a.z - b.z) <= maxSlope * Math.max(lateral, 1e-6)) break;
    pts.pop();
  }
}

// Final slope limiter (see mtMaxZSlope above): runs ONCE PER PATH in
// buildMicrotubulesForCell, AFTER collision resolution has finished moving
// points around, and measures the ratio against the REALIZED lateral
// distance between the final pair of points. Alternating-sweep relaxation (a
// violation can span several points; each pass propagates the correction one
// step further). Endpoints (i=0/i=n-1) are never moved -- their z is a
// deliberate value (the sampled start and end). See `mtTrimSteepEnds` above
// for the complementary fix when the endpoint ITSELF is the unfixable
// offender, which must run first.
// `maxSlope<=0` (not reachable via the slider, whose minimum is 1, but kept
// as a safe no-op for a directly-scripted config) skips this entirely.
function mtLimitZSlopeRealized(pts, maxSlope) {
  const n = pts.length;
  if (!(maxSlope > 0) || n < 3) return;
  for (let pass = 0; pass < 20; pass++) {
    let changed = false;
    const forward = pass % 2 === 0;
    for (let k = 1; k < n - 1; k++) {
      const i = forward ? k : n - 1 - k;
      const prev = pts[i - 1], cur = pts[i], nxt = pts[i + 1];
      const dPrev = Math.hypot(cur.x - prev.x, cur.y - prev.y) * maxSlope;
      const dNxt = Math.hypot(nxt.x - cur.x, nxt.y - cur.y) * maxSlope;
      const lo = Math.max(prev.z - dPrev, nxt.z - dNxt);
      const hi = Math.min(prev.z + dPrev, nxt.z + dNxt);
      // lo>hi: the two neighbours are themselves too far apart in z for any
      // single value to satisfy both slope budgets at once -- left alone (the
      // containment clamp still guards it).
      if (lo > hi) continue;
      if (cur.z < lo) { cur.z = lo; changed = true; }
      else if (cur.z > hi) { cur.z = hi; changed = true; }
    }
    if (!changed) break;
  }
}

function mtNormalize3(v) {
  const n = Math.hypot(v[0], v[1], v[2]) || 1;
  return [v[0] / n, v[1] / n, v[2] / n];
}
// Exact polygon area (shoelace) over cellOutlineLocal's own sampled points --
// reused directly rather than a circle-equivalent approximation, since the
// footprint is already sampled at fixed angular steps for rendering.
function mtShoelaceArea(pts) {
  let area = 0;
  for (let i = 0; i < pts.length; i++) {
    const [x1, y1] = pts[i], [x2, y2] = pts[(i + 1) % pts.length];
    area += x1 * y2 - x2 * y1;
  }
  return Math.abs(area) / 2;
}

/// Approximate distance (um) from cell-local (x, y, z) to the nucleus surface, or -1 inside the nucleus. In the
// nucleus's shape coordinates (index.html "Nucleus shape"; the plain ellipsoid is f = 0, H = 1, no lobes): the
// gap gl along the footprint direction at this height (to the section W(zeta)) and the vertical gap gv to the
// column below/above, combined as the distance to the plane through both intercepts, gl gv / hypot(gl, gv) (exact
// for a locally flat surface). Outside the footprint there is no column: gl alone, or above the top/below the
// bottom, the nearer of the rim and the pole's edge. Same inside test as mtClampIntoCytoplasm.
function mtNucleusGap(cell, x, y, z) {
  const up = z >= cell.nucZ;
  let s, rDir, ext, f;
  if (cell.nucShaped) {
    const B = nucBallLocal(cell, x, y);
    s = B.s;
    const q = nucMapLocal(cell, 1, B.C, B.S, 0);
    rDir = Math.hypot(q[0] - cell.nucOffX, q[1] - cell.nucOffY);
    ext = (cell.nucHeight / 2) * (up ? cell.nucKUp : cell.nucKDown) * (s < 1 ? B.H : nucThickAt(cell, 1, B.C, B.S));
    f = up ? cell.nucFTop : cell.nucFBot;
  } else {
    const a = Math.max(1e-6, cell.nucLong / 2), b = Math.max(1e-6, cell.nucShort / 2);
    const dx = x - cell.nucOffX, dy = y - cell.nucOffY;
    const cr = Math.cos(cell.nucRot), sr = Math.sin(cell.nucRot);
    const u = (dx * cr + dy * sr) / a, v = (-dx * sr + dy * cr) / b;
    s = Math.hypot(u, v);
    const C = s > 1e-12 ? u / s : 1, S = s > 1e-12 ? v / s : 0;
    rDir = Math.hypot(a * C, b * S);
    ext = cell.nucHeight / 2;
    f = 0;
  }
  const zeta = Math.abs(z - cell.nucZ) / Math.max(1e-6, ext);
  if (zeta < 1) {
    const W = nucSectionW(zeta, f);
    if (s < W) return -1;
    const gl = (s - W) * rDir;
    if (s >= 1) return gl;
    const gv = (zeta - nucColumnExt(s, f)) * ext;
    const h = Math.hypot(gl, gv);
    return h > 1e-12 ? gl * gv / h : 0;
  }
  if (s < 1) return (zeta - nucColumnExt(s, f)) * ext;
  return Math.min(Math.hypot((s - 1) * rDir, zeta * ext), Math.hypot((s - f) * rDir, (zeta - 1) * ext));
}

// Inside the cytoplasm volume (footprint, ceiling, coverslip; the nucleus is checked by the caller), with the
// containment margin so a sampled point is never re-clamped.
function mtInCytoplasm(cell, p, x, y, z) {
  if (!(z > 0)) return false;
  if (!(Math.hypot(x, y) < cellRadiusAt(cell, Math.atan2(y, x)) * MT_CONTAIN_MARGIN)) return false;
  return z < Math.max(0, sampleCytoMeshHeight(cell, p, x, y)) * MT_CONTAIN_MARGIN;
}

// START: a point of the cytoplasm volume (nucleus excluded) with density ~ exp(-d / mtStartDecayUm), d = the
// distance to the nucleus surface (mtNucleusGap): uniform candidates in the nucleus's bounding box grown by
// MT_DECAY_SPAN decay lengths, kept with probability exp(-d / lambda). Four draws per candidate, always.
function mtSampleStart(cell, p, geom, next) {
  const lam = Math.max(MT_MIN_DECAY_UM, p.mtStartDecayUm);
  const L = Math.min(MT_DECAY_SPAN * lam, geom.rMax);
  const nb = geom.nucBox;
  const x0 = nb[0] - L, x1 = nb[2] + L, y0 = nb[1] - L, y1 = nb[3] + L, z1 = cell.nucZ + cell.nucUp + L;
  let best = null, bestD = Infinity;
  for (let t = 0; t < MT_SAMPLE_TRIES; t++) {
    const x = lerp(x0, x1, next()), y = lerp(y0, y1, next()), z = z1 * next(), u = next();
    const d = mtNucleusGap(cell, x, y, z);
    if (d < 0) continue;
    const accept = u < Math.exp(-d / lam);
    if (!accept && !(d < bestD)) continue;
    if (!mtInCytoplasm(cell, p, x, y, z)) continue;
    if (accept) return { x, y, z };
    best = { x, y, z }; bestD = d;
  }
  if (best) return best;
  // Nothing inside the cytoplasm (a degenerate cell): just outside the nucleus rim, at its widest section.
  const cr = Math.cos(cell.nucRot), sr = Math.sin(cell.nucRot), r = cell.nucLong / 2 / MT_CONTAIN_MARGIN;
  return { x: cell.nucOffX + r * cr, y: cell.nucOffY + r * sr, z: cell.nucZ };
}

// END (x, y only; its z is a fraction of the local ceiling): a footprint point outside the nucleus footprint with
// density ~ exp(-d / mtEndDecayUm), d = the radial gap to the outline (inside by MT_CONTAIN_MARGIN). Polar
// proposal: theta uniform, d exponential (inverse CDF), kept with probability r / rMax (the area element).
// Three draws per candidate, always.
function mtSampleEnd(cell, p, geom, next) {
  const lam = Math.max(MT_MIN_DECAY_UM, p.mtEndDecayUm);
  let x = 0, y = 0;
  for (let t = 0; t < MT_SAMPLE_TRIES; t++) {
    const th = next() * Math.PI * 2, d = -lam * Math.log(1 - next()), u = next();
    const rc = cellRadiusAt(cell, th) * MT_CONTAIN_MARGIN;
    const r = rc - d;
    x = Math.cos(th) * Math.max(0, r); y = Math.sin(th) * Math.max(0, r);
    if (!(r > 0) || !(u * geom.rMax < r)) continue;
    if (nucleusColumnLocal(cell, x, y)) continue;
    return { x, y };
  }
  return { x, y };
}

// The end for a start S: MT_END_CANDIDATES ends from mtSampleEnd, one picked with weight
// exp(mtDirKappa (cos a - 1)), a = the angle in xy between S -> end and the outward direction at S (from the
// nucleus centre). kappa 0: any end (paths cross over and under the nucleus); large: roughly radial.
function mtPickEnd(cell, p, geom, next, S) {
  const kappa = Math.max(0, p.mtDirKappa);
  const ox = S.x - cell.nucOffX, oy = S.y - cell.nucOffY, oL = Math.hypot(ox, oy);
  const cands = new Array(MT_END_CANDIDATES), w = new Array(MT_END_CANDIDATES);
  let sum = 0;
  for (let k = 0; k < MT_END_CANDIDATES; k++) {
    const E = mtSampleEnd(cell, p, geom, next);
    const ex = E.x - S.x, ey = E.y - S.y, eL = Math.hypot(ex, ey);
    const cos = oL > 1e-9 && eL > 1e-9 ? (ex * ox + ey * oy) / (eL * oL) : 0;
    cands[k] = E;
    w[k] = Math.exp(kappa * (cos - 1));
    sum += w[k];
  }
  const target = next() * sum;
  let acc = 0;
  for (let k = 0; k < MT_END_CANDIDATES; k++) {
    acc += w[k];
    if (target < acc) return cands[k];
  }
  return cands[MT_END_CANDIDATES - 1];
}

// Per-cell geometry cache (local outline, footprint area, nucleus bounding box,
// largest outline radius) -- same reasoning as index.html's own cytoCache:
// these fields are a pure function of the cell's own already-resolved shape
// (semiMajor/semiMinor/rot/harmAmp/harmPh/modFloor/nucleus fields), not of its
// current (packing-relaxed) x,y, so they are not recomputed on every draw()
// call. Keyed on chunk id, invalidated by a signature over the cell's own
// resolved numeric fields (not on index.html's `p` directly, keeping this
// file self-contained).
const MT_GEOM_CACHE_MAX = 6000;
const MT_RMAX_SAMPLES = 512;
const mtGeomCache = new Map();
function mtCellShapeSig(cell) {
  return [cell.semiMajor, cell.semiMinor, cell.rot, cell.harmAmp.join(','), cell.harmPh.join(','), cell.tailAc.join(','), cell.tailAs.join(','),
    cell.modFloor, cell.nucOffX, cell.nucOffY, cell.nucLong, cell.nucShort, cell.nucRot,
    cell.nucZ, cell.nucHeight, cell.rOuter, cell.nucShaped ? cell.nucShapeSig : ''].join('|');
}
// Nucleus footprint bounding box [x0, y0, x1, y1], cell-local: the polygon (shaped) or the rotated ellipse.
function mtNucleusBox(cell) {
  if (cell.nucShaped) {
    let x0 = Infinity, y0 = Infinity, x1 = -Infinity, y1 = -Infinity;
    for (const [x, y] of cell.nucPoly) { x0 = Math.min(x0, x); y0 = Math.min(y0, y); x1 = Math.max(x1, x); y1 = Math.max(y1, y); }
    return [x0, y0, x1, y1];
  }
  const a = cell.nucLong / 2, b = cell.nucShort / 2, cr = Math.cos(cell.nucRot), sr = Math.sin(cell.nucRot);
  const hx = Math.hypot(a * cr, b * sr), hy = Math.hypot(a * sr, b * cr);
  return [cell.nucOffX - hx, cell.nucOffY - hy, cell.nucOffX + hx, cell.nucOffY + hy];
}
function getMtCellGeometry(cell) {
  const key = cell.cx + ',' + cell.cy;
  const sig = mtCellShapeSig(cell);
  const entry = mtGeomCache.get(key);
  if (entry && entry.sig === sig) return entry;
  if (mtGeomCache.size > MT_GEOM_CACHE_MAX) mtGeomCache.clear();
  const localOutline = cellOutlineLocal(cell, 48);
  // Largest outline radius (x the containment margin, as mtSampleEnd uses it): its acceptance r / rMax.
  let rMax = 0;
  for (let i = 0; i < MT_RMAX_SAMPLES; i++) rMax = Math.max(rMax, cellRadiusAt(cell, (i / MT_RMAX_SAMPLES) * Math.PI * 2));
  const fresh = {
    sig,
    localOutline,
    areaUm2: mtShoelaceArea(localOutline),
    nucBox: mtNucleusBox(cell),
    rMax: rMax * MT_CONTAIN_MARGIN,
  };
  mtGeomCache.set(key, fresh);
  return fresh;
}

// Density is microtubules per um^2 of the cell's own footprint area (not per
// cell, and not tied to semiMajor the way the old placeholder was) --
// fractional expected counts are resolved via a per-cell hash draw
// (probabilistic rounding) so low densities don't band every cell to the same
// integer count.
function mtCountForCell(seed, cx, cy, cell, p, geom) {
  const g = geom || getMtCellGeometry(cell);
  const expected = Math.max(0, g.areaUm2 * p.mtDensity);
  const base = Math.floor(expected);
  const frac = expected - base;
  const roll = hashUnit(seed, cx, cy, MT_CH_COUNT);
  return base + (roll < frac ? 1 : 0);
}

// Pushes `pt` (mutated in place) back inside the cell's own cytoplasm volume
// if the free/wobbled walk carried it out -- the whole microtubule has to
// stay inside the cell, not just its (already-valid-by-construction)
// endpoints. Three checks, in order:
//  1. Out of the NUCLEUS (a real 3D ellipsoid, not just its 2D footprint --
//     rotated into the nucleus's own local frame, then pushed back along the
//     radial direction in that normalized ellipsoid space if inside).
//  2. Inside the cell's own blobby FOOTPRINT (radial clamp against
//     cellRadiusAt at this point's own azimuth).
//  3. Between 0 and the cytoplasm HEIGHT-FIELD at this (possibly
//     footprint-clamped) xy position.
// A hard clamp, not a physically-motivated bounce/reflection -- simple,
// always exactly satisfies containment, and reads as the microtubule
// bending along the nuclear envelope/cell cortex when wobble would have
// carried it through, a reasonable stand-in for a real prototype.
// knownInside: the caller has just checked this point against the outline
// (the cut in buildMicrotubule); skip that check again unless the nucleus
// push moves it (speed only).
function mtClampIntoCytoplasm(cell, p, geom, pt, knownInside = false) {
  if (cell.nucShaped) {
    // The same radial push in the shaped nucleus's own coordinates (index.html, nucPushOutLocal).
    if (nucPushOutLocal(cell, pt, MT_CONTAIN_MARGIN)) knownInside = false;
  } else {
    const dxN = pt.x - cell.nucOffX, dyN = pt.y - cell.nucOffY;
    const cr = Math.cos(-cell.nucRot), sr = Math.sin(-cell.nucRot);
    const lx = dxN * cr - dyN * sr, ly = dxN * sr + dyN * cr;
    const a = cell.nucLong / 2, b = cell.nucShort / 2, rz = cell.nucHeight / 2;
    const nz = (pt.z - cell.nucZ) / Math.max(1e-6, rz);
    const nx = lx / Math.max(1e-6, a), ny = ly / Math.max(1e-6, b);
    const ellNorm = Math.sqrt(nx * nx + ny * ny + nz * nz);
    if (ellNorm < 1) {
      // Pushing OUT of the nucleus needs the normalized radius to end up
      // GREATER than 1 (outside), so the margin divides rather than multiplies
      // here -- the opposite direction from the footprint/height clamps below,
      // which pull a point back INSIDE their own outer bound.
      const scale = (ellNorm > 1e-9 ? 1 / ellNorm : 1) / MT_CONTAIN_MARGIN;
      const lx2 = lx * scale, ly2 = ly * scale;
      pt.z = cell.nucZ + nz * rz * scale;
      const cr2 = Math.cos(cell.nucRot), sr2 = Math.sin(cell.nucRot);
      pt.x = cell.nucOffX + lx2 * cr2 - ly2 * sr2;
      pt.y = cell.nucOffY + lx2 * sr2 + ly2 * cr2;
      knownInside = false;
    }
  }

  const dist = Math.hypot(pt.x, pt.y);
  const rc = knownInside ? dist : cellRadiusAt(cell, Math.atan2(pt.y, pt.x));
  if (dist > rc) {
    const scale = (rc * MT_CONTAIN_MARGIN) / Math.max(1e-9, dist);
    pt.x *= scale; pt.y *= scale;
  }

  // Against the ACTUAL rendered (relaxed) height, not the raw analytic
  // cytoHeightAt() -- see sampleCytoMeshHeight's own comment in index.html:
  // the relaxation lowers the sharp nucleus-adjacent dome peak below what
  // cytoHeightAt() alone would return, so clamping
  // against the raw function let a point sit above the surface actually
  // drawn (a real, reported "pokes out of the dome" bug).
  const topH = Math.max(0, sampleCytoMeshHeight(cell, p, pt.x, pt.y));
  if (pt.z < 0) pt.z = 0;
  else if (pt.z > topH) pt.z = topH * MT_CONTAIN_MARGIN;
}

// Builds one microtubule's full geometry: a start in the cytoplasm near the
// nucleus (mtSampleStart), an end near the cell edge picked by its direction
// from the start (mtPickEnd), and a free correlated-random-walk path in XY
// (real loops/U-bends allowed) between them, corrected to land exactly on both
// endpoints. z comes from the fraction-of-local-ceiling model (see the
// per-point loop below), kept over or under the nucleus by mtNucleusEnvelope,
// and the whole path is then clamped to stay inside the cell's own cytoplasm
// volume throughout. `resampleRound` (0 = first attempt) reseeds the whole
// draw sequence when the collision pass below needs to regenerate this
// microtubule from scratch.
function mtGenerateOne(seed, cx, cy, mtIndex, resampleRound, cell, p, geom) {
  const next = hashStream(seed, cx, cy, MT_STREAM_BASE + mtIndex + resampleRound * MT_RESAMPLE_SPACING);

  const S = mtSampleStart(cell, p, geom, next);
  const E = mtPickEnd(cell, p, geom, next, S);
  const startX = S.x, startY = S.y, endX = E.x, endY = E.y;

  // z is tracked as a FRACTION of the LOCAL cytoplasm ceiling (0 = floor,
  // 1 = the actual rendered height right at that (x,y)), not an absolute
  // height driven by a fixed-rate linear walk -- the old approach decreased z
  // at a constant rate per path-STEP while the true ceiling (sampleCytoMeshHeight)
  // stays near full nucleus height for a while, then drops over a short band,
  // then goes flat at the rim for the remaining, typically much longer,
  // distance -- so a path's own z was usually still near its starting value
  // once (x,y) had already crossed that drop-off band, forcing
  // mtClampIntoCytoplasm to snap it down hard and hold it near the floor for
  // the rest of the path (a real, reported "hard snap, not a slope"
  // artifact). Interpolating a FRACTION and re-deriving z from the LOCAL
  // ceiling at every point instead means z always rides whatever slope is
  // actually there, by construction.
  const startTopH = Math.max(0, sampleCytoMeshHeight(cell, p, startX, startY));
  const fracStart = startTopH > 1e-9 ? Math.min(1, Math.max(0, S.z / startTopH)) : 0;
  const fracEnd = next(); // endZ = fracEnd * (local ceiling at endX,endY) -- see the per-point loop below

  const dx = endX - startX, dy = endY - startY;
  const straightLen = Math.hypot(dx, dy);
  const turnMag = Math.max(0, p.mtWobbleTurn);
  // Path length BUDGET -- how far the walk is allowed to wander before it
  // has to arrive -- deliberately larger than the straight-line distance
  // (settable multiplier) so there's real room for loops/U-bends, not just a
  // perturbed straight line. Skipped entirely at turnMag=0 (no wobble asked
  // for): with a constant heading the raw walk below is already dead
  // straight, and budgeting extra length there would just make it overshoot
  // in the initial direction and bend backwards to correct, a worse-looking
  // result than simply not padding the length at all.
  const pathBudget = turnMag > 0 ? Math.max(straightLen, straightLen * Math.max(1, p.mtWobbleFactor)) : straightLen;
  // Capped so a small mtStepLen combined with a large mtWobbleFactor can't
  // blow a single microtubule up to an unbounded point count on its own
  // (independent of mtDensity/mtMinSeparation, which the collision-pass caps
  // above already cover) -- stepLen widens past what the slider asked for
  // once the cap binds, a graceful degradation rather than a hang.
  const steps = Math.min(MT_MAX_STEPS_PER_MT, Math.max(4, Math.round(pathBudget / Math.max(0.02, p.mtStepLen))));
  const stepLen = pathBudget / steps;

  // Free 2D correlated random walk in XY only (a persistent random walk: each
  // step's heading is the OLD heading nudged by a random perpendicular kick
  // of magnitude turnMag, then renormalized), started pointing at the target
  // but otherwise completely untethered from it -- this is what actually
  // allows real U-bends/loops in the lateral wandering that's actually
  // visible in the oblique view (a heading-based perpendicular-offset
  // wobble, tried first, can only ever deviate sideways from a straight line
  // and can never double back). z no longer rides along with this walk --
  // see the fraction-of-local-ceiling model above -- so there's no need for
  // the 3D cross-product basis (mtCross3/mtNormalize3) here, just the single
  // perpendicular direction a 2D heading has. Tracked relative to the start
  // (raw[0] = origin) so the drift-correction below can be a simple, exact
  // vector subtraction.
  //
  // `smoothLp` is a PERSISTENCE LENGTH (um, `p.mtSmoothLen`, "Path
  // smoothing") shared by this walk's own heading noise and z's fraction
  // noise below -- floored at stepLen (the slider's own minimum is now 1 um,
  // comfortably above any realistic Step length, so this floor is mostly a
  // safety net rather than something the slider's own minimum normally hits)
  // so smoothLp never drops below stepLen, `headingKickScale` never exceeds
  // 1, i.e. never an ever-shrinking-stepLen blow-up. Applying it here (not
  // just to z) is the fix for a real,
  // reported asymmetry: at a SMALLER Step length (more, smaller steps over
  // the same real distance), a flat per-step kick magnitude -- independent
  // of how physically long each step is -- makes the path accumulate MORE
  // total wander over a fixed real distance (each step contributes a fresh,
  // same-size kick; more steps per um means more kicks per um). Scaling the
  // kick's own magnitude by sqrt(stepLen/smoothLp) is the standard
  // worm-like-chain relation (tangent-angle variance grows linearly with arc
  // length, i.e. per-step variance must scale with stepLen for the total to
  // come out step-length-INVARIANT over a fixed real distance) -- this is
  // what actually keeps "how wiggly per um" constant regardless of Step
  // length, rather than just capping the worst case (below).
  const smoothLp = Math.max(stepLen, p.mtSmoothLen);
  const headingKickScale = Math.sqrt(stepLen / smoothLp);
  // Minimum radius of curvature (`p.mtMinTurnRadius`, "Min turn radius", um
  // -- 0 disables it): heading_new = normalize(heading_old + kick*perp)
  // rotates heading by exactly atan2(kick, 1) -- unbounded as the kick
  // grows, which at a high Wobble turn strength let a single step bend by
  // tens of degrees, several such steps in a row (a real possibility, not
  // just a tail-risk edge case, since a fresh kick is drawn every step)
  // producing an implausibly tight hairpin/near-180 kink (a real, reported
  // artifact) with no relation to how far the step actually moved. Recast as
  // an explicit ROTATION by that same angle (mathematically identical to the
  // old vector-add-then-renormalize for any UNCLAMPED angle -- heading/perp
  // are orthonormal, so the new vector's angle off heading is exactly
  // atan2(kick,1) either way) makes the angle a first-class value that CAN
  // be clamped: `stepTurnCap`, this path's own per-step ceiling, is derived
  // from the minimum radius via the chord-angle relation for a step of
  // length stepLen -- expressed as a radius (um) rather than a flat
  // degrees-per-step limit specifically so it stays a genuine geometric
  // floor regardless of Step length. Deliberately independent of
  // turnMag/mtWobbleTurn, per the report asking for a curvature floor
  // "without changing wobble turn strength" -- turnMag still controls how
  // OFTEN/how close to the cap a path turns; this only ever pulls in the
  // tail where an uncapped kick would have exceeded it.
  //
  // THIS CAP ON ITS OWN IS NOT ENOUGH, measured directly: ~16% of a real
  // run's interior turns still violated the intended radius, some down to
  // ~0.001 um, i.e. a near-total reversal -- because it only bounds the RAW
  // walk's own curvature, and the Brownian-bridge drift correction added
  // below is a SEPARATE, additive, fixed-direction pull with no curvature
  // bound of its own; the REALIZED point-to-point direction (raw step +
  // drift increment, which grows from 0 at the start to its full value at
  // the end) can still bend sharply wherever the two don't point the same
  // way, however gently the raw walk itself curves. `mtEnforceMinTurnRadius`
  // (below, run on the FINAL rendered x,y once drift is already added) is
  // the fix that actually catches this -- this raw-walk cap is kept anyway
  // as a cheap first line of defence that reduces how much work that pass
  // has to do.
  const stepTurnCap = p.mtMinTurnRadius > 0 ? 2 * Math.asin(Math.min(1, stepLen / (2 * p.mtMinTurnRadius))) : Math.PI;
  let heading = straightLen > 1e-9 ? [dx / straightLen, dy / straightLen] : [1, 0];
  const raw = [[0, 0]];
  for (let i = 1; i <= steps; i++) {
    if (turnMag > 0) {
      const n1 = next() * 2 - 1;
      let turnAngle = Math.atan2(turnMag * headingKickScale * n1, 1);
      if (turnAngle > stepTurnCap) turnAngle = stepTurnCap;
      else if (turnAngle < -stepTurnCap) turnAngle = -stepTurnCap;
      const cosT = Math.cos(turnAngle), sinT = Math.sin(turnAngle);
      heading = [heading[0] * cosT - heading[1] * sinT, heading[0] * sinT + heading[1] * cosT];
    }
    const prev = raw[i - 1];
    raw.push([prev[0] + heading[0] * stepLen, prev[1] + heading[1] * stepLen]);
  }

  // Brownian-bridge-style correction: the free walk above generally does NOT
  // land exactly on (dx,dy), so distribute the shortfall across every point
  // via a smoothstep taper (0 at the start, 1 at the end) -- a smooth,
  // low-frequency correction that pins both endpoints EXACTLY without
  // fighting the walk's own local wiggles/loops, the same "de-trend a free
  // random walk" construction a Brownian bridge uses.
  const driftX = dx - raw[steps][0], driftY = dy - raw[steps][1];

  // Finalize xy BEFORE computing z (which needs to sample the ceiling/
  // nucleus footprint at the path's own ACTUAL, post-correction position,
  // not a soon-to-be-adjusted one) -- build the plain x,y first, run the
  // minimum-turn-radius corrector on them, then compute z from the result.
  const ptsXY = new Array(steps + 1);
  for (let i = 0; i <= steps; i++) {
    const t = i / steps;
    const s = t * t * (3 - 2 * t); // smoothstep
    ptsXY[i] = { x: startX + raw[i][0] + driftX * s, y: startY + raw[i][1] + driftY * s };
  }
  mtEnforceMinTurnRadius(ptsXY, p.mtMinTurnRadius);

  // Fraction noise: a small persistent-random-walk scalar process (same idea
  // as the heading kick above, just applied to a scalar instead of a
  // direction), tapered to 0 at both ends (sin(pi*t), 0 at t=0 and t=1) so it
  // never disturbs the exact fracStart/fracEnd endpoints -- gives the
  // interior a bit of natural roughness without losing the slope-following
  // behaviour the interpolation itself provides. Amplitude scaled off the
  // existing mtWobbleTurn slider (down-weighted) rather than a new one.
  //
  // Its CORRELATION LENGTH is the SAME `smoothLp` persistence length the
  // heading kick above now uses (both ultimately driven by the one Path
  // smoothing slider) -- fixing a real, reported asymmetry: z used to be a
  // flat AR(1) with a fixed 0.8-per-STEP coefficient, decorrelating after a
  // roughly constant NUMBER of steps regardless of how physically long each
  // step was, so at a small Step length (more steps per um) the same
  // few-step correlation window covered much less real distance and z
  // visibly got MORE jagged per um exactly where a smaller Step length was
  // chosen to look more realistic -- while xy's heading walk, despite having
  // the same "redraws every step" shape, looked far less affected, because
  // (before this round) its minimum-curvature clamp already tied a good
  // share of ordinary steps to stepLen via the chord-angle relation; z had
  // no equivalent tie at all. Reparametrized as a proper discretized
  // Ornstein-Uhlenbeck process in ARC LENGTH: decay = exp(-stepLen/corrLen)
  // is the exact relation that keeps the process's correlation length fixed
  // in real um regardless of how many steps a given physical distance is
  // chopped into, and the innovation's own sqrt(1-decay^2) scaling is the
  // standard OU identity that keeps the STATIONARY variance constant
  // regardless of corrLen/stepLen too -- so Path smoothing controls only how
  // quickly the noise wanders (frequency), never how far it wanders
  // (amplitude, still fracNoiseAmp). If corrLen ever drops to stepLen (the
  // floor this shares with headingKickScale above -- not reachable via the
  // slider's own current minimum of 1 um for any realistic Step length, but
  // still a safe floor for a directly-scripted config), decay = exp(-1) =~
  // 0.37 per step -- a deliberately modest, not zero, floor (a flat
  // white-noise reset every single step reads as static, not roughness) that
  // still redraws fast enough to look like "smoothing off".
  const fracNoiseAmp = 0.15 * turnMag;
  const fracNoiseDecay = Math.exp(-stepLen / smoothLp);
  const fracNoiseInnovScale = Math.sqrt(Math.max(0, 1 - fracNoiseDecay * fracNoiseDecay));
  let fracNoise = 0;

  // z without the nucleus: the fraction profile (start and end fractions, the noise above), then box-smoothed
  // (window from Path smoothing, in points of this path's own stepLen; endpoints kept).
  const pts = new Array(steps + 1);
  const ceil = new Array(steps + 1);
  for (let i = 0; i <= steps; i++) {
    const t = i / steps;
    const s = t * t * (3 - 2 * t); // smoothstep
    const x = ptsXY[i].x, y = ptsXY[i].y;
    ceil[i] = Math.max(0, sampleCytoMeshHeight(cell, p, x, y));
    let frac;
    if (i === 0) frac = fracStart;
    else if (i === steps) frac = fracEnd;
    else {
      fracNoise = fracNoiseDecay * fracNoise + fracNoiseAmp * fracNoiseInnovScale * (next() * 2 - 1);
      frac = Math.min(1, Math.max(0, lerp(fracStart, fracEnd, s) + fracNoise * Math.sin(Math.PI * t)));
    }
    pts[i] = { x, y, z: frac * ceil[i] };
  }
  const smoothWinPts = Math.max(0, Math.round(p.mtSmoothLen / stepLen));
  if (smoothWinPts > 0) mtBoxSmooth(pts.map(pt => pt.z), Math.max(1, Math.round(smoothWinPts / 2)), (i, v) => { pts[i].z = v; });

  // Over or under the nucleus: wherever (x, y) is over the nucleus footprint, z must clear its column (top +
  // clearance when going over, bottom - clearance when going under), and everywhere z stays under the cytoplasm
  // ceiling. One side per path, from where it starts (above or below the nucleus's widest section), so it never
  // flip-flops. mtNucleusEnvelope lifts/lowers z by a smooth ramp, so a path climbs over the nucleus well before
  // the rim instead of being pushed up point by point at it.
  const nucBottomZ = cell.nucZ - cell.nucDown;   // nucUp/nucDown: nucHeight/2, or the shaped nucleus's extents
  const nucClearance = mtNucleusClearance(p);
  const goOverNucleus = nucBottomZ - nucClearance <= 0 || pts[0].z >= cell.nucZ;
  // The clearance fades in over the first MT_NUC_CLEAR_RAMP_UM (xy arc): a start next to the nucleus (closer than
  // the clearance) leaves it gradually instead of jumping to the full clearance at the first step.
  const lo = new Array(steps + 1).fill(-Infinity), hi = new Array(steps + 1);
  let arc = 0;
  for (let i = 0; i <= steps; i++) {
    if (i > 0) arc += Math.hypot(pts[i].x - pts[i - 1].x, pts[i].y - pts[i - 1].y);
    hi[i] = ceil[i] * MT_CONTAIN_MARGIN;
    const col = nucleusColumnLocal(cell, pts[i].x, pts[i].y);
    if (!col) continue;
    const clr = nucClearance * Math.min(1, arc / MT_NUC_CLEAR_RAMP_UM);
    if (goOverNucleus) lo[i] = Math.min(hi[i], cell.nucZ + col[1] + clr);
    else {
      const bottom = cell.nucZ - col[0];
      hi[i] = Math.min(hi[i], bottom - Math.min(clr, 0.5 * bottom));
    }
  }
  mtNucleusEnvelope(pts, lo, hi, Math.max(1, Math.round(0.5 * p.mtSmoothLen / stepLen)));

  // Truncate at the cell's own OUTER EDGE rather than clamp-and-continue past
  // it. The free walk above has genuine excursions beyond the footprint
  // (more of them the larger Wobble path length/turn strength are set), and
  // the old behaviour -- radially clamping every such point back onto the
  // boundary via mtClampIntoCytoplasm, then letting the walk continue -- let
  // a single microtubule cross the boundary many times, each crossing pulled
  // onto almost exactly the same rim radius: a real, reported "microtubules
  // hugging the outline" artifact, a dense traced ring right at the cell
  // edge that isn't a real cytoskeletal structure. A growing microtubule
  // reaching the cortex is more faithfully represented as simply ending
  // there than as bouncing along it. Scans forward from the start (built to
  // be interior, on/near the nucleus surface) so the FIRST exit ends the
  // path outright -- once truncated, nothing further is generated, so there
  // is no way for a second or third crossing to build up a hugging run. Does
  // NOT apply to the nucleus boundary (still a push-back clamp, below) or the
  // height ceiling (still a clamp, since the fraction-of-local-ceiling z
  // model already keeps height close to right by construction) -- this is
  // specifically about the outer footprint, the one boundary the free walk
  // can wander back and forth across on its own.
  let cutLen = pts.length;
  for (let i = 0; i < pts.length; i++) {
    const ang = Math.atan2(pts[i].y, pts[i].x);
    if (Math.hypot(pts[i].x, pts[i].y) > cellRadiusAt(cell, ang)) { cutLen = i; break; }
  }
  pts.length = cutLen;

  // The SURVIVING points -- now guaranteed within the footprint by the
  // truncation above -- must still stay outside the nucleus and below the
  // cytoplasm height-field; the fraction-of-local-ceiling z already keeps
  // height close to right by construction, but this (and the collision-nudge
  // pass elsewhere in this file, which moves points after generation) still
  // need it as a safety net. mtClampIntoCytoplasm's own footprint clamp is a
  // no-op here in the ordinary case (nothing left in `pts` violates it) --
  // kept as-is rather than split out, since the collision-nudge pass still
  // needs the full three-way clamp for points it moves after this point.
  for (let i = 0; i < pts.length; i++) mtClampIntoCytoplasm(cell, p, geom, pts[i], true);

  return { pts, priority: next() };
}

// Spatial-hash grid over every point of every microtubule in the cell, bucket
// size = minSep. Two points further apart than minSep in every axis can never
// be within minSep of each other, so checking a point against only its own
// bucket + the 26 neighbouring buckets (searchNeighbors below) is guaranteed
// to find every real conflict -- turning what used to be an O(M^2*S^2)
// all-pairs scan into an O(N) one (N = total points in the cell). This is the
// difference between "fine for a handful of microtubules" and actually
// working at the up-to-2/um^2 density ceiling (mtDensity), which can put
// thousands of points in a single cell -- a real, measured hang (Chromium's
// tab froze mid-drag) at that combination with the original all-pairs
// version is why this exists.
// Plain integer spatial-hash key (Teschner et al.'s constants) rather than a
// string ("ix,iy,iz") -- a numeric key lets the grid be a Map<number,array>
// instead of Map<string,array>, which V8 hashes/compares meaningfully faster
// than a freshly-concatenated string, and this function is called N times
// per round, N being the whole point of this file's own performance pass. A
// hash COLLISION (two different buckets landing on the same key) is
// harmless here -- it only costs a few extra real-distance checks against
// points that turn out not to be nearby, never an incorrect result.
function mtBucketKey(ix, iy, iz) {
  return (Math.imul(ix, 73856093) ^ Math.imul(iy, 19349663) ^ Math.imul(iz, 83492791)) >>> 0;
}
function mtBuildSpatialIndex(mts, cellSize) {
  const grid = new Map();
  for (let i = 0; i < mts.length; i++) {
    const pts = mts[i].pts;
    for (let j = 0; j < pts.length; j++) {
      const pt = pts[j];
      const key = mtBucketKey(Math.floor(pt.x / cellSize), Math.floor(pt.y / cellSize), Math.floor(pt.z / cellSize));
      let arr = grid.get(key);
      if (!arr) { arr = []; grid.set(key, arr); }
      arr.push(i, j); // endpoints (j=0 or pts.length-1) are included as obstacles too, just never the point that gets moved (see the caller's own loop bounds)
    }
  }
  return grid;
}

// One Jacobi-style round (same "every point updates from a single shared
// snapshot" idea as index.html's own relax() for cell packing): scans every
// point once via the grid, accumulates a push-apart vector for each
// NON-endpoint point from every other-microtubule point within minSep found
// in its own 3x3x3 bucket neighbourhood, then applies every push at once and
// re-clamps into the cell volume. Returns false once no point had a
// violation (converged).
//
// The grid turns the TOTAL cost into O(N), but a single bucket can still
// hold many points if a lot of microtubules happen to pass close together
// (routine right near the nucleus, where every path's start end is confined
// to a thin shell by mtStartDecayUm) -- checking all of THOSE against each
// other is locally O(bucket^2), the same blow-up the grid exists to avoid,
// just scoped to one crowded neighbourhood instead of the whole cell.
// MT_COLLISION_OP_BUDGET bails out of the CURRENT round once total pairwise
// checks cross a fixed ceiling, same "just cap it" idiom as CHUNK_CAP/
// RENDER_CAP elsewhere in index.html -- a real, measured hang (Chromium
// froze) at mtDensity's own new 2/um^2 ceiling combined with a small
// mtMinSeparation is why this exists, not a hypothetical.
function mtNudgeRoundGrid(cell, p, geom, mts, minSep) {
  const cellSize = Math.max(minSep, 1e-6);
  const grid = mtBuildSpatialIndex(mts, cellSize);
  const pushes = mts.map(m => m.pts.map(() => ({ x: 0, y: 0, z: 0 })));
  let any = false, ops = 0;
  scan: for (let i = 0; i < mts.length; i++) {
    const pts = mts[i].pts;
    for (let j = 1; j < pts.length - 1; j++) { // endpoints never move, so never need a push accumulated
      const pt = pts[j];
      const ix = Math.floor(pt.x / cellSize), iy = Math.floor(pt.y / cellSize), iz = Math.floor(pt.z / cellSize);
      for (let dx = -1; dx <= 1; dx++) for (let dy = -1; dy <= 1; dy++) for (let dz = -1; dz <= 1; dz++) {
        const arr = grid.get(mtBucketKey(ix + dx, iy + dy, iz + dz));
        if (!arr) continue;
        for (let k = 0; k < arr.length; k += 2) {
          const i2 = arr[k], j2 = arr[k + 1];
          if (i2 === i) continue; // same microtubule is never a conflict target
          if (++ops > MT_COLLISION_OP_BUDGET) break scan;
          const p2 = mts[i2].pts[j2];
          const ddx = pt.x - p2.x, ddy = pt.y - p2.y, ddz = pt.z - p2.z;
          const d = Math.hypot(ddx, ddy, ddz);
          if (d >= minSep) continue;
          any = true;
          let nx, ny, nz;
          if (d < 1e-6) { nx = 1; ny = 0; nz = 0; } else { nx = ddx / d; ny = ddy / d; nz = ddz / d; }
          const mag = (minSep - d) / 2 + 1e-4;
          pushes[i][j].x += nx * mag; pushes[i][j].y += ny * mag; pushes[i][j].z += nz * mag;
        }
      }
    }
  }
  if (!any) return false;
  // Apply whatever was found even if the budget cut the scan short -- a
  // partial round still makes progress; returning `true` below (never
  // `false` on a budget-cut round) means the caller keeps calling this until
  // MT_NUDGE_ROUNDS runs out rather than mistaking a cut-short round for
  // convergence.
  for (let i = 0; i < mts.length; i++) {
    const pts = mts[i].pts;
    for (let j = 1; j < pts.length - 1; j++) {
      const push = pushes[i][j];
      if (push.x === 0 && push.y === 0 && push.z === 0) continue;
      pts[j].x += push.x; pts[j].y += push.y; pts[j].z += push.z;
      mtClampIntoCytoplasm(cell, p, geom, pts[j]);
    }
  }
  return true;
}

// Step 7: within ONE cell's own microtubule set only (each starts near its
// own nucleus and ends near its own cell edge, so it never leaves that cell's
// territory -- no cross-chunk/spatial-hash checking needed, just within-cell).
// Iteratively nudges apart (grid-accelerated, see mtNudgeRoundGrid) any pair
// closer than mtMinSeparation in 3D (a purely-XY pair separated in Z is not a
// violation -- "ok to overlap in 2D" per the design). After MT_NUDGE_ROUNDS,
// any microtubule that STILL has a violating point gets its lower-priority
// member regenerated from scratch (a fresh hash-stream attempt) -- bounded to
// MT_RESAMPLE_ROUNDS so a pathological configuration can't loop indefinitely;
// a residual violation past that is a documented best-effort limit, same
// spirit as index.html's own prune() PRUNE_ROUNDS cap.
function mtResolveCollisions(seed, cx, cy, cell, p, mts, geom) {
  const minSep = Math.max(0, p.mtMinSeparation);
  if (minSep <= 0 || mts.length < 2) return;
  let totalPoints = 0;
  for (const m of mts) totalPoints += m.pts.length;
  if (totalPoints > MT_COLLISION_MAX_TOTAL_POINTS) return; // see MT_COLLISION_MAX_TOTAL_POINTS's own comment

  function nudgePass(rounds) {
    for (let round = 0; round < rounds; round++) {
      if (!mtNudgeRoundGrid(cell, p, geom, mts, minSep)) return true;
    }
    return false;
  }

  if (nudgePass(MT_NUDGE_ROUNDS)) return;

  for (let resampleRound = 0; resampleRound < MT_RESAMPLE_ROUNDS; resampleRound++) {
    // One more grid pass, used only to find which microtubules still have a
    // violating point (not to move anything yet) -- same O(N) scan mtNudgeRoundGrid
    // already does, just reporting per-microtubule instead of applying a push.
    const cellSize = Math.max(minSep, 1e-6);
    const grid = mtBuildSpatialIndex(mts, cellSize);
    const violating = new Set();
    for (let i = 0; i < mts.length; i++) {
      const pts = mts[i].pts;
      outer: for (let j = 0; j < pts.length; j++) {
        const pt = pts[j];
        const ix = Math.floor(pt.x / cellSize), iy = Math.floor(pt.y / cellSize), iz = Math.floor(pt.z / cellSize);
        for (let dx = -1; dx <= 1; dx++) for (let dy = -1; dy <= 1; dy++) for (let dz = -1; dz <= 1; dz++) {
          const arr = grid.get(mtBucketKey(ix + dx, iy + dy, iz + dz));
          if (!arr) continue;
          for (let k = 0; k < arr.length; k += 2) {
            const i2 = arr[k], j2 = arr[k + 1];
            if (i2 === i) continue;
            const p2 = mts[i2].pts[j2];
            if (Math.hypot(pt.x - p2.x, pt.y - p2.y, pt.z - p2.z) < minSep) { violating.add(i); break outer; }
          }
        }
      }
    }
    if (violating.size === 0) return;
    // Resample the lower half (by priority) of the still-violating set --
    // regenerating every violator at once risks two violators swapping
    // places forever; only ever replacing the losing half converges.
    const sorted = [...violating].sort((a, b) => mts[a].priority - mts[b].priority);
    const loserCount = Math.max(1, Math.ceil(sorted.length / 2));
    for (let k = 0; k < loserCount; k++) {
      const idx = sorted[k];
      mts[idx] = mtGenerateOne(seed, cx, cy, idx, resampleRound + 1, cell, p, geom);
    }
    if (nudgePass(MT_POST_RESAMPLE_NUDGE_ROUNDS)) return;
  }
  // Still-violating microtubules after this point are left as-is --
  // best-effort, see this function's own header comment.
}

// Final-result cache: the same "recomputed from scratch every draw() call
// made panning/dragging incredibly slow" lesson index.html's own cytoCache
// comment documents applies even more here -- generation plus the O(M^2*S^2)
// collision-resolution pass is real work, and draw() calls buildMicrotubulesForCell
// for every visible cell on every single pointermove/wheel frame, not just
// once per parameter change. Keyed on chunk id, invalidated by a signature
// over the seed, the cell's own resolved shape (mtCellShapeSig) and every
// mt*-prefixed control -- a pan/zoom/tilt drag that doesn't touch any of
// those hits this cache for every already-seen chunk, exactly like
// getCytoGeometry does for the cell body/nucleus mesh.
const MT_RESULT_CACHE_MAX = 6000;
const mtResultCache = new Map();
function mtResultSig(seed, cell, p) {
  // p.nucMargin isn't an mt*-prefixed control, but it feeds the cytoplasm
  // height field (via cytoHeightAt, MODULE: index.html) that
  // sampleCytoMeshHeight reads AND the over/under-nucleus clearance
  // (mtNucleusClearance) directly -- a real, previously-latent staleness gap:
  // without it here, changing nucMargin alone would silently keep serving a
  // cached microtubule set built against the old clearance/height field.
  return [seed, mtCellShapeSig(cell), p.mtDensity,
    p.mtStartDecayUm, p.mtEndDecayUm, p.mtDirKappa, p.mtWobbleTurn,
    p.mtWobbleFactor, p.mtStepLen, p.mtSmoothLen, p.mtMinTurnRadius, p.mtMinSeparation, p.mtMaxZSlope, p.nucMargin, p.cytoMaxSlope, p.cytoDomeSlope, p.cytoRelaxUm, p.cytoRings, p.cytoTheta].join('|');
}

// The one entry point index.html's draw() calls: builds every microtubule for
// one cell (already collision-resolved) and returns their point arrays
// ({x,y,z}, cell-local frame) ready for localToWorld()+project().
function buildMicrotubulesForCell(seed, cx, cy, cell, p) {
  if (!cell || !cell.present) return [];
  const key = cx + ',' + cy;
  const sig = mtResultSig(seed, cell, p);
  const cached = mtResultCache.get(key);
  if (cached && cached.sig === sig) return cached.paths;
  if (mtResultCache.size > MT_RESULT_CACHE_MAX) mtResultCache.clear();

  const geom = getMtCellGeometry(cell);
  // Capped independently of MT_COLLISION_MAX_TOTAL_POINTS -- that one only
  // protects the collision PASS; without this, a very large/blobby cell at
  // mtDensity's own 2/um^2 ceiling would still GENERATE thousands of
  // multi-hundred-point paths (and index.html's draw() would still have to
  // stroke all of them) even with collision-checking skipped.
  const count = Math.min(MT_MAX_PER_CELL, mtCountForCell(seed, cx, cy, cell, p, geom));
  let paths = [];
  if (count > 0) {
    const mts = [];
    for (let i = 0; i < count; i++) mts.push(mtGenerateOne(seed, cx, cy, i, 0, cell, p, geom));
    mtResolveCollisions(seed, cx, cy, cell, p, mts, geom);
    paths = mts.map(m => m.pts);
    // Final realized-slope safety net, run AFTER collision resolution -- see
    // mtLimitZSlopeRealized's own comment for why this has to happen here,
    // not just inside mtGenerateOne. Re-clamped afterward (same three-way
    // containment check every other z-adjusting pass in this file re-applies)
    // since pulling z toward a neighbour's own value is not guaranteed to
    // land inside THIS point's own footprint/nucleus/height bounds.
    for (const pts of paths) {
      mtTrimSteepEnds(pts, p.mtMaxZSlope);
      mtLimitZSlopeRealized(pts, p.mtMaxZSlope);
      // Every point is inside the outline from an earlier clamp (the cut, or
      // the clamp after a collision nudge); the two steps above change z only.
      for (let i = 0; i < pts.length; i++) mtClampIntoCytoplasm(cell, p, geom, pts[i], true);
    }
  }
  mtResultCache.set(key, { sig, paths });
  return paths;
}

// ---- Surface labels (nanobody/antibody + dye) --------------------------------
// Each centerline is treated as a MT_RADIUS_NM cylinder carrying the 13_3
// protofilament lattice. Per lattice site: attachment point on the surface ->
// binder tip (stalk of MT_BINDER_NM, radially outward) -> dye, displaced from
// the tip by displaceByLinker (uniform in volume, ported from webSMLM.html's
// NUP model). Full-network output can reach millions of points, so this is
// only ever called on a short window (see the debug preview in index.html).
const MT_RADIUS_NM = 12.5;
const MT_N_PROTOFILAMENTS = 13;
const MT_DIMER_NM = 8;
const MT_LATTICE_START = 3;       // 13_3 lattice: neighbouring protofilaments stagger by 3/13 dimer
const MT_BINDER_NM = 12;
const MT_LINKER_MIN_NM = 2;
const MT_LINKER_MAX_NM = 5;
const MT_LABEL_STREAM_BASE = 7777777;

function mtDisplaceByLinker(p, minNm, maxNm, rng) {
  const u = rng() * 2 - 1, phi = rng() * 2 * Math.PI, sn = Math.sqrt(1 - u * u);
  const r = Math.cbrt(Math.pow(minNm, 3) + (Math.pow(maxNm, 3) - Math.pow(minNm, 3)) * rng());
  return { a: p.a + r * sn * Math.cos(phi), b: p.b + r * sn * Math.sin(phi), c: p.c + r * u };
}

// pts: centerline [{x,y,z}] (um). opts: {startUm, lenUm, efficiency, binderNm,
// linkerMinNm, linkerMaxNm}. rng: () => [0,1). Returns [{x,y,z (dye, um),
// ax..az (attachment), bx..bz (binder tip), k (protofilament), s (nm along the
// window), att/tip/dye: {u,v,s} offsets in nm in the axis frame}].
function buildMicrotubuleLabelPoints(pts, opts, rng) {
  const o = opts || {};
  const binderNm = o.binderNm != null ? o.binderNm : MT_BINDER_NM;
  const lMin = o.linkerMinNm != null ? o.linkerMinNm : MT_LINKER_MIN_NM;
  const lMax = o.linkerMaxNm != null ? o.linkerMaxNm : MT_LINKER_MAX_NM;
  const eff = o.efficiency != null ? o.efficiency : 1;
  const n = pts.length;
  if (n < 2) return [];

  // Per-segment tangent + parallel-transported normal frame, cumulative length (um).
  const cum = [0], T = [], U = [], V = [];
  let prevU = null;
  for (let i = 0; i < n - 1; i++) {
    const dx = pts[i + 1].x - pts[i].x, dy = pts[i + 1].y - pts[i].y, dz = pts[i + 1].z - pts[i].z;
    const len = Math.hypot(dx, dy, dz);
    cum.push(cum[i] + len);
    let t = len > 1e-12 ? [dx / len, dy / len, dz / len] : (T.length ? T[i - 1] : [1, 0, 0]);
    let u;
    if (!prevU) {
      u = Math.abs(t[2]) < 0.9 ? [0, 0, 1] : [1, 0, 0];
    } else u = prevU;
    const d = u[0] * t[0] + u[1] * t[1] + u[2] * t[2];
    u = [u[0] - d * t[0], u[1] - d * t[1], u[2] - d * t[2]];
    const ul = Math.hypot(u[0], u[1], u[2]) || 1;
    u = [u[0] / ul, u[1] / ul, u[2] / ul];
    T.push(t); U.push(u);
    V.push([t[1] * u[2] - t[2] * u[1], t[2] * u[0] - t[0] * u[2], t[0] * u[1] - t[1] * u[0]]);
    prevU = u;
  }
  const total = cum[n - 1];
  const lenUm = Math.min(o.lenUm != null ? o.lenUm : 1, total);
  const startUm = o.startUm != null ? Math.min(Math.max(0, o.startUm), total - lenUm) : (total - lenUm) / 2;
  const phase = o.phase != null ? o.phase : rng() * 2 * Math.PI;
  const nm = 1e-3;
  const out = [];
  for (let k = 0; k < MT_N_PROTOFILAMENTS; k++) {
    const th = phase + k * 2 * Math.PI / MT_N_PROTOFILAMENTS;
    const ct = Math.cos(th), st = Math.sin(th);
    const off = (k * MT_LATTICE_START * MT_DIMER_NM / MT_N_PROTOFILAMENTS) % MT_DIMER_NM;
    let seg = 0;
    for (let sNm = off; sNm < lenUm * 1000 - 1e-9; sNm += MT_DIMER_NM) {
      if (rng() >= eff) continue;
      const S = startUm + sNm * nm;
      while (seg < n - 2 && cum[seg + 1] < S) seg++;
      const t = T[seg], u = U[seg], v = V[seg], f = S - cum[seg];
      const cx = pts[seg].x + t[0] * f, cy = pts[seg].y + t[1] * f, cz = pts[seg].z + t[2] * f;
      const r = [ct * u[0] + st * v[0], ct * u[1] + st * v[1], ct * u[2] + st * v[2]];
      const R = MT_RADIUS_NM * nm, B = (MT_RADIUS_NM + binderNm) * nm;
      const a = [cx + r[0] * R, cy + r[1] * R, cz + r[2] * R];
      const b = [cx + r[0] * B, cy + r[1] * B, cz + r[2] * B];
      const dye = mtDisplaceByLinker({ a: b[0], b: b[1], c: b[2] }, lMin * nm, lMax * nm, rng);
      const dv = [dye.a - cx, dye.b - cy, dye.c - cz];
      out.push({
        x: dye.a, y: dye.b, z: dye.c,
        ax: a[0], ay: a[1], az: a[2],
        bx: b[0], by: b[1], bz: b[2],
        k, s: sNm,
        att: { u: R / nm * ct, v: R / nm * st, s: sNm },
        tip: { u: B / nm * ct, v: B / nm * st, s: sNm },
        dye: {
          u: (dv[0] * u[0] + dv[1] * u[1] + dv[2] * u[2]) / nm,
          v: (dv[0] * v[0] + dv[1] * v[1] + dv[2] * v[2]) / nm,
          s: sNm + (dv[0] * t[0] + dv[1] * t[1] + dv[2] * t[2]) / nm,
        },
      });
    }
  }
  return out;
}
};
