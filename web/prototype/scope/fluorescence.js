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
import { bucketEventsByFrame, renderPhotonImage, renderGaussian, noiseMaps, applyNoiseChain } from './render.js';
import { nearestZIndex, planSplat, splatRows } from './psf.js';
import { meanFieldImage, renderShiftedImages } from './widefield.js';
import { driftMaxXyNm, driftFocusGrid, driftGridDzNm, driftGridWeights } from './drift.js';
import { EVENT_STATE } from './dyes.js';
import { STRUCTURES } from './world.js';

const MIN_DETECTED_FRACTION = 1e-4;   // a (structure, state) detected less than this is skipped (no kernel)

const gaussianSigmaPx = (lambdaNm, na, pixelNm) => Math.min(Math.max(0.21 * lambdaNm / Math.max(0.01, na) / pixelNm, 0.3), 20.0);

// One unit-photon emitter (sign +1 or -1) into img at its plane: the running image's add/remove (dxPx, dyPx: the
// sample drift).
function splatUnit(img, W, H, e, g, zStage, pixelNm, sign, dxPx = 0.0, dyPx = 0.0) {
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
  S.labels.forEach((L, s) => {
    for (const role of ['main', 'pre']) {
      const st = L.states[role];
      if (st && st.detectedFraction >= MIN_DETECTED_FRACTION && st.detectedPerSec > 0) wanted.push({ s, role, st });
    }
  });
  for (const w of wanted) {
    const lambdaNm = kernelWavelengthNm(w.st.lambdaNm);
    const kernel = scopeKernel(spec, lambdaNm, opts.onProgress && ((k, nz) => opts.onProgress('psf', (k + 1) / nz)), wanted.length + 1);
    const g = { structure: w.s, role: w.role, lambdaNm, kernel, sigmaPx: gaussianSigmaPx(lambdaNm, S.p.na, pixelNm),
      detectedFraction: w.st.detectedFraction, detectedPerSec: w.st.detectedPerSec, perFrame: w.st.detectedPerSec * S.expSec };
    groups.push(g);
    groupOf.set(`${w.s},${w.role}`, g);
  }
  const tPsf = performance.now();

  // ---- blinks (none to query when every label is WideField) ----
  const anyBlinks = S.labels.some(L => L.mode !== 'WideField');
  const events = anyBlinks ? cellFieldEvents(world, S.q).filter(e => groupOf.has(`${e.structure},main`)) : [];
  if (opts.onEvents) opts.onEvents(events, S);
  const byGroup = groups.map(g => events.filter(e => e.structure === g.structure && g.role === 'main'));
  const buckets = byGroup.map(ev => bucketEventsByFrame(ev, N));

  // ---- continuous populations ----
  // Counted from the structure's dyes (every dye has the window from t = 0): nZ in the query rect and z range, nSlab
  // in the FOV within the focal slab. The per-dye windows are fetched only if some frame renders per dye.
  const pops = [];
  const focus = S.q.zCullCentreUm, slabHalf = S.meanField.slabNm / 2000, fovArea = W * H * um * um;
  const [zMin, zMax] = S.q.zHalfRangeUm > 0 ? [focus - S.q.zHalfRangeUm, focus + S.q.zHalfRangeUm] : [-Infinity, Infinity];
  S.labels.forEach((L, s) => {
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
      pops.push({ structure: s, state, g, lambda, budget, emissionPerSec: st.emissionPerSec, rate: st.detectedPerSec,
        nZ, nSlab, wins: null, image: null, mf: null, acc: null, accFrame: -1, accZ: 0, accDx: 0, accDy: 0, meanFieldFrames: 0, perDyeFrames: 0 });
    }
  });
  const isMeanField = (p, f) => {
    const tMid = S.t0Sec + (f + 0.5) * S.expSec, P = Math.exp(-p.lambda * tMid);
    return p.nSlab * P / fovArea > S.meanField.densityPerUm2 || p.nZ * P > S.meanField.maxEmitters;
  };
  // The per-dye windows in frames (fetched once, for every population that needs them): a dye with a photon budget
  // bleaches at aux x budget / emission rate.
  let allWindows = null;
  for (const p of pops) {
    if (isMeanField(p, N - 1)) continue;   // mean-field to the end: no windows needed
    if (!allWindows) allWindows = cellFieldContinuous(world, S.q);
    p.wins = [];
    for (const e of allWindows) {
      if (e.structure !== p.structure || e.state !== p.state) continue;
      let tEndSec = (e.tEnd - S.q.frameIndex) * S.expSec + S.q.tSec;   // back to seconds (toFov mapped it)
      if (p.budget > 0) tEndSec = Math.min(tEndSec, e.aux * p.budget / p.emissionPerSec);
      const b = (tEndSec - S.q.tSec) / S.expSec;
      if (b > 0) p.wins.push({ ...e, tStart: (0 - S.q.tSec) / S.expSec, tEnd: b });
    }
  }
  allWindows = null;
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

  const maps = noiseMaps(S.seed, W, H, S.cam);
  const imagerPerFrame = S.labels.reduce((sum, L) => sum + L.imagerBackgroundPerPxPerSec(um), 0) * S.expSec;
  const bg = S.p.backgroundPhotons + imagerPerFrame;
  let blinks = 0;
  const POP_NAME = { [EVENT_STATE.ALWAYS_ON]: 'WideField dyes', [EVENT_STATE.INITIAL_ON]: 'initial ON', [EVENT_STATE.PRE]: 'pre state' };
  for (let f = 0; f < N; f++) {
    const frameBlinks0 = blinks, paths = [];
    const img = new Float32Array(W * H).fill(bg);
    const zf = zAt(f), [dx, dy] = dPx(f);
    groups.forEach((g, gi) => {
      if (g.role !== 'main') return;
      const evs = buckets[gi][f].map(i => byGroup[gi][i]);
      blinks += evs.length;
      renderPhotonImage(W, H, evs, f, { pixelSizeNm: pixelNm, photonsPerBlink: g.perFrame, psfSigmaPx: g.sigmaPx }, g.kernel, zf, img, dx, dy);
    });
    for (const p of pops) {
      const tf0 = S.t0Sec + f * S.expSec, tf1 = tf0 + S.expSec;
      if (isMeanField(p, f)) {
        if (!p.mf) {
          p.mf = meanFieldImage(world, S, 1 << p.structure, p.g.kernel, p.g.lambdaNm, driftMarginUm);
          p.image = p.mf.image;
        }
        let image = p.image;
        if (drift) {
          // DriftMeanField: the images at the two grid foci around dz, interpolated and shifted by the xy drift.
          const d = S.drift[f], [k, w] = driftGridWeights(driftGrid, d.z);
          const at = kk => p.mf.imagesAt(S.q.zRefUm + zStage - driftGridDzNm(driftGrid, kk) / 1000.0);
          const i0 = at(k), i1 = w !== 0 && k + 1 < driftGrid.n ? at(k + 1) : null, pitchNm = p.mf.pitchUm * 1000.0;
          image = new Float32Array(W * H);
          renderShiftedImages(i0, i1, i1 ? w : 0.0, d.x / pitchNm, d.y / pitchNm, image);
        }
        const m = Math.fround(meanPhotons(p.rate, p.lambda, tf0, tf1));
        for (let i = 0; i < img.length; i++) img[i] += Math.fround(m * image[i]);
        p.meanFieldFrames++;
        paths.push(`${POP_NAME[p.state]}: mean-field (FFT)`);
      } else {
        advanceAcc(p, f, zf, dx, dy);
        const perFrame = p.rate * S.expSec;
        for (let i = 0; i < img.length; i++) if (p.acc[i] !== 0) img[i] += Math.fround(perFrame * p.acc[i]);
        // Windows that start or end inside this frame: their overlap.
        const partial = p.wins.filter(e => e.tStart < f + 1 && e.tEnd > f && !(e.tStart <= f && e.tEnd >= f + 1));
        renderPhotonImage(W, H, partial, f, { pixelSizeNm: pixelNm, photonsPerBlink: perFrame, psfSigmaPx: p.g.sigmaPx }, p.g.kernel, zf, img, dx, dy);
        p.perDyeFrames++;
        paths.push(`${POP_NAME[p.state]}: per dye (${p.wins.length} windows)`);
      }
    }
    // opts.onPhotons(f, photons): the photon image goes there instead of the noise chain and onFrame (C++
    // FluorescenceFrameOptions::onPhotons; the combined light adds the lamp and runs its own noise chain).
    if ((opts.onPhotons ? opts.onPhotons(f, img) : onFrame(f, applyNoiseChain(img, S.cam, maps, f), img)) === false) break;
    // Which backend drew this frame: the SMLM splat for blinks, mean-field or per dye for each continuous population.
    if (opts.onProgress) opts.onProgress('frames', (f + 1) / N, {
      frame: f, frames: N, blinks: blinks - frameBlinks0, backends: [...(anyBlinks ? [`SMLM: ${blinks - frameBlinks0} blinks (splat)`] : []), ...paths] });
  }
  return {
    width: W, height: H, frames: N, blinks: events.length, renderedBlinks: blinks, psfSec: (tPsf - t0) / 1000, querySec,
    totalSec: (performance.now() - t0) / 1000, psf: groups.some(g => g.kernel) ? 'GibsonLanniZernike' : 'Gaussian',
    imagerBackgroundPerPxPerFrame: imagerPerFrame, driftNm: driftInfo(S),
    labels: S.labels.map((L, s) => ({ structure: STRUCTURES[s].id, dye: L.dye.id, mode: L.mode, kActPerSec: L.kActPerSec,
      states: Object.fromEntries(Object.entries(L.states).map(([k, v]) => [k, v && { lambdaNm: v.lambdaNm,
        detectedFraction: v.detectedFraction, detectedPerSec: v.detectedPerSec }])) })),
    groups: groups.map(g => ({ structure: g.structure, role: g.role, lambdaNm: g.lambdaNm, perFrame: g.perFrame })),
    populations: pops.map(p => ({ structure: p.structure, state: p.state, dyes: p.nZ, inSlab: p.nSlab,
      meanFieldFrames: p.meanFieldFrames, perDyeFrames: p.perDyeFrames })),
  };
}
