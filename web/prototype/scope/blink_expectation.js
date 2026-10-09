// The expected ON time of one dye of a blinking label in [t0, t1) of its clock, the mirror of
// adapter/inSiliScope/Simulation/BlinkExpectation.cpp (the blink mean-field regime): dSTORM / PALM as the core's
// Markov chain (initial ON -> first dark at the activation rate -> ON -> bleached (p) | OFF -> ON; matrix exponential
// of the generator with an integral row), DNA-PAINT as the exact integral over its 1 s Poisson bins.
// segs: [{ tStart, activationRatePerSec, onSec, offSec, bleachProb, initialOnSec }], ascending, the first at 0.
const S_INIT = 0, S_DARK = 1, S_ON = 2, S_OFF = 3, S_BLEACHED = 4, S_INT = 5, NS = 6;
const INSTANT = 1e12, ON_CAP = 20.0, BIN_SEC = 1.0;

function matMul(a, b) {
  const out = new Float64Array(NS * NS);
  for (let i = 0; i < NS; ++i) for (let j = 0; j < NS; ++j) {
    let s = 0.0;
    for (let k = 0; k < NS; ++k) s += a[i * NS + k] * b[k * NS + j];
    out[i * NS + j] = s;
  }
  return out;
}

function expm(Q, T) {
  const A = new Float64Array(NS * NS);
  let norm = 0.0;
  for (let i = 0; i < NS; ++i) {
    let r = 0.0;
    for (let j = 0; j < NS; ++j) { A[i * NS + j] = Q[i * NS + j] * T; r += Math.abs(A[i * NS + j]); }
    norm = Math.max(norm, r);
  }
  let s = 0, scale = 1.0;
  while (norm > 0.5) { norm *= 0.5; scale *= 0.5; ++s; }
  let term = new Float64Array(NS * NS), E = new Float64Array(NS * NS);
  for (let i = 0; i < NS; ++i) for (let j = 0; j < NS; ++j) {
    A[i * NS + j] *= scale;
    term[i * NS + j] = i === j ? 1.0 : 0.0;
    E[i * NS + j] = term[i * NS + j];
  }
  for (let k = 1; k <= 16; ++k) {
    const next = matMul(term, A);
    for (let i = 0; i < NS * NS; ++i) { term[i] = next[i] / k; E[i] += term[i]; }
  }
  for (let q = 0; q < s; ++q) E = matMul(E, E);
  return E;
}

function propagate(v, Q, T) {
  if (!(T > 0.0)) return;
  const E = expm(Q, T), w = new Float64Array(NS);
  for (let j = 0; j < NS; ++j) { let s = 0.0; for (let i = 0; i < NS; ++i) s += v[i] * E[i * NS + j]; w[j] = s; }
  v.set(w);
}

const rateOf = meanSec => meanSec > 0.0 ? 1.0 / meanSec : INSTANT;

function generator(k) {
  const Q = new Float64Array(NS * NS), at = (i, j) => i * NS + j;
  const p = Math.min(1.0, Math.max(0.01, k.bleachProb)), rOn = rateOf(k.onSec);
  Q[at(S_INIT, S_DARK)] = k.initialOnSec > 0.0 ? 1.0 / k.initialOnSec : 0.0;
  Q[at(S_DARK, S_ON)] = Math.max(0.0, k.activationRatePerSec);
  Q[at(S_ON, S_BLEACHED)] = p * rOn;
  Q[at(S_ON, S_OFF)] = (1.0 - p) * rOn;
  Q[at(S_OFF, S_ON)] = rateOf(k.offSec);
  for (let i = 0; i < S_INT; ++i) {
    let out = 0.0;
    for (let j = 0; j < S_INT; ++j) if (j !== i) out += Q[at(i, j)];
    Q[at(i, i)] = -out;
  }
  Q[at(S_ON, S_INT)] = 1.0;
  return Q;
}

function markovOnSeconds(mode, segs, t0, t1) {
  const v = new Float64Array(NS);
  v[mode === 'dSTORM' && segs[0].initialOnSec > 0.0 ? S_INIT : S_DARK] = 1.0;
  for (let s = 0; s < segs.length; ++s) {
    const a = segs[s].tStart, b = s + 1 < segs.length ? segs[s + 1].tStart : Infinity;
    if (!(a < t1)) break;
    if (v[S_INIT] !== 0.0 && !(segs[s].initialOnSec > 0.0)) { v[S_DARK] += v[S_INIT]; v[S_INIT] = 0.0; }
    const Q = generator(segs[s]);
    if (a < t0) { propagate(v, Q, Math.min(b, t0) - a); v[S_INT] = 0.0; }
    const lo = Math.max(a, t0), hi = Math.min(b, t1);
    if (hi > lo) propagate(v, Q, hi - lo);
  }
  return v[S_INT];
}

function segmentAt(segs, t) {
  let lo = 0, hi = segs.length;
  while (hi - lo > 1) { const mid = (lo + hi) >> 1; if (segs[mid].tStart <= t) lo = mid; else hi = mid; }
  return lo;
}

function paintBinOverlap(b, tau, cap, t0, t1) {
  const inner = [t0, t1, t0 - cap, t1 - cap].sort((x, y) => x - y).filter(x => x > b && x < b + BIN_SEC);
  const cuts = [b, ...inner, b + BIN_SEC];
  let sum = 0.0;
  for (let i = 0; i + 1 < cuts.length; ++i) {
    const al = cuts[i], be = cuts[i + 1];
    if (!(be > al)) continue;
    const m = 0.5 * (al + be), lIsA = m >= t0, uIsCap = m + cap <= t1;
    const Lm = lIsA ? m : t0, Um = uIsCap ? m + cap : t1;
    if (!(Um > Lm)) continue;
    const a1 = lIsA ? tau * (be - al) : tau * tau * (Math.exp(-(t0 - be) / tau) - Math.exp(-(t0 - al) / tau));
    const a2 = uIsCap ? tau * Math.exp(-cap / tau) * (be - al) : tau * tau * (Math.exp(-(t1 - be) / tau) - Math.exp(-(t1 - al) / tau));
    sum += a1 - a2;
  }
  return sum;
}

function paintOnSeconds(segs, t0, t1) {
  let maxOn = 0.0;
  for (const k of segs) maxOn = Math.max(maxOn, ON_CAP * k.onSec);
  const b0 = Math.max(0, Math.floor((t0 - maxOn) / BIN_SEC) - 1), b1 = Math.floor(t1 / BIN_SEC);
  let sum = 0.0;
  for (let b = b0; b <= b1; ++b) {
    const k = segs[segmentAt(segs, b * BIN_SEC)];
    if (!(k.activationRatePerSec > 0.0) || !(k.onSec > 0.0)) continue;
    sum += k.activationRatePerSec * paintBinOverlap(b * BIN_SEC, k.onSec, ON_CAP * k.onSec, t0, t1);
  }
  return sum;
}

// mode: 'dSTORM' | 'PALM' | 'DNA-PAINT' | 'WideField' (0 for WideField).
export function expectedBlinkOnSeconds(mode, segs, t0, t1) {
  if (!segs.length || !(t1 > t0) || mode === 'WideField') return 0.0;
  if (mode === 'DNA-PAINT') return paintOnSeconds(segs, t0, t1);
  return markovOnSeconds(mode, segs, t0, t1);
}
