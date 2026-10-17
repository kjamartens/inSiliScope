// The mean field under a clock map (2026-10-15, spec/ALGORITHM.md "Exact light history"): the JS reference for
// ScopeMovie.cpp's map mean field (ClockPairAt .. BlinkPhotons, MapEntriesFor, Survey, the weighted populations'
// frames) and WidefieldRender.cpp's bleach basis over a weight map (BuildBleachChannels, BleachCoefficients,
// AnchorDecay, RenderDecayed, MappedWeight, WidefieldImages::Render), the arithmetic and its operand order mirrored.
//
// A population's mean-field image under a map weights each dye with its photons per dye in the frame at its own clock:
// the weights live on the lattice of the structure's map (its finest grid's nodes, one beyond the scene's grid), per
// height band where the map changes with height; a grid cell reads them log-bilinearly between the four nodes around
// it (a Chebyshev term's rate linearly), or takes its own node's weight where a corner is missing, holds no weight or
// belongs to another bleach group. A rate population's frames decay each entry at its own rate (a basis of <= 8 rate
// groups, or Chebyshev terms in the rate); a blink population's weights are made per frame.
//
// LICENSE: BSD-3-Clause (see LICENSE at the repository root)
import { CLOCK_GLOBAL, CLOCK_DOSE, CLOCK_ACTIVATION, evalField } from './clockmap.js';
import { expectedBlinkOnSeconds } from './blink_expectation.js';
import { EVENT_STATE } from './dyes.js';
import { LIGHT_TILE_UM } from './light_history.js';
import { usedWeights } from './widefield.js';

// ScopeMovie.cpp: the basis' decay span, the window survey's floor, MapEntries' bands, the survey's raster.
export const BASIS_SPAN_SEC = 4.0, WINDOW_ALIVE_MIN = 1e-4;
const MAP_BAND_TOL = 0.01, MAP_BANDS = 64, MAP_RASTER_UM = 1.0, SURVEY_STEP_UM = 1.0, SURVEY_HEIGHTS = 4;
// WidefieldRender.h: the basis' groups, Chebyshev terms and RenderDecayed's rate bins.
const MAX_GROUPS = 8, CHEB_TERMS = 12, RATE_BINS = 64;
const NONE = 0xFFFFFFFF;

// ---- a population on a map's clock (ScopeMovie.cpp) ----

// PopulationLambda: a continuous population's decay rate under a label's physics (per second).
export function populationLambda(state, L) {
  if (state === EVENT_STATE.ALWAYS_ON) return L.photonBudget > 0 ? L.states.main.emissionPerSec / L.photonBudget : 0;
  if (state === EVENT_STATE.INITIAL_ON) {
    const t = L.label.kinetics.initialOnSec;
    return t > 0 ? 1 / t : Infinity;
  }
  return L.kActPerSec + (L.prePhotonBudget > 0 ? L.states.pre.emissionPerSec / L.prePhotonBudget : 0);
}

// EpochLambdas: each epoch's (a movie's own map: one epoch, the current labels).
export const epochLambdas = (m, labels, st, state) => m.s[st].hist.tStart.map(() => populationLambda(state, labels[st]));

const fieldsOf = c => [c.total, c.rate, c.bleachTotal || [], c.bleachRate || [], ...c.bound, ...c.steps.map(q => q.field)];

// MapZDependent: whether a structure's clock changes with the height (TIRF's depth tables, HILO's shear).
export function mapZDependent(m, st) {
  return fieldsOf(m.s[st]).some(f => f.some(t => t.depth >= 0 || t.sx !== 0.0 || t.sy !== 0.0));
}

// MapLatticeUm: the node spacing of structure st's map, its finest grid's (LIGHT_TILE_UM without grids).
export function mapLatticeUm(m, st) {
  let h = LIGHT_TILE_UM;
  for (const f of fieldsOf(m.s[st])) for (const t of f) if (t.grid >= 0) h = Math.min(h, m.grids[t.grid].h);
  return h;
}

// ClockMapView::Hazard: the activation hazard a dye at (x, y, z) has had by T = t.
function hazard(m, c, x, y, z, t) {
  let H = 0;
  for (const q of c.steps) {
    if (!(t > q.t0)) break;
    const inc = evalField(m, q.field, x, y, z, q.c);
    H += t >= q.t1 ? inc : inc * ((t - q.t0) / (q.t1 - q.t0));
  }
  if (t > c.tOpen) H += evalField(m, c.rate, x, y, z, c.rateC) * (t - c.tOpen);
  return H;
}

// ClockPairAt: a place's clock pair (world um) {a, b} and, DOSE, its epochs' bounds. blink: the blinks' (PALM: the
// activation alone), else a continuous population's (PALM's pre state: with its bleaching).
export function clockPairAt(m, st, blink, x, y, z) {
  const c = m.s[st];
  if (c.mode === CLOCK_GLOBAL) return { a: c.tOpen, b: 1.0, bounds: [] };
  if (c.mode === CLOCK_DOSE)
    return { a: evalField(m, c.total, x, y, z, c.totalC), b: evalField(m, c.rate, x, y, z, c.rateC),
      bounds: c.bound.map(f => evalField(m, f, x, y, z)) };
  let a = hazard(m, c, x, y, z, c.tOpen), b = evalField(m, c.rate, x, y, z, c.rateC);
  if (!blink) {
    a += evalField(m, c.bleachTotal || [], x, y, z);
    b += evalField(m, c.bleachRate || [], x, y, z);
  }
  return { a, b, bounds: [] };
}

// PopulationDecay: a continuous population's decay rate per second at a place's rate b.
export function populationDecay(c, lam, b) {
  if (c.mode === CLOCK_ACTIVATION) return b;
  const r = c.mode === CLOCK_GLOBAL ? 1.0 : b, l = lam.length ? lam[lam.length - 1] : 0.0;
  return r > 0 && Number.isFinite(l) ? l * r : 0.0;
}

// PopulationAt: its survival S at the sample's time T and its decay rate h per second.
export function populationAt(c, lam, a, b, bounds, T) {
  if (c.mode === CLOCK_ACTIVATION) return { S: Math.exp(-(a + b * (T - c.tOpen))), h: b };
  const global = c.mode === CLOCK_GLOBAL;
  const tau = global ? T : a + b * (T - c.tOpen);
  const n = Math.min(c.hist.tStart.length, lam.length);
  const start = e => e === 0 ? 0.0 : global ? c.hist.tStart[e] : bounds[e - 1];
  let H = 0.0;
  for (let e = 0; e < n; e++) {
    const d = (e + 1 < n ? Math.min(tau, start(e + 1)) : tau) - start(e);
    if (d > 0) H += lam[e] * d;
  }
  const S = Math.exp(-H);
  return { S, h: S > 0 ? populationDecay(c, lam, b) : 0.0 };
}

// DecayWindow: the mean of exp(-h t) over [0, w) times w.
const decayWindow = (h, w) => h > 0 ? -Math.expm1(-h * w) / h : w;

// PopulationPhotons: its mean photons per dye in [T, T + expSec).
export function populationPhotons(c, lam, rate, a, b, bounds, T, expSec) {
  if (c.mode === CLOCK_ACTIVATION)
    return rate * Math.exp(-(a + Math.max(0.0, b) * (T - c.tOpen))) * decayWindow(Math.max(0.0, b), expSec);
  const global = c.mode === CLOCK_GLOBAL;
  if (!global && !(b > 0)) return 0.0;   // frozen: no light
  if (lam.length === 1) {
    if (!Number.isFinite(lam[0])) return 0.0;
    const r = global ? 1.0 : b, tau = global ? T : a + b * (T - c.tOpen);
    return rate * Math.exp(-lam[0] * tau) * decayWindow(lam[0] * r, expSec);
  }
  const { S, h } = populationAt(c, lam, a, b, bounds, T);
  if (!(S > 0)) return 0.0;
  return rate * S * decayWindow(h, expSec);
}

// KinSegment: a kinetics row as a blink segment from t0 on.
const kinSegment = (k, t0) => ({ tStart: t0, activationRatePerSec: k.activationRatePerSec, onSec: k.onSec, offSec: k.offSec,
  bleachProb: k.bleachProb, initialOnSec: k.initialOnSec });

// BlinkClockAt: a blink population at a place's clock pair: its clock t at T, the clock's rate and its segments.
export function blinkClockAt(c, a, b, bounds, T) {
  const h = c.hist;
  if (c.mode === CLOCK_GLOBAL) return { t: T, rate: 1.0, segs: h.kin.map((k, e) => kinSegment(k, h.tStart[e])) };
  if (c.mode === CLOCK_DOSE)
    return { t: a + b * (T - c.tOpen), rate: b, segs: h.kin.map((k, e) => kinSegment(k, e === 0 ? 0.0 : bounds[e - 1])) };
  const segs = [kinSegment(h.kin[h.kin.length - 1], 0.0)];
  if (b > 0) {
    segs[0].activationRatePerSec = b;
    return { t: a / b + (T - c.tOpen), rate: 1.0, segs };
  }
  segs[0].activationRatePerSec = c.tOpen > 0 ? a / c.tOpen : 0.0;
  if (c.tOpen > 0) segs.push({ ...segs[0], tStart: c.tOpen, activationRatePerSec: 0.0 });
  return { t: T, rate: 1.0, segs };
}

// BlinkPhotons: its photons per dye in the exposure from clock t (clock rate r).
export const blinkPhotons = (mode, rate, segs, t, r, expSec) =>
  r > 0 ? rate * expectedBlinkOnSeconds(mode, segs, t, t + r * expSec) / r : 0.0;

// ---- MapEntriesFor: a population's weight entries on its scene's grid gr ----
// { ntx, nty, nb, edges, a, b, nBounds, bounds, rates (Float32Array; null: decay0 everywhere), decay0, cellMap }; the
// cell map (WidefieldWeightMap): each grid cell's node (tile), its stencil's corner (NONE: beyond the lattice) and its
// place between the nodes (fx, fy).
export function mapEntriesFor(m, p, gr, t0Sec, spanSec, expSec) {
  const T = mapLatticeUm(m, p.structure);
  const X = i => gr.x0Um + (i + 0.5) * gr.pitchUm, Y = j => gr.y0Um + (j + 0.5) * gr.pitchUm;
  let tx0 = 0, ty0 = 0, ntx = 0, nty = 0;
  if (gr.nx && gr.ny) {
    tx0 = Math.floor(X(0) / T) - 1;
    ty0 = Math.floor(Y(0) / T) - 1;
    ntx = Math.floor(X(gr.nx - 1) / T) - tx0 + 2;
    nty = Math.floor(Y(gr.ny - 1) / T) - ty0 + 2;
  }
  const axis = (v, t0) => {
    const u = v / T - 0.5 - t0, fl = Math.floor(u);
    return { tile: Math.floor(v / T) - t0, lo: fl, f: Math.fround(u - fl) };
  };
  const ax = Array.from({ length: gr.nx }, (_, i) => axis(X(i), tx0));
  const ay = Array.from({ length: gr.ny }, (_, j) => axis(Y(j), ty0));
  const nc = gr.nx * gr.ny;
  const cellMap = { count: ntx * nty, nx: ntx, tile: new Uint32Array(nc), corner: new Uint32Array(nc),
    fx: new Float32Array(nc), fy: new Float32Array(nc) };
  for (let jy = 0; jy < gr.ny; jy++)
    for (let i = 0; i < gr.nx; i++) {
      const c = gr.nx * jy + i, sx = ax[i], sy = ay[jy];
      cellMap.tile[c] = sy.tile * ntx + sx.tile;
      cellMap.corner[c] = sx.lo + 1 < ntx && sy.lo + 1 < nty ? sy.lo * ntx + sx.lo : NONE;
      cellMap.fx[c] = sx.f;
      cellMap.fy[c] = sy.f;
    }
  const st = p.structure, blink = p.state === EVENT_STATE.BLINK, dz = gr.zPlaneUm, c = m.s[st];
  const zDep = mapZDependent(m, st) && dz > 0 && ntx > 0 && nty > 0;
  // The bands hold for [Tq, Tq + 2 span], Tq the movie's start on a grid of the span.
  const Tq = zDep && spanSec > 0 ? Math.floor(t0Sec / spanSec) * spanSec : 0.0;
  const nBounds = c.mode === CLOCK_DOSE ? c.bound.length : 0;
  const tileX = tx => (tx0 + tx + 0.5) * T, tileY = ty => (ty0 + ty + 0.5) * T;
  const valueAt = (pair, Tt) => {
    if (blink) {
      const bc = blinkClockAt(c, pair.a, pair.b, pair.bounds, Tt);
      return blinkPhotons(p.mode, p.rate, bc.segs, bc.t, bc.rate, expSec);
    }
    return populationPhotons(c, p.lam, p.rate, pair.a, pair.b, pair.bounds, Tt, expSec);
  };
  // The bands: the grid's dye planes (centres (k + 1/2) dz, within the map's heights), merged while their photons per
  // dye differ by at most MAP_BAND_TOL of the largest in the span's first, middle and last frames.
  const edges = [], bandZ = [];
  if (zDep) {
    const k0 = Math.floor(Math.max(gr.zMinUm, -2.0) / dz), k1 = Math.floor(Math.min(gr.zMaxUm, 20.0) / dz);
    const pts = [];
    const step = Math.max(1, Math.round(MAP_RASTER_UM / T));
    for (let ty = 0; ty < nty; ty += step) for (let tx = 0; tx < ntx; tx += step) pts.push([tileX(tx), tileY(ty)]);
    const np = pts.length, nk = k1 >= k0 ? k1 - k0 + 1 : 0, Ts = [Tq, Tq + spanSec, Tq + 2 * spanSec];
    const V = new Float64Array(nk * np * 3);
    for (let kk = 0; kk < nk; kk++) {
      const z = (k0 + kk + 0.5) * dz;
      for (let i = 0; i < np; i++) {
        const pair = clockPairAt(m, st, blink, pts[i][0], pts[i][1], z);
        for (let t = 0; t < 3; t++) V[(kk * np + i) * 3 + t] = valueAt(pair, Ts[t]);
      }
    }
    let vMax = 0.0;
    for (const v of V) vMax = Math.max(vMax, Math.abs(v));
    for (let tol = MAP_BAND_TOL; nk; tol *= 2) {
      const starts = [0];
      let s0 = 0;
      for (let k = 1; k < nk; k++) {
        let fits = true;
        for (let i = 0; i < np * 3 && fits; i++) fits = Math.abs(V[k * np * 3 + i] - V[s0 * np * 3 + i]) <= tol * vMax;
        if (!fits) { starts.push(k); s0 = k; }
      }
      if (starts.length > MAP_BANDS) continue;
      starts.forEach((ks, j) => {
        const ke = j + 1 < starts.length ? starts[j + 1] - 1 : nk - 1;
        if (j > 0) edges.push((k0 + ks) * dz);
        bandZ.push((k0 + Math.floor((ks + ke) / 2) + 0.5) * dz);
      });
      break;
    }
  }
  if (!bandZ.length) bandZ.push(0.0);   // (any height: the same everywhere)
  const nb = bandZ.length, nt = ntx * nty, n = nt * nb;
  const a = new Float64Array(n), b = new Float64Array(n), bounds = new Float64Array(n * nBounds);
  for (let band = 0; band < nb; band++)
    for (let ty = 0; ty < nty; ty++)
      for (let tx = 0; tx < ntx; tx++) {
        const i = band * nt + ty * ntx + tx;
        const pair = clockPairAt(m, st, blink, tileX(tx), tileY(ty), bandZ[band]);
        a[i] = pair.a;
        b[i] = pair.b;
        for (let e = 0; e < nBounds; e++) bounds[i * nBounds + e] = pair.bounds[e];
      }
  // A rate population's decay per entry: one number, or the rates.
  let rates = null, decay0 = 0;
  if (!blink && n) {
    const r = new Float32Array(n), h0 = populationDecay(c, p.lam, b[0]);
    let equal = true;
    for (let i = 0; i < n; i++) {
      const h = populationDecay(c, p.lam, b[i]);
      r[i] = h;
      equal = equal && h === h0;
    }
    decay0 = h0;
    if (!equal) rates = r;
  }
  return { ntx, nty, nb, edges, a, b, nBounds, bounds, rates, decay0, cellMap, n };
}

// MovieCache::SlabQuantiles: the heights of structure s's dyes in [zLo, zHi) of the column at K quantiles.
function slabQuantiles(world, x0, y0, x1, y1, zLo, zHi, s, K) {
  const zs = [];
  world.forEachDye(x0, y0, x1, y1, -Infinity, Infinity, (bk, i) => { if (bk.structure === s) zs.push(bk.z[i]); });
  const v = Float64Array.from(zs).sort();
  const lowerBound = z => { let lo = 0, hi = v.length; while (lo < hi) { const mid = (lo + hi) >> 1; if (v[mid] < z) lo = mid + 1; else hi = mid; } return lo; };
  const lo = lowerBound(zLo), hi = lowerBound(zHi), out = [];
  for (let k = 0; hi > lo && k < K; k++) out.push(v[lo + Math.trunc((k + 0.5) / K * (hi - lo))]);
  return out;
}

// Survey: on a SURVEY_STEP_UM raster of the query at its dyes' heights, the share of the places the light reaches now,
// their mean survival at frame 0 and the decay rate averaged with the survival as its weight (the mean-field switch).
// excite: the population's factor in the FOV frame (null: uniform).
export function survey(m, p, S, world, excite, zMin, zMax) {
  const st = p.structure, c = m.s[st], q = S.q;
  let zs = [];
  if (mapZDependent(m, st)) zs = slabQuantiles(world, q.x0Um, q.y0Um, q.x1Um, q.y1Um, zMin, zMax, st, SURVEY_HEIGHTS);
  if (!zs.length) zs.push(q.zCullCentreUm);
  let n = 0, lit = 0, sumS = 0, sumSH = 0;
  for (let y = q.y0Um + SURVEY_STEP_UM / 2; y < q.y1Um; y += SURVEY_STEP_UM)
    for (let x = q.x0Um + SURVEY_STEP_UM / 2; x < q.x1Um; x += SURVEY_STEP_UM)
      for (const z of zs) {
        ++n;
        if (excite && !(excite(x - q.originXUm, y - q.originYUm, (z - q.zRefUm) * 1000.0) > 0)) continue;
        const pair = clockPairAt(m, st, false, x, y, z);
        if (c.mode === CLOCK_DOSE && !(pair.b > 0)) continue;
        lit++;
        const { S: Sv, h } = populationAt(c, p.lam, pair.a, pair.b, pair.bounds, S.t0Sec);
        sumS += Sv;
        sumSH += Sv * h;
      }
  p.surveyed = true;
  p.nZLit = n ? Math.round(p.nZ * lit / n) : 0;
  p.meanSurvival = lit ? sumS / lit : 0.0;
  p.surveyRate = sumS > 0 ? sumSH / sumS : 0.0;
}

// ---- the bleach basis over a weight map (WidefieldRender.cpp) ----

// ChebExp: Chebyshev coefficients (y in [-1, 1]) of exp(-tau (y + 1) / 2), n terms, by interpolation at the nodes.
export function chebExp(tau, n) {
  const c = new Array(n).fill(0.0);
  for (let j = 0; j < n; j++) {
    let s = 0.0;
    for (let k = 0; k < n; k++) {
      const th = Math.PI * (k + 0.5) / n;
      s += Math.exp(-tau * (Math.cos(th) + 1.0) / 2.0) * Math.cos(j * th);
    }
    c[j] = (j === 0 ? 1.0 : 2.0) * s / n;
  }
  return c;
}

// ChebTermsFor: the terms that hold exp(-tau (y + 1) / 2) to 1e-6: 3 .. CHEB_TERMS.
export function chebTermsFor(tau) {
  const c = chebExp(Math.max(0.0, tau), 2 * CHEB_TERMS);
  let tail = 0.0, m = c.length;
  while (m > 3 && tail + Math.abs(c[m - 1]) <= 1e-6) tail += Math.abs(c[--m]);
  return Math.min(m, CHEB_TERMS);
}

const logF = w => w > 0 ? Math.fround(Math.log(w)) : -Infinity;

// A scene's bleach basis over its weight map (cellMap), its bands (edges, world um between them, on dye planes of
// dz): the channels of BuildBleachChannels and their coefficients.
export class WeightBasis {
  constructor(cellMap, edges, dz) {
    this.map = cellMap;
    this.edges = edges;
    this.dz = dz;
    this.serial = 0;
    this.valid = false;
    this.channels = [];
  }
  rateOf(i) { return this.rates ? this.rates[i] : 1.0; }   // (no host rates: the uniform frame dose, one group)
  // BuildBleachChannels(wb, single) with SetWeightRates(rates, rateTau).
  build(wb, rates, single, rateTau) {
    const n = wb.length;
    this.serial++;
    this.valid = true;
    this.wbRef = Float32Array.from(wb);
    this.rates = rates && rates.length === n ? rates : null;
    const hostRates = this.rates !== null;
    this.cheb = false;
    this.channels = [];
    this.doseMax = 0.0;
    const groupOf = i => single ? 0.0 : this.rateOf(i);
    const groups = [];
    for (let i = 0; i < n; i++) {
      if (wb[i] === 0) continue;
      this.doseMax = Math.max(this.doseMax, this.rateOf(i));
      if (groups.length <= MAX_GROUPS && !groups.includes(groupOf(i))) groups.push(groupOf(i));
    }
    if (groups.length <= MAX_GROUPS) {
      groups.sort((x, y) => x - y);
      for (const g of groups) {
        const w = new Float32Array(n);
        for (let i = 0; i < n; i++) if (wb[i] !== 0 && groupOf(i) === g) w[i] = wb[i];
        this.channels.push({ cheb: -1, weight: w, logw: w.map(logF) });
      }
      // Each weight's channel and each channel's largest |weight| (the first in index order).
      this.cellChannel = new Int32Array(n).fill(-1);
      this.channelMax = new Array(this.channels.length).fill(0);
      for (let i = 0; i < n; i++)
        for (let g = 0; g < this.channels.length; g++) {
          const w = this.channels[g].weight;
          if (w[i] !== 0) {
            this.cellChannel[i] = g;
            if (Math.abs(w[i]) > Math.abs(w[this.channelMax[g]])) this.channelMax[g] = i;
            break;
          }
        }
      return;
    }
    this.cheb = true;
    const terms = hostRates ? chebTermsFor(rateTau * this.doseMax) : CHEB_TERMS;
    for (let j = 0; j < terms; j++) this.channels.push({ cheb: j, weight: new Float32Array(n) });
    // The entry the coefficients read the decay from (the largest weight x rate), the largest weight and each rate
    // bin's (RenderDecayed).
    this.chebRef = n;
    this.wbRefMax = 0.0;
    this.rateBinMax = new Float64Array(RATE_BINS);
    let best = 0.0;
    for (let i = 0; i < n; i++) {
      const mm = wb[i] * this.rateOf(i);
      if (mm > best) { best = mm; this.chebRef = i; }
      const w = Math.abs(wb[i]);
      this.wbRefMax = Math.max(this.wbRefMax, w);
      const u = this.doseMax > 0 ? Math.max(0.0, this.rateOf(i)) / this.doseMax : 0.0;
      const bin = Math.min(RATE_BINS - 1, Math.floor(u * RATE_BINS));
      this.rateBinMax[bin] = Math.max(this.rateBinMax[bin], w);
    }
    this.lwRef = this.wbRef.map(logF);
    for (let i = 0; i < n; i++) {
      if (wb[i] === 0) continue;
      const y = 2.0 * this.rateOf(i) / this.doseMax - 1.0;
      let t0 = 1.0, t1 = y;
      for (let j = 0; j < terms; j++) {
        let v = j === 0 ? t0 : (j === 1 ? t1 : 0.0);
        if (j >= 2) { v = 2.0 * y * t1 - t0; t0 = t1; t1 = v; }
        this.channels[j].weight[i] = wb[i] * v;
      }
    }
  }
  anchorCoefficients() { return this.channels.map((_, j) => this.cheb ? (j === 0 ? 1.0 : 0.0) : 1.0); }
  // BleachCoefficients: wb's coefficients in the basis (null: not a combination of it to 1e-5 of its largest weight).
  coefficients(wb) {
    if (!this.valid || wb.length !== this.wbRef.length) return null;
    if (!this.channels.length) return [];
    const n = wb.length, ch = this.channels;
    let a;
    if (!this.cheb) {
      a = ch.map((c, g) => { const i = this.channelMax[g]; return c.weight[i] !== 0 ? wb[i] / c.weight[i] : 0.0; });
    } else {
      const iRef = this.chebRef;
      if (iRef >= n) return null;
      const r = wb[iRef] / this.wbRef[iRef];
      if (!(r > 0.0)) return null;
      a = chebExp(-Math.log(r) * this.doseMax / this.rateOf(iRef), ch.length);
    }
    let wMax = 0.0;
    for (let i = 0; i < n; i++) wMax = Math.max(wMax, Math.abs(wb[i]));
    const tol = 1e-5 * Math.max(wMax, 1e-30);
    if (!this.cheb) {
      for (let i = 0; i < n; i++) {
        const g = this.cellChannel[i], v = g < 0 ? 0.0 : a[g] * ch[g].weight[i];
        if (Math.abs(wb[i] - v) > tol) return null;
      }
      return a;
    }
    for (let i = 0; i < n; i++) {
      let v = 0.0;
      for (let j = 0; j < ch.length; j++) v += a[j] * ch[j].weight[i];
      if (Math.abs(wb[i] - v) > tol) return null;
    }
    return a;
  }
  // AnchorDecay: wb0 held in the Chebyshev basis, as {serial, d0, dev} (null: not).
  anchorDecay(wb0) {
    if (!this.cheb || !this.channels.length || !this.coefficients(wb0)) return null;
    const iRef = this.chebRef, r = wb0[iRef] / this.wbRef[iRef];
    const d0 = -Math.log(r) / this.rateOf(iRef);
    let dev = 0.0;
    for (let i = 0; i < wb0.length; i++)
      if (wb0[i] !== 0 || this.wbRef[i] !== 0)
        dev = Math.max(dev, Math.abs(wb0[i] - this.wbRef[i] * Math.exp(-d0 * this.rateOf(i))));
    return { serial: this.serial, d0, dev };
  }
  // RenderDecayed's coefficients: frame k of the anchor's decay while the series holds it (null: render the weights).
  decayed(an, k) {
    if (!this.cheb || !this.valid || !this.channels.length || an.serial !== this.serial || !(this.doseMax > 0)) return null;
    const d = an.d0 + k, x = d * this.doseMax, m = this.channels.length;
    const c = chebExp(x, 2 * CHEB_TERMS);
    let tail = 0.0;
    for (let j = m; j < c.length; j++) tail += Math.abs(c[j]);
    let low = 0.0;
    for (let b = 0; b < RATE_BINS; b++)
      if (this.rateBinMax[b] > 0.0) low = Math.max(low, this.rateBinMax[b] * Math.exp(-d * this.doseMax * (b + 1.0) / RATE_BINS));
    if (!(an.dev + 2.0 * tail * this.wbRefMax <= 1e-5 * (low - an.dev))) return null;
    return chebExp(x, m);
  }
  // CellValue's band offset of dye plane k (upper_bound of its centre in the edges).
  bandOf(k) {
    const zc = (k + 0.5) * this.dz;
    let i = 0;
    while (i < this.edges.length && this.edges[i] <= zc) i++;
    return i;
  }
  // MappedWeight of channel j over the grid cells in band `band` (index).
  cellWeights(j, band) {
    const M = this.map, ch = this.channels[j], nc = M.count, out = new Float32Array(M.tile.length);
    const off = ch.weight.length > nc && nc > 0 ? nc * band : 0;
    for (let cell = 0; cell < out.length; cell++) out[cell] = this.mappedWeight(ch, cell, off, ch.weight[M.tile[cell] + off]);
    return out;
  }
  mappedWeight(ch, cell, band, w) {
    const M = this.map, c0 = M.corner[cell];
    if (c0 === NONE) return w;
    const i0 = band + c0, idx = [i0, i0 + 1, i0 + M.nx, i0 + M.nx + 1];
    const cheb = ch.cheb >= 0, ref = cheb ? this.wbRef : ch.weight, lw = cheb ? this.lwRef : ch.logw;
    if (ref[idx[0]] === ref[idx[1]] && ref[idx[0]] === ref[idx[2]] && ref[idx[0]] === ref[idx[3]] &&
        (!cheb || (this.rateOf(idx[0]) === this.rateOf(idx[1]) && this.rateOf(idx[0]) === this.rateOf(idx[2]) &&
                   this.rateOf(idx[0]) === this.rateOf(idx[3]))))
      return w;   // (one value: the tile's, exactly)
    const fx = M.fx[cell], fy = M.fy[cell];
    const b = [(1.0 - fx) * (1.0 - fy), fx * (1.0 - fy), (1.0 - fx) * fy, fx * fy];
    let l = 0.0, r = 0.0;
    for (let q = 0; q < 4; q++) {
      if (b[q] === 0.0) continue;
      const v = lw[idx[q]];
      if (!(v > -Infinity)) return w;   // a corner without weight (unlit, another group)
      l += b[q] * v;
      if (cheb) r += b[q] * this.rateOf(idx[q]);
    }
    if (!cheb) return Math.fround(Math.exp(l));
    // Chebyshev term j at the cell's rate.
    const y = 2.0 * r / this.doseMax - 1.0;
    let t0 = 1.0, t1 = y, v = 1.0;
    for (let j = 0; j <= ch.cheb; j++) {
      v = j === 0 ? t0 : (j === 1 ? t1 : 0.0);
      if (j >= 2) { v = 2.0 * y * t1 - t0; t0 = t1; t1 = v; }
    }
    return Math.fround(Math.exp(l) * v);
  }
}

// WidefieldImages::Render: frame = per camera pixel the sum over its upscale x upscale cells of max(0, sum_j a_j I_j)
// (float), the images I_j of the basis' channels (null: none).
export function renderBasis(imgs, a, W, H, u, cw) {
  const cam = new Float32Array(W * H);
  let nb = 0;
  for (let j = 0; j < imgs.length && j < a.length; j++) if (imgs[j] && a[j] !== 0) nb = j + 1;
  if (nb === 0) return cam;
  const af = a.slice(0, nb).map(Math.fround);
  for (let Y = 0; Y < H; Y++)
    for (let X = 0; X < W; X++) {
      let acc = 0.0;
      for (let sy = 0; sy < u; sy++) {
        const row = (Y * u + sy) * cw + X * u;
        for (let sx = 0; sx < u; sx++) {
          let v = 0.0;
          for (let j = 0; j < nb; j++) if (imgs[j]) v = Math.fround(v + Math.fround(af[j] * imgs[j][row + sx]));
          acc += Math.max(0.0, v);
        }
      }
      cam[Y * W + X] = acc;
    }
  return cam;
}

// ---- a weighted population's frames (FluorescenceMovie::Impl: BuildMeanField's weighted part, WeightedImage,
// RateWeightedFrame, BlinkWeightedFrame, DriftMeanField) ----
// scene: widefield.js meanFieldScene of the population's structure; m: the map; p: the population (structure, state,
// lam, rate, mode); S: the movie's setup.
export class MapMeanField {
  constructor(scene, m, p, S) {
    this.scene = scene;
    this.m = m;
    this.p = p;
    this.S = S;
    const gr = scene.g, c = m.s[p.structure];
    this.blink = p.state === EVENT_STATE.BLINK;
    const E = this.E = mapEntriesFor(m, p, gr, S.t0Sec, Math.max(BASIS_SPAN_SEC, S.N * S.expSec), S.expSec);
    this.basis = new WeightBasis(E.cellMap, E.edges, gr.zPlaneUm);
    this.imageCache = { serial: -1, imgs: null };
    this.driftImages = new Map();
    this.driftAnchor = null;
    this.anchor = null;
    if (this.blink) {
      this.used = usedWeights(scene, E.cellMap, E.edges, E.n);
      this.wb0 = this.blinkWeights(0);
      this.rates = null;
      this.rateTau = 0.0;
    } else {
      const wb0 = new Float32Array(E.n);
      for (let i = 0; i < E.n; i++)
        wb0[i] = populationPhotons(c, p.lam, p.rate, E.a[i], E.b[i], this.boundsOf(i), S.t0Sec, S.expSec);
      this.wb0 = wb0;
      this.rates = E.rates;
      this.decay = E.decay0;
      this.rateTau = Math.max(BASIS_SPAN_SEC, S.N * S.expSec);
      // Several frames: the whole basis at once, anchored at frame 0.
      if (S.N > 1) {
        this.anchor = this.basis.anchorDecay(wb0);
        if (!this.anchor && !this.basis.coefficients(wb0)) {
          this.basis.build(wb0, this.rates, false, this.rateTau);
          this.anchor = this.basis.anchorDecay(wb0);
        }
      }
    }
    this.image = this.renderFrame(this.wb0);   // WeightedImage
  }
  boundsOf(i) {
    const nb = this.E.nBounds;
    return nb ? this.E.bounds.subarray(i * nb, i * nb + nb) : [];
  }
  // BlinkWeights: a blink population's photons per dye in frame f, per entry its dye cells read (used; the others 0).
  blinkWeights(f) {
    const E = this.E, S = this.S, c = this.m.s[this.p.structure], wb = new Float32Array(E.n);
    for (let i = 0; i < E.n; i++) {
      if (!this.used[i]) continue;
      const bc = blinkClockAt(c, E.a[i], E.b[i], this.boundsOf(i), S.t0Sec);
      wb[i] = blinkPhotons(this.p.mode, this.p.rate, bc.segs, bc.t + bc.rate * (f * S.expSec), bc.rate, S.expSec);
    }
    return wb;
  }
  decayedWeights(k) {
    const wb = Float32Array.from(this.wb0);
    for (let i = 0; i < wb.length; i++) if (wb[i] !== 0 && k !== 0) wb[i] = this.wb0[i] * Math.exp(-k * this.rates[i]);
    return wb;
  }
  // Channel j's weights in dye plane k: the mapped weights of its band and the plane factor (null: an unlit plane).
  planeWeights(j, bandCache) {
    const sc = this.scene, B = this.basis;
    return k => {
      const pf = sc.planeFactor ? sc.planeFactor(k) : null;
      if (sc.planeFactor && !pf) return null;
      const band = B.bandOf(k);
      let w = bandCache.get(band);
      if (!w) { w = B.cellWeights(j, band); bandCache.set(band, w); }
      return { w, pf };
    };
  }
  channelImages(setup, keep) {
    return this.basis.channels.map((_, j) => setup.channel(this.planeWeights(j, new Map()), keep));
  }
  combine(a) {
    if (this.imageCache.serial !== this.basis.serial) {
      this.imageCache = { serial: this.basis.serial, imgs: this.channelImages(this.scene.base, false).map(c => c && c.img) };
    }
    const s = this.scene.base;
    return renderBasis(this.imageCache.imgs, a, this.S.W, this.S.H, s.u, s.cw);
  }
  // RenderFrame: the basis' coefficients of wb, or a new basis of it.
  renderFrame(wb) {
    let a = this.basis.coefficients(wb);
    if (!a) {
      this.basis.build(wb, this.rates, !!this.rates && !this.basis.valid, this.rateTau);
      a = this.basis.anchorCoefficients();
    }
    return this.combine(a);
  }
  // RateWeightedFrame (no drift): each entry f exposures on at its own rate.
  rateFrame(f) {
    if (f === 0) return this.image;
    const k = f * this.S.expSec;
    const a = this.anchor ? this.basis.decayed(this.anchor, k) : null;
    if (a) return this.combine(a);
    return this.renderFrame(this.decayedWeights(k));
  }
  blinkFrame(f) { return this.renderFrame(this.blinkWeights(f)); }
  // DriftMeanField: frame f's image with the sample moved by d (focus grid foci around zBase - dz), into cam (+=).
  driftFrame(f, d, at, kk0, w, nGrid, cam, shifted) {
    let wb = this.wb0;
    if (this.rates && f > 0) wb = this.decayedWeights(f * this.S.expSec);
    if (!this.driftAnchor || !this.basis.coefficients(wb)) {
      this.driftAnchor = wb;
      this.driftImages.clear();
      this.basis.build(this.driftAnchor, this.rates, !this.rates, this.rateTau);
    }
    const imagesAt = kk => {
      const focus = at(kk);
      let im = this.driftImages.get(focus);
      if (!im) {
        const setup = this.scene.setupAt(focus);
        im = { ...setup, specs: this.channelImages(setup, true).map(c => c && c.spec) };
        this.driftImages.set(focus, im);
      }
      return im;
    };
    const i0 = imagesAt(kk0), i1 = w !== 0 && kk0 + 1 < nGrid ? imagesAt(kk0 + 1) : null;
    const coef = this.basis.coefficients(wb) || this.basis.anchorCoefficients();
    const pitchNm = this.scene.g.pitchUm * 1000.0;
    shifted(i0, i1, i1 ? w : 0.0, d.x / pitchNm, d.y / pitchNm, cam, coef);
  }
}
