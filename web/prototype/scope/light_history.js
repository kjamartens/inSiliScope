// The movie's own clock map (2026-10-15, spec/ALGORITHM.md "Exact light history"): the JS reference for the map
// adapter/inSiliScope/Simulation/LightHistory.cpp builds for a cli/viewer movie under a shaped beam (ScopeMovie.cpp
// FluorescenceMovie::Begin, ownMap): the sample lit by the movie's light at its pose from T = 0 -- the past start-sec,
// then the movie, one record -- so every dye's kinetic clock is read at its own (x, y, z) by the core's map queries
// (world.js eventsInMap / continuousInMap; core ABI 14). A host with a history (the MM adapter) builds its maps from
// its records and chapters; this is that builder's case of one chapter and one open record, number for number.
//
// The record's footprint is the beam profile at the nodes ((i + 1/2) h) of the chapter's lattice h (lightLatticeUm),
// out to LIGHT_REACH of its peak, in sheared coordinates u = x - z tan theta_s under a tilted (HILO) beam; a Flat
// beam's is 1 over its field stop (here the query and a tile more). Per structure (encodeClockMap's layout):
//   GLOBAL      DNA-PAINT, the off targets: T, the label's rows;
//   DOSE        dSTORM, WideField: tau = F x footprint x depth(z) x T (the open record from tOpen = 0);
//   ACTIVATION  PALM: the hazard spont + lin F footprint depth + quad F^2 footprint^2 depth^2 per second;
// and the host's bleach hazard of a WideField dye or a PALM pre state (bleachRate: F x the state's emission over its
// photon budget x footprint x its depth; bleachEnd).
import { CLOCK_GLOBAL, CLOCK_DOSE, CLOCK_ACTIVATION, encodeClockMap, evalField } from './clockmap.js';
import { sampleAt } from './spectra.js';

export const LIGHT_TILE_UM = 0.25, LIGHT_LATTICE_ERR = 1e-3, LIGHT_REACH = 1e-4, LIGHT_DEPTH_STEP_UM = 0.005;
export const LIGHT_LIT_MARGIN_UM = 2.5;

// A chapter's node spacing under a beam (LightHistory.cpp LightLatticeUm): LIGHT_TILE_UM, halved (to 1/16 of it) while
// h^2 / 8 x the profile's largest curvature exceeds LIGHT_LATTICE_ERR.
export function lightLatticeUm(ex) {
  const k = ex.maxCurvature();
  let h = LIGHT_TILE_UM;
  while (h > LIGHT_TILE_UM / 16 && h * h / 8 * k > LIGHT_LATTICE_ERR) h /= 2;
  return h;
}

const nodeAt = (i, h) => (i + 0.5) * h;
const nodeAtOrAbove = (v, h) => Math.ceil(v / h - 0.5);
const nodeAtOrBelow = (v, h) => Math.floor(v / h - 0.5);
const KIN = L => ({ ...L.label.kinetics });

// The map of a movie of setup S (scope_movie.js scopeSetup) over its query box: { m (clockmap.js's object, for the
// host's reads), buffer (world.setClockMap's) }.
export function movieClockMap(S) {
  const EX = S.excite, um = S.p.pixelSizeNm / 1000.0, W = S.W, H = S.H, x = S.O('x'), y = S.O('y'), q = S.q;
  // The light at the sample (ScopeSampleLines) and whether it reaches it (MakeLightState).
  let total = 0.0;
  for (const l of S.lp.lasers) {
    const I = l.kWPerCm2 * (1 - sampleAt(S.lp.dichroic.T, l.nm));
    if (I > 0) total += I;
  }
  const lit = total > 0 && !EX.clipped;
  const flat = EX.profile === 0, tilted = !EX.tirf && EX.tanS !== 0.0, sheared = !flat && tilted;
  const h = flat ? LIGHT_TILE_UM : lightLatticeUm(EX), F = EX.F, tanS = EX.tanS, cosI = EX.cosI;
  const sx = sheared ? tanS : 0.0;
  // The pose (the FOV centre pixel's centre, the focal plane) and a Flat beam's stop (the FOV and its margin, grown to
  // the query: no dye of the movie is beyond it).
  const ax = x - W * um / 2 + (W - 1) / 2.0 * um, ay = y - H * um / 2 + (H - 1) / 2.0 * um, pivot = EX.spec.pivotUm ?? 0.0;
  const stop = [Math.min(x - W * um / 2 - LIGHT_LIT_MARGIN_UM, q.x0Um - 1.0), Math.min(y - H * um / 2 - LIGHT_LIT_MARGIN_UM, q.y0Um - 1.0),
    Math.max(x + W * um / 2 + LIGHT_LIT_MARGIN_UM, q.x1Um + 1.0), Math.max(y + H * um / 2 + LIGHT_LIT_MARGIN_UM, q.y1Um + 1.0)];
  let z0 = q.zHalfRangeUm > 0 ? q.zCullCentreUm - q.zHalfRangeUm : -1e3, z1 = q.zHalfRangeUm > 0 ? q.zCullCentreUm + q.zHalfRangeUm : 1e3;
  z0 = Math.max(z0, -2.0);
  z1 = Math.min(z1, 20.0);
  if (!(z1 >= z0)) z1 = z0;

  const m = { grids: [], depths: [], s: [] };
  // The footprint (MakeFootprint) over the nodes the query reads (Build's nodeRect), and its square.
  const grids = {};
  const footprint = sq => {
    if (!lit) return -1;
    if (sq in grids) return grids[sq];
    let r;
    const au = sheared ? ax - pivot * tanS : ax;
    if (flat)
      r = [Math.ceil(stop[0] / LIGHT_TILE_UM - 1.5), Math.ceil(stop[1] / LIGHT_TILE_UM - 1.5),
        Math.floor(stop[2] / LIGHT_TILE_UM + 0.5), Math.floor(stop[3] / LIGHT_TILE_UM + 0.5)];
    else {
      const R = EX.reachAt(LIGHT_REACH), Rx = R / cosI;
      r = [nodeAtOrAbove(au - Rx, h), nodeAtOrAbove(ay - R, h), nodeAtOrBelow(au + Rx, h), nodeAtOrBelow(ay + R, h)];
    }
    let ua = q.x0Um, ub = q.x1Um;
    if (sheared) {
      ua = Math.min(q.x0Um - z0 * tanS, q.x0Um - z1 * tanS);
      ub = Math.max(q.x1Um - z0 * tanS, q.x1Um - z1 * tanS);
    }
    const ia = Math.max(nodeAtOrBelow(ua, h) - 1, r[0]), ib = Math.min(nodeAtOrAbove(ub, h) + 1, r[2]);
    const ja = Math.max(nodeAtOrBelow(q.y0Um, h) - 1, r[1]), jb = Math.min(nodeAtOrAbove(q.y1Um, h) + 1, r[3]);
    let idx = -1;
    if (r[2] >= r[0] && r[3] >= r[1] && ib >= ia && jb >= ja) {
      const g = { x0: nodeAt(ia, h), y0: nodeAt(ja, h), h, nx: ib - ia + 1, ny: jb - ja + 1, v: [] };
      for (let j = ja; j <= jb; j++) {
        const dy = nodeAt(j, h) - ay;
        for (let i = ia; i <= ib; i++) {
          const f = flat ? 1.0 : EX.profileAt((nodeAt(i, h) - au) * cosI, dy);
          g.v.push(sq ? f * f : f);
        }
      }
      idx = m.grids.length;
      m.grids.push(g);
    }
    return (grids[sq] = idx);
  };
  // TIRF's depth factor of a state's lines on LIGHT_DEPTH_STEP_UM (and its square).
  const depths = {};
  const depthFor = (shares, key, sq) => {
    if (!lit || !EX.tirf) return -1;
    const k = `${key}:${sq ? 1 : 0}`;
    if (k in depths) return depths[k];
    let dMax = 0;
    for (const l of shares) dMax = Math.max(dMax, EX.decayUm(l.nm));
    const top = Math.max(LIGHT_DEPTH_STEP_UM, Math.min(Math.max(z1, 0.0), 30.0 * dMax));
    const n = Math.ceil(top / LIGHT_DEPTH_STEP_UM) + 1, d = { z0: 0, dz: LIGHT_DEPTH_STEP_UM, v: [] };
    for (let i = 0; i < n; i++) {
      const v = EX.depth(i * LIGHT_DEPTH_STEP_UM, shares);
      d.v.push(sq ? v * v : v);
    }
    m.depths.push(d);
    return (depths[k] = m.depths.length - 1);
  };
  const term = (grid, depth, scale) => ({ grid, depth, ox: 0, oy: 0, zp: 0, sx, sy: 0, scale });

  S.labels.forEach((L, st) => {
    const c = { mode: CLOCK_GLOBAL, hist: { tStart: [0], kin: [KIN(L)] }, bound: [], total: [], totalC: 0, rate: [],
      rateC: 0, tOpen: 0, steps: [], bleachTotal: [], bleachRate: [] };
    m.s.push(c);
    const on = !L.off && S.mounted.includes(st);
    const main = L.states.main;
    if (!on || !(L.mode === 'dSTORM' || L.mode === 'WideField' || L.mode === 'PALM')) {
      c.rateC = 1;
      return;
    }
    if (L.mode !== 'PALM') {
      c.mode = CLOCK_DOSE;
      if (lit && main && main.excitationPerSec > 0) {
        const g = footprint(false);
        if (g >= 0) c.rate.push(term(g, depthFor(main.lineShares, `${st}m`, false), F * (main.excitationPerSec / main.excitationPerSec) * 1.0));
      }
    } else {
      c.mode = CLOCK_ACTIVATION;
      const a = L.activationParts || { spont: 0, lin: 0, quad: 0 };
      c.rateC = a.spont;
      if (lit) {
        const shares = main ? main.lineShares : [];
        if (a.lin !== 0) {
          const g = footprint(false);
          if (g >= 0) c.rate.push(term(g, depthFor(shares, `${st}m`, false), a.lin * F * 1.0));
        }
        if (a.quad !== 0) {
          const g = footprint(true);
          if (g >= 0) c.rate.push(term(g, depthFor(shares, `${st}m`, true), a.quad * (F * F) * (1.0 * 1.0)));
        }
      }
    }
    // The photon-budget bleach hazard (a WideField dye, a PALM pre state).
    const pre = L.mode === 'PALM';
    if (!(L.mode === 'WideField' || (pre && L.label.preState))) return;
    const sp = pre ? L.states.pre : main, budget = pre ? L.prePhotonBudget : L.photonBudget;
    const lam = lit && sp && budget > 0 && sp.emissionPerSec > 0 ? sp.emissionPerSec / budget : 0.0;
    if (lam > 0) {
      const g = footprint(false);
      if (g >= 0) c.bleachRate.push(term(g, depthFor(sp.lineShares, `${st}${pre ? 'p' : 'm'}`, false), F * lam * 1.0));
    }
  });
  return { m, buffer: encodeClockMap(m) };
}

// The T at which a dye at (x, y, z) (world um) with draw aux has used its photon budget (LightHistory.cpp
// ClockMapView::BleachEnd): Infinity when it never does, -Infinity when it had by tOpen and no light reaches it now.
export function bleachEnd(m, st, x, y, z, aux) {
  const c = m.s[st];
  if (!c.bleachTotal.length && !c.bleachRate.length) return Infinity;
  const B = evalField(m, c.bleachTotal, x, y, z), r = evalField(m, c.bleachRate, x, y, z);
  if (r > 0) return c.tOpen + (aux - B) / r;
  return aux > B ? Infinity : -Infinity;
}
