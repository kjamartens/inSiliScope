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
//      most once, mean-field -> per dye.
// Then ApplyNoiseChain at QE 1 (the photons are detected photons: QE(lambda) sits in each dye's detected fraction).
import { scopeKernel, scopeWorld, cellFieldEvents, cellFieldContinuous, kernelWavelengthNm, driftInfo } from './scope_movie.js';
import { bucketEventsByFrame, frameEmitters, renderPhotonImage, renderGaussian, noiseMaps, applyNoiseChain } from './render.js';
import { binnedBlinkRenderer } from './binned_blinks.js';
import { expectedBlinkOnSeconds } from './blink_expectation.js';
import { nearestZIndex, planSplat, splatRows, withHaloCut } from './psf.js';
import { meanFieldImage, renderShiftedImages } from './widefield.js';
import { driftMaxXyNm, driftFocusGrid, driftGridDzNm, driftGridWeights } from './drift.js';
import { EVENT_STATE } from './dyes.js';
import { doseNodes, DOSE_LEVELS } from './excitation.js';

const DOSE_BINS = 256;   // the mean-field switch's histogram of the dyes' excitation factors (shaped excitation)
import { STRUCTURES } from './world.js';

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
  const tPsf = performance.now();

  // ---- the kinetics at the local light (a shaped excitation, excitation.js) ----
  // A dye's light-driven rates follow its excitation factor a (its state's photon factor, g.excite):
  //   dSTORM (on- and off-switching, the initial ON and the 405 nm return all linear in the excitation): its kinetic
  //     clock runs at a x real time (photons per switching cycle stay as measured; the 405 nm line taken to fall off
  //     as the main one);
  //   PALM: its activation at spont + lin a + quad a^2 (activationParts; its first activation moves from T to T / b,
  //     b = that / kAct), its blinks keep their times;
  //   DNA-PAINT: binding needs no light; bleaching by a photon budget (WideField, the PALM pre state): at a x the rate.
  // Uniform excitation: the rates of the label (outputs as before).
  const shaped = !EX.uniform, T0 = S.t0Sec, tEndSec = S.t0Sec + N * S.expSec;
  const actScale = (L, a) => {
    const q = L.activationParts;
    return q && L.kActPerSec > 0 ? (q.spont + q.lin * a + q.quad * a * a) / L.kActPerSec : 1.0;
  };
  const zWin = S.q.zHalfRangeUm > 0 ? [S.q.zCullCentreUm - S.q.zHalfRangeUm, S.q.zCullCentreUm + S.q.zHalfRangeUm] : [-Infinity, Infinity];
  const fovOf = e => [e.x - S.q.originXUm, e.y - S.q.originYUm, (e.z - S.q.zRefUm) * 1000.0];
  // The blinks under a shaped excitation: the dSTORM and PALM labels' queried from t = 0 to the latest time a dye's
  // own clock reaches (kMax x the movie's end), mapped to real time; the others' as usual.
  function localEvents() {
    const kinetic = [], plain = [];
    for (const g of groups)
      if (g.role === 'main' && g.mode) (g.mode === 'dSTORM' || g.mode === 'PALM' ? kinetic : plain).push(g.structure);
    const out = [];
    if (plain.length)
      for (const e of cellFieldEvents(world, S.q)) if (plain.includes(e.structure)) out.push(e);
    if (!kinetic.length) return out;
    let kMax = 1.0;
    for (const s of kinetic) {
      const L = S.labels[s];
      kMax = Math.max(kMax, L.mode === 'dSTORM' ? EX.F : actScale(L, EX.F));
    }
    const first = new Map();
    for (const e of world.eventsInWindow(S.q.x0Um, S.q.y0Um, S.q.x1Um, S.q.y1Um, zWin[0], zWin[1], 0.0, kMax * tEndSec)) {
      if (!kinetic.includes(e.structure)) continue;
      const g = groupOf.get(`${e.structure},main`), L = S.labels[e.structure];
      const [xUm, yUm, zNm] = fovOf(e), a = g.excite(xUm, yUm, zNm);
      if (!(a > 0)) continue;
      let tOn = e.tOn, tOff = e.tOff;
      if (L.mode === 'dSTORM') { tOn = e.tOn / a; tOff = e.tOff / a; }
      else {
        // The dye's first blink (its events come in time order from t = 0) is its activation.
        const key = `${e.structure}:${e.x}:${e.y}:${e.z}`;   // a dye: its structure and position
        let T = first.get(key);
        if (T === undefined) { T = e.tOn; first.set(key, T); }
        const b = actScale(L, a);
        if (!(b > 0)) continue;
        const shift = T / b - T;
        tOn = e.tOn + shift; tOff = e.tOff + shift;
      }
      const tStart = (tOn - T0) / S.expSec, tEnd = (tOff - T0) / S.expSec;
      if (!(tEnd > 0.0) || !(tStart < N)) continue;
      out.push({ xUm, yUm, zNm, tStart, tEnd, brightness: e.brightness, structure: e.structure, state: e.state, aux: e.aux });
    }
    return out;
  }

  // ---- blink mean-field frames (BlinkExpectation.cpp), decided before the query from the expected ON emitters at
  // start-sec: such a frame draws no events, a movie mean-field throughout queries none ----
  const anyBlinks = S.labels.some((L, s) => S.mounted.includes(s) && L.mode !== 'WideField');
  const fovArea = W * H * um * um, BM = S.blinkMeanField;
  const blinkMfPossible = BM.densityPerUm2 < 1e9 || BM.maxEmitters < 1e9;
  let queryBlinks = false;
  for (const g of groups) {
    const L = S.labels[g.structure];
    if (g.role !== 'main' || L.mode === 'WideField') continue;
    g.mode = L.mode;
    if (blinkMfPossible) {
      const focus = S.q.zCullCentreUm, slabHalf = S.meanField.slabNm / 2000, ox = S.q.originXUm, oy = S.q.originYUm;
      const [zMin, zMax] = S.q.zHalfRangeUm > 0 ? [focus - S.q.zHalfRangeUm, focus + S.q.zHalfRangeUm] : [-Infinity, Infinity];
      let nZ = 0, nSlab = 0;
      world.forEachDye(S.q.x0Um, S.q.y0Um, S.q.x1Um, S.q.y1Um, zMin, zMax, (b, i) => {
        if (b.structure !== g.structure) return;
        nZ++;
        if (b.x[i] >= ox && b.x[i] < ox + W * um && b.y[i] >= oy && b.y[i] < oy + H * um && b.z[i] >= Math.max(zMin, focus - slabHalf) && b.z[i] < Math.min(zMax, focus + slabHalf)) nSlab++;
      });
      const k = L.label.kinetics;
      g.nZ = nZ; g.nSlab = nSlab;
      g.segs = [{ tStart: 0, activationRatePerSec: k.activationRatePerSec, onSec: k.onSec, offSec: k.offSec, bleachProb: k.bleachProb, initialOnSec: k.initialOnSec }];
      g.mfFrame = new Uint8Array(N);
      g.allMf = nZ > 0; g.anyMf = false;
      for (let f = 0; f < N; f++) {
        const t = S.t0Sec + f * S.expSec, on = expectedBlinkOnSeconds(L.mode, g.segs, t, t + S.expSec) / S.expSec;
        const mf = nZ > 0 && (nSlab * on / fovArea > BM.densityPerUm2 || nZ * on > BM.maxEmitters);
        g.mfFrame[f] = mf ? 1 : 0;
        g.anyMf = g.anyMf || mf; g.allMf = g.allMf && mf;
      }
    }
    queryBlinks = queryBlinks || !g.allMf;
  }

  // ---- blinks (none to query when every label is WideField or every frame is mean-field) ----
  const events = !queryBlinks ? [] : shaped ? localEvents() : cellFieldEvents(world, S.q).filter(e => groupOf.has(`${e.structure},main`));
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
        nZ, nSlab, wins: null, image: null, mf: null, acc: null, accFrame: -1, accZ: 0, accDx: 0, accDy: 0, meanFieldFrames: 0, perDyeFrames: 0,
        nodes: null, lambdaAt: null, hZ: null, hS: null };
      if (shaped) {
        // The decay rate at factor a (the budget's bleaching at a x; the initial ON at a x; the pre state's activation
        // as the blinks'), the dose nodes of its mean-field images and the dyes' factors (DOSE_BINS bins over
        // [0, F]: the mean-field switch counts the surviving dyes).
        const bl = budget > 0 ? st.emissionPerSec / budget : 0;
        p.lambdaAt = state === EVENT_STATE.ALWAYS_ON ? a => bl * a
          : state === EVENT_STATE.INITIAL_ON ? a => a / L.label.kinetics.initialOnSec
          : a => L.kActPerSec * actScale(L, a) + bl * a;
        p.nodes = doseNodes(EX.F, p.lambdaAt(EX.F) * tEndSec);
        p.hZ = new Float64Array(DOSE_BINS); p.hS = new Float64Array(DOSE_BINS);
        world.forEachDye(S.q.x0Um, S.q.y0Um, S.q.x1Um, S.q.y1Um, zMin, zMax, (b, i) => {
          if (b.structure !== s) return;
          const xUm = b.x[i] - ox, yUm = b.y[i] - oy, a = g.excite(xUm, yUm, (b.z[i] - S.q.zRefUm) * 1000.0);
          const k = EX.F > 0 ? Math.min(DOSE_BINS - 1, Math.max(0, Math.floor(a / EX.F * DOSE_BINS))) : 0;
          p.hZ[k]++;
          if (xUm >= 0 && xUm < W * um && yUm >= 0 && yUm < H * um && Math.abs(b.z[i] - focus) < slabHalf) p.hS[k]++;
        });
      }
      pops.push(p);
    }
  });
  const isMeanField = (p, f) => {
    const tMid = S.t0Sec + (f + 0.5) * S.expSec;
    if (p.hZ) {
      let nz = 0.0, ns = 0.0;
      for (let k = 0; k < DOSE_BINS; k++) {
        if (!p.hZ[k]) continue;
        const P = Math.exp(-p.lambdaAt((k + 0.5) / DOSE_BINS * EX.F) * tMid);
        nz += p.hZ[k] * P; ns += p.hS[k] * P;
      }
      return ns / fovArea > S.meanField.densityPerUm2 || nz > S.meanField.maxEmitters;
    }
    const P = Math.exp(-p.lambda * tMid);
    return p.nSlab * P / fovArea > S.meanField.densityPerUm2 || p.nZ * P > S.meanField.maxEmitters;
  };
  // The per-dye windows in frames (fetched once, for every population that needs them): a dye with a photon budget
  // bleaches at aux x budget / emission rate (x its factor a under a shaped excitation, where the windows are
  // fetched from t = 0 and mapped to real time: the dSTORM initial ON ends at tOff / a, the PALM pre state at
  // tOff / b).
  let allWindows = null;
  for (const p of pops) {
    if (isMeanField(p, N - 1)) continue;   // mean-field to the end: no windows needed
    p.wins = [];
    if (shaped) {
      if (!allWindows) allWindows = world.continuousInWindow(S.q.x0Um, S.q.y0Um, S.q.x1Um, S.q.y1Um, zWin[0], zWin[1], -Infinity);
      const L = S.labels[p.structure];
      for (const e of allWindows) {
        if (e.structure !== p.structure || e.state !== p.state) continue;
        const [xUm, yUm, zNm] = fovOf(e), a = p.g.excite(xUm, yUm, zNm);
        if (!(a > 0)) continue;
        let tEnd = Infinity;
        if (p.state === EVENT_STATE.INITIAL_ON) tEnd = e.tOff / a;
        else if (p.state === EVENT_STATE.PRE) { const b = actScale(L, a); tEnd = b > 0 ? e.tOff / b : Infinity; }
        if (p.budget > 0) tEnd = Math.min(tEnd, e.aux * p.budget / (p.emissionPerSec * a));
        const bF = (tEnd - T0) / S.expSec;
        if (bF > 0) p.wins.push({ xUm, yUm, zNm, tStart: (0 - T0) / S.expSec, tEnd: bF, brightness: 1, structure: e.structure, state: e.state, aux: e.aux });
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
  // The blink populations of the groups with mean-field frames (photons per dye: rate x expected ON time).
  for (const g of groups)
    if (g.anyMf)
      pops.push({ structure: g.structure, state: EVENT_STATE.BLINK, blink: true, g, rate: g.detectedPerSec, nZ: g.nZ, nSlab: g.nSlab,
        mode: g.mode, segs: g.segs, image: null, mf: null, meanFieldFrames: 0, perDyeFrames: 0,
        // Shaped: dSTORM's ON time is nonlinear in a (DOSE_LEVELS nodes up to F), the others' linear (one node).
        nodes: !shaped ? null : !(EX.F > 0) ? [] : g.mode === 'dSTORM' ? Array.from({ length: DOSE_LEVELS }, (_, k) => EX.F * (k + 1) / DOSE_LEVELS) : [EX.F] });
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
  // column integral over the chamber (epi: its height; HILO: the sheet; TIRF: ~ the decay depth).
  let bgMap = null, imagerMean = imagerPerFrame;
  if (!EX.uniform) {
    bgMap = new Float32Array(W * H);
    const H0 = S.lp.chamberHeightUm, imagers = S.labels.filter((L, s) => S.mounted.includes(s) && L.imagerPerPxPerUmPerSec(um) > 0);
    let sum = 0.0;
    for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) {
      const dx = x * um - axisX, dy = y * um - axisY;
      let im = 0.0;
      for (const L of imagers) im += L.imagerPerPxPerUmPerSec(um) * EX.column(dx, dy, H0, L.states.main.lineShares);
      im *= S.expSec;
      sum += im;
      bgMap[y * W + x] = S.p.backgroundPhotons * EX.lateral(dx, dy, 0.0) + im;
    }
    imagerMean = sum / (W * H);
  }
  // A population's mean-field frame into img: per dose node k (one node, a = 1, for a uniform excitation) its image --
  // drifted: the images at the two grid foci around dz, interpolated and shifted by the xy drift (DriftMeanField) --
  // x the photons of a dye at a_k in frame f.
  const addMeanField = (img, p, f, photonsAt) => {
    if (p.nodes && !p.nodes.length) return;   // a clipped spot: no light
    if (!p.mf) p.mf = meanFieldImage(world, S, 1 << p.structure, p.g.kernel, p.g.lambdaNm, driftMarginUm, p.g.shares, p.nodes);
    p.mf.levels.forEach((lv, k) => {
      let image = lv.image;
      if (drift) {
        const d = S.drift[f], [kk0, w] = driftGridWeights(driftGrid, d.z);
        const at = kk => lv.imagesAt(S.q.zRefUm + zStage - driftGridDzNm(driftGrid, kk) / 1000.0);
        const i0 = at(kk0), i1 = w !== 0 && kk0 + 1 < driftGrid.n ? at(kk0 + 1) : null, pitchNm = p.mf.pitchUm * 1000.0;
        image = new Float32Array(W * H);
        renderShiftedImages(i0, i1, i1 ? w : 0.0, d.x / pitchNm, d.y / pitchNm, image);
      }
      const m = Math.fround(photonsAt(p.nodes ? p.nodes[k] : 1.0));
      for (let i = 0; i < img.length; i++) img[i] += Math.fround(m * image[i]);
    });
  };
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
      const o = { pixelSizeNm: pixelNm, photonsPerBlink: g.perFrame, psfSigmaPx: g.sigmaPx, excite: g.excite };
      if (binnedFrame(g, evs, f)) {
        g.binned.render(frameEmitters(evs, f, o, g.blinkKernel, zf, dx, dy), img);
        binned = true;
        return;
      }
      renderPhotonImage(W, H, evs, f, o, g.blinkKernel, zf, img, dx, dy);
    });
    if (binned) binnedFrames++;
    for (const p of pops) {
      const tf0 = S.t0Sec + f * S.expSec, tf1 = tf0 + S.expSec;
      if (p.blink) {
        if (!p.g.mfFrame[f]) continue;   // drawn from its events (splat or binned)
        // Photons per dye: rate x expected ON time; dSTORM at factor a: its clock at a x (rate x a x ON / a).
        addMeanField(img, p, f, !shaped ? () => p.rate * expectedBlinkOnSeconds(p.mode, p.segs, tf0, tf1)
          : p.mode === 'dSTORM' ? a => p.rate * expectedBlinkOnSeconds(p.mode, p.segs, a * tf0, a * tf1)
          : a => p.rate * a * expectedBlinkOnSeconds(p.mode, p.segs, tf0, tf1));
        p.meanFieldFrames++;
        paths.push('blinks: mean-field (FFT)');
        continue;
      }
      if (isMeanField(p, f)) {
        addMeanField(img, p, f, !shaped ? () => meanPhotons(p.rate, p.lambda, tf0, tf1)
          : a => meanPhotons(p.rate * a, p.lambdaAt(a), tf0, tf1));
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
    populations: pops.filter(p => !p.blink).map(p => ({ structure: p.structure, state: p.state, dyes: p.nZ, inSlab: p.nSlab,
      meanFieldFrames: p.meanFieldFrames, perDyeFrames: p.perDyeFrames })),
  };
}
