// Fluorescence movie (issue 16): every structure's label in its mode, through the light path, onto one camera.
// The JS reference for adapter/inSiliScope/Simulation/ScopeMovie.cpp FluorescenceMovie (port: issue 16 phase 3).
//
// Per frame, in this order (the summation order of the C++):
//   1. flat background x the QE at the emission filter centre, plus every DNA-PAINT label's free-imager background
//      (a static offset: c x N_A x chamber height x pixel area x the imager's detected rate);
//   2. blinks (dSTORM, PALM, DNA-PAINT), per (structure, state) group with that group's PSF (one kernel per detected
//      emission wavelength, rounded to 2 nm) and detected photons per frame;
//   3. continuous populations -- WideField dyes (always on, exponential photon budget), PALM pre states (until
//      conversion or their own budget), the dSTORM initial ON (until pumped dark) -- each either mean-field (the
//      structure's FFT image x the exact mean photons per dye in the frame) or per dye (exact windows, through a
//      running image: a dye's PSF is added when its window starts and subtracted when it ends, 2 splats per dye instead
//      of one per frame). Mean-field while the expected emitting dyes exceed Renderer.MeanFieldDensityPerUm2 in the
//      focal slab or Renderer.MeanFieldMaxEmitters in the z range; these populations only decay, so a movie switches at
//      most once, mean-field -> per dye. Under a shaped excitation (the movie's own clock map) the mean field weights
//      each dye with its photons at its own clock (mean_field_map.js MapMeanField).
// Then ApplyNoiseChain at QE 1 (the photons are detected photons: QE(lambda) sits in each dye's detected fraction).
import { scopeKernel, scopeWorld, cellFieldEvents, cellFieldContinuous, cellFieldEventsInMap, cellFieldContinuousInMap, kernelWavelengthNm,
  driftInfo, scopePsfRequest } from './scope_movie.js';
import { bucketEventsByFrame, frameEmitters, renderPhotonImage, renderGaussian, noiseMaps, applyNoiseChain } from './render.js';
import { binnedBlinkRenderer } from './binned_blinks.js';
import { nearestZIndex, planSplat, splatRows, withHaloCut } from './psf.js';
import { meanFieldImage, meanFieldScene, renderShiftedImages } from './widefield.js';
import { driftMaxXyNm, driftFocusGrid, driftGridDzNm, driftGridWeights } from './drift.js';
import { EVENT_STATE } from './dyes.js';
import { makeExcitation, BG_FOCUS_STEP_UM } from './excitation.js';
import { movieClockMap } from './light_history.js';
import { CLOCK_DOSE, CLOCK_GLOBAL } from './clockmap.js';
import { MapMeanField, clockPairAt, blinkClockAt, blinkPhotons, epochLambdas, survey, WINDOW_ALIVE_MIN } from './mean_field_map.js';
import { STRUCTURES } from './world.js';
import { beadKernel, beadHaloFor, beadHaloHalfWidthPx, splatBead, beadRatePerSec, FIDUCIAL_TYPE_IDS } from './fiducials.js';

const MIN_DETECTED_FRACTION = 1e-4;   // a (structure, state) detected less than this is skipped (no kernel)

const gaussianSigmaPx = (lambdaNm, na, pixelNm) => Math.min(Math.max(0.21 * lambdaNm / Math.max(0.01, na) / pixelNm, 0.3), 20.0);

// One unit-photon emitter (sign +1 or -1, x its excitation factor g.excite) into img at its plane: the running image's
// add/remove (dxPx, dyPx: the sample drift).
function splatUnit(img, W, H, e, g, zStage, pixelNm, sign, dxPx = 0.0, dyPx = 0.0) {
  if (g.excite) { sign *= g.excite(e.xUm, e.yUm, e.zNm); if (sign === 0) return; }
  const xPx = e.xUm * 1000.0 / pixelNm + dxPx, yPx = e.yUm * 1000.0 / pixelNm + dyPx;
  if (!g.kernel) { renderGaussian(img, W, H, xPx, yPx, g.sigmaPx, sign); return; }
  const plan = planSplat(g.kernel, nearestZIndex(g.kernel, e.zNm / 1000.0 - zStage), xPx, yPx, 1.0, g.kernel.interpMode);
  if (plan) splatRows(img, W, H, 0, H, g.kernel, plan, sign);
}

// Mean detected photons per dye of a decaying population in [t0, t1): rate x integral of exp(-lambda t).
const meanPhotons = (ratePerSec, lambda, t0, t1) =>
  lambda > 0 ? ratePerSec * (Math.exp(-lambda * t0) - Math.exp(-lambda * t1)) / lambda : ratePerSec * (t1 - t0);

// ---- fiducial beads (fiducials.js) ----
// A component whose brightest bead gives fewer detected photons per frame than this is left out (no kernel, no splat):
// gold's laser leak behind blocking filters, a TetraSpeck dye no laser excites.
const FID_MIN_PHOTONS = 1e-3;
// A bead component takes a dye group's kernel when that kernel's wavelength is within this (< 1.5 % in PSF width), so
// beads add no PSF computation to a labelled sample; else a kernel of its own.
const FID_KERNEL_REUSE_NM = 10;
// The beads' static images (no drift): each bead's decay over the movie as a Taylor series about its middle frame,
// e^-L(tau - tm) = sum_k (-L)^k (tau - tm)^k / k!, of the lowest order D whose remainder (L_max x half the span)^(D+1) /
// (D+1)! is below FID_TAYLOR_TOL; one image per order, frame f = sum_k (tau_f - tm)^k / k! x image_k. Bleaching over
// hours makes D 1-3; a bleach faster than FID_TAYLOR_MAX_ORDER allows splats per frame.
const FID_TAYLOR_TOL = 1e-9, FID_TAYLOR_MAX_ORDER = 6;

// The beads of the movie (FiducialBeads): their sites in the query rect and z window, per component the beads with
// their rate (detected photons per second at their excitation a, before bleaching) and bleach rate (a x the
// component's), the kernel and its bead corrections. null without beads.
function fiducialBeads(spec, S, world, groups, exciteFor, keep) {
  if (!S.fid.on) return null;
  const q = S.q, pixelNm = S.p.pixelSizeNm;
  const zMin = q.zHalfRangeUm > 0 ? q.zCullCentreUm - q.zHalfRangeUm : -Infinity;
  const zMax = q.zHalfRangeUm > 0 ? q.zCullCentreUm + q.zHalfRangeUm : Infinity;
  const sites = world.fiducialsInRect(q.x0Um, q.y0Um, q.x1Um, q.y1Um).filter(b => b.z >= zMin && b.z < zMax);
  const comps = [];
  let own = 0;
  for (const c of S.fid.comps) {
    const excite = exciteFor(c.lineShares), beads = [];
    let maxRate = 0.0;
    for (const b of sites) {
      const xUm = b.x - q.originXUm, yUm = b.y - q.originYUm, zNm = (b.z - q.zRefUm) * 1000.0;
      const a = excite ? excite(xUm, yUm, zNm) : 1.0, rate = beadRatePerSec(c, b.d) * a;
      if (!(rate > 0)) continue;
      beads.push({ xUm, yUm, zNm, d: b.d, rate, lambda: c.bleachPerSec * a });
      if (rate > maxRate) maxRate = rate;
    }
    if (!beads.length || maxRate * S.expSec < FID_MIN_PHOTONS) continue;
    const lambdaNm = kernelWavelengthNm(c.lambdaNm);
    let kernel = null, best = Infinity;
    for (const g of groups) {
      const dl = Math.abs(g.lambdaNm - lambdaNm);
      if (g.kernel && dl <= FID_KERNEL_REUSE_NM && dl < best) { best = dl; kernel = g.kernel; }
    }
    if (!kernel) kernel = scopeKernel(spec, lambdaNm, null, keep + ++own);
    // The Gaussian PSF: the sphere's projection adds r^2 / 5 to the variance per axis.
    const sigmaPx = gaussianSigmaPx(lambdaNm, S.p.na, pixelNm);
    // The halo: the point PSF at the component's wavelength over a window twice the FOV, one sample per pixel.
    let halo = null;
    if (kernel) {
      const hr = scopePsfRequest(spec, lambdaNm);
      hr.oversampling = 1;
      hr.kernelHalfWidthPx = beadHaloHalfWidthPx(S.W, S.H, pixelNm, Math.trunc(kernel.halfWidthOversampled / Math.max(1, kernel.oversampling)));
      halo = beadHaloFor(hr);
    }
    comps.push({ c, beads, kernel, B: kernel ? beadKernel(kernel, pixelNm) : null, halo, sigmaPx, taylor: null, lambdaNm });
  }
  return { sites, comps, t0Sec: S.t0Sec, expSec: S.expSec, N: S.N, sec: 0.0 };
}
// One bead into img with `photons` at the stage height zf and drift (dx, dy) px.
function splatFiducial(img, W, H, F, e, zf, pixelNm, photons, dx, dy) {
  const xPx = e.xUm * 1000.0 / pixelNm + dx, yPx = e.yUm * 1000.0 / pixelNm + dy;
  if (!F.B) {
    const rPx = e.d / 2.0 / pixelNm;
    renderGaussian(img, W, H, xPx, yPx, Math.sqrt(F.sigmaPx * F.sigmaPx + rPx * rPx / 5.0), photons);
    return;
  }
  splatBead(img, W, H, F.B, nearestZIndex(F.kernel, e.zNm / 1000.0 - zf), e.d, xPx, yPx, photons, F.halo);
}
// The beads' photons of frame f into img: per component, each bead's rate x the integral of its survival e^-L t over the
// frame [t0 + f exp, t0 + (f + 1) exp) (meanPhotons). Without drift through the Taylor images (built at the first
// frame), else splatted one by one.
function renderFiducials(img, W, H, FB, f, zf, pixelNm, drift, dx, dy) {
  const t0 = FB.t0Sec, exp = FB.expSec, tm = (FB.N - 1) * exp / 2;
  for (const F of FB.comps) {
    if (!drift && F.taylor === null) {
      let lMax = 0.0;
      for (const e of F.beads) lMax = Math.max(lMax, e.lambda);
      const x = lMax * tm;
      let D = 0, rem = x;   // rem = x^(D+1) / (D+1)!
      while (rem > FID_TAYLOR_TOL && D < FID_TAYLOR_MAX_ORDER) { D++; rem *= x / (D + 1); }
      F.taylor = rem > FID_TAYLOR_TOL ? false : [];
      for (let k = 0; F.taylor && k <= D; k++) {
        const image = new Float32Array(W * H);
        for (const e of F.beads) {
          // rate x e^-L (t0 + tm) x the frame integral's factor (1 - e^-L exp) / L, x (-L)^k.
          const q = e.lambda > 0 ? (1.0 - Math.exp(-e.lambda * exp)) / e.lambda : exp;
          const w = e.rate * Math.exp(-e.lambda * (t0 + tm)) * q * Math.pow(-e.lambda, k);
          if (w !== 0.0) splatFiducial(image, W, H, F, e, zf, pixelNm, w, 0.0, 0.0);
        }
        F.taylor.push(image);
      }
    }
    if (!drift && F.taylor) {
      const u = f * exp - tm;
      let c = 1.0;
      for (let k = 0; k < F.taylor.length; k++) {
        if (k > 0) c = c * u / k;
        const m = Math.fround(c), image = F.taylor[k];
        for (let i = 0; i < img.length; i++) if (image[i] !== 0) img[i] += Math.fround(m * image[i]);
      }
      continue;
    }
    const tf = t0 + f * exp;
    for (const e of F.beads) splatFiducial(img, W, H, F, e, zf, pixelNm, meanPhotons(e.rate, e.lambda, tf, tf + exp), dx, dy);
  }
}

export function renderFluorescenceMovie(P, spec, S, onFrame, opts = {}) {
  const t0 = performance.now();
  const W = S.W, H = S.H, N = S.N, pixelNm = S.p.pixelSizeNm, um = pixelNm / 1000, zStage = S.O('z');
  const world = scopeWorld(P, S);
  // The excitation factor of a dye state at (xUm, yUm, zNm) in the FOV frame (excitation.js; the axis at the FOV
  // centre, z = height above the coverslip); null when it is 1 everywhere (a Flat beam at the BFP centre).
  const EX = S.excite, axisX = (W - 1) / 2 * um, axisY = (H - 1) / 2 * um, zRef = S.q.zRefUm;
  const exciteFor = shares => EX.uniform ? null : (x, y, zNm) => EX.at(x - axisX, y - axisY, zNm / 1000.0 + zRef, shares);
  // Drift: the sample moved by d, the focal plane dz lower in it (zAt); mean-field images on the drift's focus grid,
  // the lit square and grid grown by the xy drift (DriftMarginUm).
  const drift = S.driftOn;
  const zAt = f => drift ? zStage - S.drift[f].z / 1000.0 : zStage;
  const dPx = f => drift ? [S.drift[f].x / pixelNm, S.drift[f].y / pixelNm] : [0.0, 0.0];
  const driftMarginUm = drift ? (Math.ceil(driftMaxXyNm(S.driftBounds) / pixelNm) + 1.0) * pixelNm / 1000.0 : 0.0;
  const driftGrid = drift ? driftFocusGrid(S.driftBounds) : null;

  // ---- groups: (structure, state) with their PSF and detected photons per frame ----
  const groups = [];
  const groupOf = new Map();   // `${s},${'main'|'pre'}` -> group
  const wanted = [];
  // Only the mounted specimen's labels (S.mounted): another specimen's structures have no sites in this world.
  S.labels.forEach((L, s) => {
    if (!S.mounted.includes(s)) return;
    for (const role of ['main', 'pre']) {
      const st = L.states[role];
      if (st && st.detectedFraction >= MIN_DETECTED_FRACTION && st.detectedPerSec > 0) wanted.push({ s, role, st });
    }
  });
  for (const w of wanted) {
    const lambdaNm = kernelWavelengthNm(w.st.lambdaNm);
    const kernel = scopeKernel(spec, lambdaNm, opts.onProgress && ((k, nz) => opts.onProgress('psf', (k + 1) / nz)), wanted.length + 1);
    // blinkKernel: with the halo cut (psf-halo-cut), for the blinks; the continuous populations keep kernel.
    const g = { structure: w.s, role: w.role, lambdaNm, kernel, blinkKernel: kernel && withHaloCut(kernel, S.O('psf-halo-cut')), sigmaPx: gaussianSigmaPx(lambdaNm, S.p.na, pixelNm),
      shares: w.st.lineShares, excite: exciteFor(w.st.lineShares),
      detectedFraction: w.st.detectedFraction, detectedPerSec: w.st.detectedPerSec, perFrame: w.st.detectedPerSec * S.expSec };
    groups.push(g);
    groupOf.set(`${w.s},${w.role}`, g);
  }
  // The fiducial beads: after the dye groups, so a bead component can share their kernels.
  const tFid = performance.now(), FB = fiducialBeads(spec, S, world, groups, exciteFor, wanted.length + 1);
  if (FB) FB.sec = (performance.now() - tFid) / 1000;
  const tPsf = performance.now();

  // ---- the kinetics at the local light (a shaped excitation, excitation.js) ----
  // A dye's light-driven rates follow its excitation (its state's photon factor a, g.excite):
  //   dSTORM (on- and off-switching, the initial ON and the 405 nm return all linear in the excitation): its kinetic
  //     clock is its dose (photons per switching cycle stay as measured; the 405 nm line taken to fall off as the
  //     main one);
  //   PALM: its activation at spont + lin a + quad a^2 (activationParts), its blinks after it on the sample's time;
  //   DNA-PAINT: binding needs no light; bleaching by a photon budget (WideField, the PALM pre state): at a x the rate.
  // The blinks, the per-dye windows and the mean field read each dye's clock at its own place from the movie's own
  // clock map (light_history.js movieClockMap: the sample lit by the movie's light at its pose from T = 0; the core's
  // map queries, ABI 14; the mean field's weights per map entry and the switch's survey, mean_field_map.js). Uniform
  // excitation: the rates of the label (outputs as before).
  const shaped = !EX.uniform;
  const own = shaped ? movieClockMap(S) : null, mapped = own !== null;
  // A structure whose clock is the same everywhere (no map, or the map's GLOBAL: DNA-PAINT, a target no light excites).
  const globalClock = s => !mapped || own.m.s[s].mode === CLOCK_GLOBAL;
  // The blinks under the own map (ScopeMovie.cpp mapEvents): every dye on its own clock (a dye the light does not
  // reach emits nothing).
  const mapEvents = () => cellFieldEventsInMap(world, S.q, own.buffer).filter(e => groupOf.has(`${e.structure},main`));

  // ---- blink mean-field frames (BlinkExpectation.cpp), decided before the query from the expected ON emitters on the
  // reference clock (start-sec; under the map the clock at the FOV centre and the focal plane): such a frame draws no
  // events, a movie mean-field throughout queries none. Not for a map's light-driven clock under drift (its weighted
  // scene does not follow the drift; a GLOBAL clock, the same everywhere, renders unweighted) ----
  const anyBlinks = S.labels.some((L, s) => S.mounted.includes(s) && L.mode !== 'WideField');
  const fovArea = W * H * um * um, BM = S.blinkMeanField;
  const blinkMfPossible = BM.densityPerUm2 < 1e9 || BM.maxEmitters < 1e9;
  let queryBlinks = false;
  for (const g of groups) {
    const L = S.labels[g.structure];
    if (g.role !== 'main' || L.mode === 'WideField') continue;
    g.mode = L.mode;
    if (blinkMfPossible && !(drift && !globalClock(g.structure))) {
      const focus = S.q.zCullCentreUm, slabHalf = S.meanField.slabNm / 2000, ox = S.q.originXUm, oy = S.q.originYUm;
      const [zMin, zMax] = S.q.zHalfRangeUm > 0 ? [focus - S.q.zHalfRangeUm, focus + S.q.zHalfRangeUm] : [-Infinity, Infinity];
      let nZ = 0, nSlab = 0;
      world.forEachDye(S.q.x0Um, S.q.y0Um, S.q.x1Um, S.q.y1Um, zMin, zMax, (b, i) => {
        if (b.structure !== g.structure) return;
        nZ++;
        if (b.x[i] >= ox && b.x[i] < ox + W * um && b.y[i] >= oy && b.y[i] < oy + H * um && b.z[i] >= Math.max(zMin, focus - slabHalf) && b.z[i] < Math.min(zMax, focus + slabHalf)) nSlab++;
      });
      g.nZ = nZ; g.nSlab = nSlab;
      g.refClock = S.t0Sec; g.refRate = 1.0;
      if (mapped) {
        const pr = clockPairAt(own.m, g.structure, true, ox + (W - 1) * um / 2, oy + (H - 1) * um / 2, focus);
        const bc = blinkClockAt(own.m.s[g.structure], pr.a, pr.b, pr.bounds, S.t0Sec);
        g.refClock = bc.t; g.refRate = bc.rate; g.segs = bc.segs;
      } else {
        const k = L.label.kinetics;
        g.segs = [{ tStart: 0, activationRatePerSec: k.activationRatePerSec, onSec: k.onSec, offSec: k.offSec, bleachProb: k.bleachProb, initialOnSec: k.initialOnSec }];
      }
      g.mfFrame = new Uint8Array(N);
      g.allMf = nZ > 0; g.anyMf = false;
      for (let f = 0; f < N; f++) {
        const t = g.refClock + g.refRate * (f * S.expSec), on = blinkPhotons(L.mode, 1.0, g.segs, t, g.refRate, S.expSec) / S.expSec;
        const mf = nZ > 0 && (nSlab * on / fovArea > BM.densityPerUm2 || nZ * on > BM.maxEmitters);
        g.mfFrame[f] = mf ? 1 : 0;
        g.anyMf = g.anyMf || mf; g.allMf = g.allMf && mf;
      }
    }
    queryBlinks = queryBlinks || !g.allMf;
  }

  // ---- blinks (none to query when every label is WideField or every frame is mean-field) ----
  const events = !queryBlinks ? [] : mapped ? mapEvents() : cellFieldEvents(world, S.q).filter(e => groupOf.has(`${e.structure},main`));
  if (opts.onEvents) opts.onEvents(events, S);
  const byGroup = groups.map(g => events.filter(e => e.structure === g.structure && g.role === 'main'));
  const buckets = byGroup.map(ev => bucketEventsByFrame(ev, N));

  // ---- continuous populations ----
  // Counted from the structure's dyes (every dye has the window from t = 0): nZ in the query rect and z range, nSlab
  // in the FOV within the focal slab. The per-dye windows are fetched only if some frame renders per dye.
  const pops = [];
  const focus = S.q.zCullCentreUm, slabHalf = S.meanField.slabNm / 2000;
  const [zMin, zMax] = S.q.zHalfRangeUm > 0 ? [focus - S.q.zHalfRangeUm, focus + S.q.zHalfRangeUm] : [-Infinity, Infinity];
  S.labels.forEach((L, s) => {
    if (!S.mounted.includes(s)) return;
    const states = [];
    if (L.mode === 'WideField') states.push(EVENT_STATE.ALWAYS_ON);
    if (L.mode === 'dSTORM' && L.label.kinetics.initialOnSec > 0) states.push(EVENT_STATE.INITIAL_ON);
    if (L.mode === 'PALM' && L.label.preState) states.push(EVENT_STATE.PRE);
    if (!states.length) return;
    const ox = S.q.originXUm, oy = S.q.originYUm;
    let nZ = 0, nSlab = 0;
    world.forEachDye(S.q.x0Um, S.q.y0Um, S.q.x1Um, S.q.y1Um, zMin, zMax, (b, i) => {
      if (b.structure !== s) return;
      nZ++;
      if (b.x[i] >= ox && b.x[i] < ox + W * um && b.y[i] >= oy && b.y[i] < oy + H * um && Math.abs(b.z[i] - focus) < slabHalf) nSlab++;
    });
    for (const state of states) {
      const g = groupOf.get(`${s},${state === EVENT_STATE.PRE ? 'pre' : 'main'}`);
      if (!g || !nZ) continue;
      const st = L.states[state === EVENT_STATE.PRE ? 'pre' : 'main'];
      let lambda = 0, budget = 0;
      if (state === EVENT_STATE.ALWAYS_ON) { budget = L.photonBudget; lambda = budget > 0 ? st.emissionPerSec / budget : 0; }
      else if (state === EVENT_STATE.INITIAL_ON) lambda = 1 / L.label.kinetics.initialOnSec;
      else { budget = L.prePhotonBudget; lambda = L.kActPerSec + (budget > 0 ? st.emissionPerSec / budget : 0); }
      const p = { structure: s, state, g, lambda, budget, emissionPerSec: st.emissionPerSec, rate: st.detectedPerSec,
        nZ, nSlab, nZLit: nZ, wins: [], image: null, mf: null, acc: null, accFrame: -1, accZ: 0, accDx: 0, accDy: 0, meanFieldFrames: 0, perDyeFrames: 0,
        lam: null, surveyed: false, meanSurvival: 1.0, surveyRate: 0.0 };
      // A map: each epoch's decay rate, and the share of the dyes the light reaches, their survival (survey).
      if (mapped) {
        p.lam = epochLambdas(own.m, S.labels, s, state);
        survey(own.m, p, S, world, g.excite, zMin, zMax);
      }
      pops.push(p);
    }
  });
  // The surviving share of the dyes in frame f (under the map the survey's survival, decaying at its rate) against the
  // switch's thresholds.
  const isMeanField = (p, f) => {
    const P = p.surveyed ? p.meanSurvival * Math.exp(-p.surveyRate * ((f + 0.5) * S.expSec))
      : Math.exp(-p.lambda * (S.t0Sec + (f + 0.5) * S.expSec));
    return p.nSlab * P / fovArea > S.meanField.densityPerUm2 || p.nZLit * P > S.meanField.maxEmitters;
  };
  // The per-dye windows in frames (fetched once, for every population that needs them): a dye with a photon budget
  // bleaches at aux x budget / emission rate (under a shaped excitation: the own map's windows in T -- the initial ON
  // ends on its dye's dose, the pre state at its activation; a dye the light does not reach is dark -- and a budget's
  // end where the dye's bleach hazard reaches its draw: the core's, from the map's bleach fields).
  let allWindows = null;
  for (const p of pops) {
    p.needWindows = !isMeanField(p, N - 1);
    // A map where no dye is likely still in the state (the survey: the initial ON's ends, long gone after a few seconds
    // of light): no windows, no query.
    if (p.needWindows && p.surveyed && p.nZLit * p.meanSurvival < WINDOW_ALIVE_MIN) p.needWindows = false;
  }
  for (const p of pops) {
    if (!p.needWindows) continue;   // mean-field to the end (or no dye left): no windows needed
    if (mapped) {
      if (!allWindows) allWindows = cellFieldContinuousInMap(world, S.q, own.buffer);
      const dose = own.m.s[p.structure].mode === CLOCK_DOSE;
      for (const e of allWindows) {
        if (e.structure !== p.structure || e.state !== p.state) continue;
        if (dose && !(e.rate > 0)) continue;   // frozen: no light
        const a = p.g.excite(e.xUm, e.yUm, e.zNm);
        if (!(a > 0)) continue;
        if (e.tEnd > 0) {
          const { clock, rate, ...w } = e;
          p.wins.push({ ...w, brightness: 1 });
        }
      }
      continue;
    }
    if (!allWindows) allWindows = cellFieldContinuous(world, S.q);
    for (const e of allWindows) {
      if (e.structure !== p.structure || e.state !== p.state) continue;
      let tEndSec = (e.tEnd - S.q.frameIndex) * S.expSec + S.q.tSec;   // back to seconds (toFov mapped it)
      if (p.budget > 0) tEndSec = Math.min(tEndSec, e.aux * p.budget / p.emissionPerSec);
      const b = (tEndSec - S.q.tSec) / S.expSec;
      if (b > 0) p.wins.push({ ...e, tStart: (0 - S.q.tSec) / S.expSec, tEnd: b });
    }
  }
  allWindows = null;
  // The blink populations of the groups with mean-field frames (photons per dye: rate x expected ON time, on the
  // reference clock; under the map at each dye's own).
  for (const g of groups)
    if (g.anyMf)
      pops.push({ structure: g.structure, state: EVENT_STATE.BLINK, blink: true, g, rate: g.detectedPerSec, nZ: g.nZ, nZLit: g.nZ,
        nSlab: g.nSlab, mode: g.mode, segs: g.segs, blinkClock: g.refClock, blinkRate: g.refRate, image: null, mf: null,
        meanFieldFrames: 0, perDyeFrames: 0 });
  const querySec = (performance.now() - t0) / 1000;
  // Running image of the windows that cover frame f fully (unit photons each).
  const advanceAcc = (p, f, z, dx, dy) => {
    // Another focal plane or a moved sample: start the running image afresh.
    if (p.accFrame >= 0 && (p.accZ !== z || p.accDx !== dx || p.accDy !== dy)) p.accFrame = -1;
    p.accZ = z; p.accDx = dx; p.accDy = dy;
    if (p.accFrame < 0) {
      p.acc = new Float64Array(W * H);
      for (const e of p.wins) if (e.tStart <= f && e.tEnd >= f + 1) splatUnit(p.acc, W, H, e, p.g, z, pixelNm, 1, dx, dy);
    } else {
      for (const e of p.wins) {
        const was = e.tStart <= p.accFrame && e.tEnd >= p.accFrame + 1, now = e.tStart <= f && e.tEnd >= f + 1;
        if (was !== now) splatUnit(p.acc, W, H, e, p.g, z, pixelNm, now ? 1 : -1, dx, dy);
      }
    }
    p.accFrame = f;
  };

  // The binned blink regime (BinnedBlinks.cpp): per main group, used for a frame whose ON emitters per um^2 of the FOV
  // (or in all) pass the thresholds.
  const BB = S.blinkBinned;
  const binnedPossible = BB.densityPerUm2 < 1e9 || BB.maxEmitters < 1e9;
  for (const g of groups)
    if (g.role === 'main' && binnedPossible) g.binned = binnedBlinkRenderer(g.blinkKernel, g.sigmaPx, W, H, BB.upscale);
  const binnedFrame = (g, evs, f) => {
    if (!g.binned) return false;
    let nOn = 0.0;
    for (const e of evs) { const ov = Math.min(f + 1, e.tEnd) - Math.max(f, e.tStart); if (ov > 0.0) nOn += Math.min(ov, 1.0); }
    return nOn / fovArea > BB.densityPerUm2 || nOn > BB.maxEmitters;
  };
  let binnedFrames = 0;
  const maps = noiseMaps(S.seed, W, H, S.cam);
  const imagerPerFrame = S.labels.reduce((sum, L, s) => sum + (S.mounted.includes(s) ? L.imagerBackgroundPerPxPerSec(um) : 0), 0) * S.expSec;
  const bg = S.p.backgroundPhotons + imagerPerFrame;
  // A shaped excitation (not uniform): the background per pixel -- the flat background x the beam's footprint on the
  // coverslip (no field factor; it includes stray light), plus each DNA-PAINT label's free imager x the excitation's
  // collected column over the chamber (excitation.js imagerMap: the lit imager at each height, defocused onto the
  // pixel; epi: ~ the beam's radius, not the chamber's height; HILO: the sheet; TIRF: ~ the decay depth).
  let bgMap = null, imagerMean = imagerPerFrame;
  if (!EX.uniform) {
    bgMap = new Float32Array(W * H);
    const H0 = S.lp.chamberHeightUm, imagers = S.labels.filter((L, s) => S.mounted.includes(s) && L.imagerPerPxPerUmPerSec(um) > 0);
    // Made at the grid's foci around this one (BG_FOCUS_STEP_UM apart; on a grid focus that one alone) and blended
    // linearly (ScopeMovie.cpp BgMapCache: a host moving the focus reuses the grid's maps).
    const sources = imagers.map(L => ({ rate: L.imagerPerPxPerUmPerSec(um), shares: L.states.main.lineShares }));
    const slice = k => {
      const ex = makeExcitation({ ...EX.spec, pivotUm: k * BG_FOCUS_STEP_UM });
      const imMap = ex.imagerMap(W, H, um, axisX, axisY, H0, sources);
      const lat = new Float64Array(W * H), imv = new Float64Array(W * H);
      for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) {
        const dx = x * um - axisX, dy = y * um - axisY;
        let im = imMap[y * W + x];
        im *= S.expSec;
        imv[y * W + x] = im;
        lat[y * W + x] = S.p.backgroundPhotons * ex.lateral(dx, dy, 0.0);
      }
      return { lat, im: imv };
    };
    const pivot = EX.spec.pivotUm ?? 0.0, k0 = Math.floor(pivot / BG_FOCUS_STEP_UM);
    const t = (pivot - k0 * BG_FOCUS_STEP_UM) / BG_FOCUS_STEP_UM;
    const A = slice(k0), B = t > 0.0 ? slice(k0 + 1) : null;
    let sum = 0.0;
    for (let k = 0; k < W * H; k++) {
      const im = B ? (1.0 - t) * A.im[k] + t * B.im[k] : A.im[k];
      const lat = B ? (1.0 - t) * A.lat[k] + t * B.lat[k] : A.lat[k];
      sum += im;
      bgMap[k] = lat + im;
    }
    imagerMean = sum / (W * H);
  }
  // A population's mean field (BuildMeanField): under the map weighted -- each dye with its photons at its own clock
  // (MapMeanField; a blink population on a GLOBAL clock, the same everywhere, renders unweighted) --, else the image x
  // its photons per dye in the frame (framePhotons; a blink population on its clock at the FOV centre).
  const buildMeanField = p => {
    p.weighted = mapped && !(p.blink && globalClock(p.structure));
    p.mf = p.weighted
      ? new MapMeanField(meanFieldScene(world, S, 1 << p.structure, p.g.kernel, p.g.lambdaNm, driftMarginUm, p.g.shares), own.m, p, S)
      : meanFieldImage(world, S, 1 << p.structure, p.g.kernel, p.g.lambdaNm, driftMarginUm, p.g.shares);
  };
  const framePhotons = (p, f) => {
    if (p.blink) return blinkPhotons(p.mode, p.rate, p.segs, p.blinkClock + p.blinkRate * (f * S.expSec), p.blinkRate, S.expSec);
    const tf0 = S.t0Sec + f * S.expSec;
    return meanPhotons(p.rate, p.lambda, tf0, tf0 + S.expSec);
  };
  // Drift (DriftMeanField): the scene's images at the two focus-grid foci around the frame's dz (the sample dz higher:
  // the focal plane dz lower in it), their spectra interpolated and shifted by the xy drift.
  const driftFocus = kk => S.q.zRefUm + zStage - driftGridDzNm(driftGrid, kk) / 1000.0;
  const meanFieldImageOf = (p, f) => {
    if (!drift) return p.mf.image;
    const d = S.drift[f], [kk0, w] = driftGridWeights(driftGrid, d.z), image = new Float32Array(W * H);
    if (p.weighted) {
      p.mf.driftFrame(f, d, driftFocus, kk0, w, driftGrid.n, image, renderShiftedImages);
      return image;
    }
    const i0 = p.mf.imagesAt(driftFocus(kk0)), i1 = w !== 0 && kk0 + 1 < driftGrid.n ? p.mf.imagesAt(driftFocus(kk0 + 1)) : null;
    const pitchNm = p.mf.pitchUm * 1000.0;
    renderShiftedImages(i0, i1, i1 ? w : 0.0, d.x / pitchNm, d.y / pitchNm, image);
    return image;
  };
  const addScaled = (img, image, m) => { for (let i = 0; i < img.length; i++) img[i] += Math.fround(m * image[i]); };
  const addFrame = (img, frame) => { for (let i = 0; i < img.length; i++) img[i] += frame[i]; };
  let blinks = 0;
  const POP_NAME = { [EVENT_STATE.ALWAYS_ON]: 'WideField dyes', [EVENT_STATE.INITIAL_ON]: 'initial ON', [EVENT_STATE.PRE]: 'pre state' };
  for (let f = 0; f < N; f++) {
    const frameBlinks0 = blinks, paths = [];
    const img = bgMap ? bgMap.slice() : new Float32Array(W * H).fill(bg);
    const zf = zAt(f), [dx, dy] = dPx(f);
    let binned = false, mfBlinks = false;
    groups.forEach((g, gi) => {
      if (g.role !== 'main') return;
      if (g.mfFrame && g.mfFrame[f]) { mfBlinks = true; return; }
      const evs = buckets[gi][f].map(i => byGroup[gi][i]);
      blinks += evs.length;
      const o = { pixelSizeNm: pixelNm, photonsPerBlink: g.perFrame, psfSigmaPx: g.sigmaPx, excite: g.excite,
        minPhotons: S.blinkMinPhotons };
      if (binnedFrame(g, evs, f)) {
        g.binned.render(frameEmitters(evs, f, o, g.blinkKernel, zf, dx, dy), img);
        binned = true;
        return;
      }
      renderPhotonImage(W, H, evs, f, o, g.blinkKernel, zf, img, dx, dy);
    });
    if (binned) binnedFrames++;
    for (const p of pops) {
      if (p.blink) {
        if (!p.g.mfFrame[f]) continue;   // drawn from its events (splat or binned)
        if (!p.mf) buildMeanField(p);
        if (p.weighted) addFrame(img, p.mf.blinkFrame(f));
        else addScaled(img, meanFieldImageOf(p, f), Math.fround(framePhotons(p, f)));
        p.meanFieldFrames++;
        paths.push('blinks: mean-field (FFT)');
        continue;
      }
      if (isMeanField(p, f)) {
        if (!p.mf) buildMeanField(p);
        // A weighted population with rates: each entry at its own decay (RateWeightedFrame; under drift in the drift's
        // weights); without: frame 0 at each entry's clock, every one f exposures later (its decay the same everywhere).
        if (p.weighted && p.mf.rates && !drift) addFrame(img, p.mf.rateFrame(f));
        else if (p.weighted) {
          const sc = !p.mf.rates && p.mf.decay > 0 ? Math.exp(-p.mf.decay * f * S.expSec) : 1.0;
          addScaled(img, meanFieldImageOf(p, f), sc);
        } else addScaled(img, meanFieldImageOf(p, f), Math.fround(framePhotons(p, f)));
        p.meanFieldFrames++;
        paths.push(`${POP_NAME[p.state]}: mean-field (FFT)`);
      } else {
        advanceAcc(p, f, zf, dx, dy);
        const perFrame = p.rate * S.expSec;
        for (let i = 0; i < img.length; i++) if (p.acc[i] !== 0) img[i] += Math.fround(perFrame * p.acc[i]);
        // Windows that start or end inside this frame: their overlap.
        const partial = p.wins.filter(e => e.tStart < f + 1 && e.tEnd > f && !(e.tStart <= f && e.tEnd >= f + 1));
        renderPhotonImage(W, H, partial, f, { pixelSizeNm: pixelNm, photonsPerBlink: perFrame, psfSigmaPx: p.g.sigmaPx, excite: p.g.excite }, p.g.kernel, zf, img, dx, dy);
        p.perDyeFrames++;
        paths.push(`${POP_NAME[p.state]}: per dye (${p.wins.length} windows)`);
      }
    }
    // The fiducial beads, last.
    if (FB && FB.comps.length) {
      const tb = performance.now();
      renderFiducials(img, W, H, FB, f, zf, pixelNm, drift, dx, dy);
      FB.sec += (performance.now() - tb) / 1000;
      paths.push(`fiducials: ${FB.comps.map(F => `${F.beads.length} ${F.c.id}`).join(', ')}`);
    }
    // opts.onPhotons(f, photons): the photon image goes there instead of the noise chain and onFrame (C++
    // FluorescenceFrameOptions::onPhotons; the combined light adds the lamp and runs its own noise chain).
    if ((opts.onPhotons ? opts.onPhotons(f, img) : onFrame(f, applyNoiseChain(img, S.cam, maps, f), img)) === false) break;
    // Which backend drew this frame: the SMLM splat for blinks, mean-field or per dye for each continuous population.
    if (opts.onProgress) opts.onProgress('frames', (f + 1) / N, {
      frame: f, frames: N, blinks: blinks - frameBlinks0, backends: [...(anyBlinks && !(mfBlinks && blinks === frameBlinks0) ? [`SMLM: ${blinks - frameBlinks0} blinks (${binned ? 'binned FFT' : 'splat'})`] : []), ...paths] });
  }
  return {
    width: W, height: H, frames: N, blinks: events.length, renderedBlinks: blinks, binnedFrames, psfSec: (tPsf - t0) / 1000, querySec,
    totalSec: (performance.now() - t0) / 1000, psf: groups.some(g => g.kernel) ? 'GibsonLanniZernike' : 'Gaussian',
    imagerBackgroundPerPxPerFrame: imagerMean, driftNm: driftInfo(S),
    excitation: { regime: EX.regime, bfp: EX.u, uMax: EX.uMax, fieldFactor: EX.F,
      depthNm: EX.depthNm(groups.length && groups[0].shares.length ? groups[0].shares[0].nm : 640) },
    labels: S.labels.flatMap((L, s) => !S.mounted.includes(s) ? [] : [{ structure: STRUCTURES[s].id, dye: L.dye ? L.dye.id : "None", mode: L.mode, kActPerSec: L.kActPerSec,
      states: Object.fromEntries(Object.entries(L.states).map(([k, v]) => [k, v && { lambdaNm: v.lambdaNm,
        detectedFraction: v.detectedFraction, detectedPerSec: v.detectedPerSec }])) }]),
    groups: groups.map(g => ({ structure: g.structure, role: g.role, lambdaNm: g.lambdaNm, perFrame: g.perFrame })),
    fiducials: FB ? { type: FIDUCIAL_TYPE_IDS[S.fid.type], beads: FB.sites.length, sec: FB.sec,
      components: FB.comps.map(F => ({ id: F.c.id, lambdaNm: F.lambdaNm, beads: F.beads.length,
        meanPerSec: F.beads.reduce((m, e) => m + e.rate, 0) / F.beads.length,
        bleachHalfSec: F.c.bleachPerSec > 0 ? Math.LN2 / F.c.bleachPerSec : Infinity })) } : null,
    populations: pops.filter(p => !p.blink).map(p => ({ structure: p.structure, state: p.state, dyes: p.nZ, inSlab: p.nSlab,
      meanFieldFrames: p.meanFieldFrames, perDyeFrames: p.perDyeFrames })),
  };
}
