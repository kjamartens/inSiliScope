// Random-walk sample drift, mirroring adapter/inSiliScope/Simulation/Drift.cpp: every frame adds an
// independent normal step per axis, variance sigma^2 x frame time (sigma = RMS nm after 1 s, x and y each
// sigma_xy, z sigma_z), summed from 0 at frame 0 (Cnossen et al., Opt. Express 29, 27961 (2021); Ma et al.,
// Sci. Adv. 10, eadm7765 (2024)). Counter-based draws: step f is a pure function of (seed, f).
// Also the focus grid (z drift in WideField/BrightField; the shift itself: widefield.js shiftedCrop).
// Part of the JS reference for imaging (web/prototype/scope/README.md); embedded in the webSMLM block.
import { CounterRng, counterGauss } from './rng.js';

export const DRIFT_FOCUS_STEP_NM = 10.0;

// seed ^ "DRFT" as uint32 (ToInt32 keeps the low 32 bits of the integer seed, as the C++ cast does).
export const driftSeed = seed => (Math.trunc(seed) ^ 0x44524654) >>> 0;
// DriftSettings with its defaults: random walk (RMS nm after 1 s), directed part (mean speeds; the xy angle, < 0 =
// random per seed; the z direction, 1 up / -1 down / 0 random per seed, a negative z speed reversing it; the swings of
// the xy and z directions, deg; the strength wander, and the correlation time).
export const DRIFT_DEFAULTS = { xyNmPerSqrtSec: 0, zNmPerSqrtSec: 0, xySpeedNmPerSec: 0, zSpeedNmPerSec: 0, xyAngleDeg: -1,
  zDirection: 0, angleWanderDeg: 180, zAngleWanderDeg: 90, speedWanderPct: 0, wanderTimeSec: 60 };
export const driftOn = s => s.xyNmPerSqrtSec > 0 || s.zNmPerSqrtSec > 0 || (s.xySpeedNmPerSec || 0) !== 0 ||
  (s.zSpeedNmPerSec || 0) !== 0;

// The random-walk step from frame f - 1 to frame f (f >= 1), nm: x, y, z drawn in that order.
export function driftStep(seed32, f, frameSec, s) {
  const u = new CounterRng(seed32, f);
  u.pixel(0);
  const gx = counterGauss(u);
  const gy = counterGauss(u);
  const gz = counterGauss(u);
  const r = Math.sqrt(Math.max(0.0, frameSec));
  return { x: s.xyNmPerSqrtSec * r * gx, y: s.xyNmPerSqrtSec * r * gy, z: s.zNmPerSqrtSec * r * gz };
}

// erf(x / sqrt 2) = 2 Phi(x) - 1: a unit normal mapped to [-1, 1] (Abramowitz & Stegun 7.1.26; Drift.cpp DriftSwing).
export function driftSwing(x) {
  const z = Math.abs(x) / Math.sqrt(2.0);
  const t = 1.0 / (1.0 + 0.3275911 * z);
  const poly = ((((1.061405429 * t - 1.453152027) * t + 1.421413741) * t - 0.284496736) * t + 0.254829592) * t;
  const e = 1.0 - poly * Math.exp(-z * z);
  return x < 0.0 ? -e : e;
}

// DriftWalker (Drift.cpp): zero at frame f0, then step() per frame. Directed velocity at the frame's start:
// v_xy = V_xy max(0, 1 + w s_xy) along theta0 + A_xy swing(phi), v_z = +/-V_z max(0, 1 + w s_z) cos(A_z swing(psi));
// phi, s_xy, s_z unit-variance Ornstein-Uhlenbeck (correlation time wanderTimeSec) on pixel 1 of each frame's stream,
// psi on pixel 3; theta0 then the z sign on pixel 2 of frame 0xFFFFFFFF; the random walk on pixel 0 (unchanged).
export class DriftWalker {
  constructor(seed, s, f0 = 0) {
    this.seed32 = driftSeed(seed); this.s = Object.assign({}, DRIFT_DEFAULTS, s); this.f = f0; this.d = { x: 0, y: 0, z: 0 };
    const a = new CounterRng(this.seed32, 0xFFFFFFFF);
    a.pixel(2);
    this.theta0 = 2.0 * Math.PI * a.uniform();
    this.zUp = a.uniform() < 0.5;
    const u = new CounterRng(this.seed32, f0);
    u.pixel(1);
    this.phi = counterGauss(u);
    this.sxy = counterGauss(u);
    this.sz = counterGauss(u);
    const v = new CounterRng(this.seed32, f0);
    v.pixel(3);
    this.psi = counterGauss(v);
  }
  step(frameSec) {
    const s = this.s, dt = Math.max(0.0, frameSec);
    const theta = (s.xyAngleDeg >= 0.0 ? s.xyAngleDeg * Math.PI / 180.0 : this.theta0) + s.angleWanderDeg * Math.PI / 180.0 * driftSwing(this.phi);
    const w = s.speedWanderPct / 100.0;
    const vxy = s.xySpeedNmPerSec * Math.max(0.0, 1.0 + w * this.sxy);
    const up = s.zDirection > 0 ? 1.0 : s.zDirection < 0 ? -1.0 : (this.zUp ? 1.0 : -1.0);
    const vz = up * s.zSpeedNmPerSec * Math.max(0.0, 1.0 + w * this.sz) *
      Math.cos(s.zAngleWanderDeg * Math.PI / 180.0 * driftSwing(this.psi));
    const vx = vxy * Math.cos(theta), vy = vxy * Math.sin(theta);
    this.f++;
    const st = driftStep(this.seed32, this.f, frameSec, s), d = this.d;
    this.d = { x: d.x + vx * dt + st.x, y: d.y + vy * dt + st.y, z: d.z + vz * dt + st.z };
    const u = new CounterRng(this.seed32, this.f);
    u.pixel(1);
    const gp = counterGauss(u);
    const gs = counterGauss(u);
    const gz = counterGauss(u);
    const a = s.wanderTimeSec > 0.0 ? Math.exp(-dt / s.wanderTimeSec) : 0.0;
    const b = Math.sqrt(Math.max(0.0, 1.0 - a * a));
    this.phi = a * this.phi + b * gp;
    this.sxy = a * this.sxy + b * gs;
    this.sz = a * this.sz + b * gz;
    const v = new CounterRng(this.seed32, this.f);
    v.pixel(3);
    this.psi = a * this.psi + b * counterGauss(v);
    return this.d;
  }
}

// d(0) = 0, then one DriftWalker step per frame: [{x, y, z}] nm per frame.
export function driftTrajectory(seed, frames, frameSec, s) {
  const t = Array.from({ length: Math.max(0, frames) }, () => ({ x: 0, y: 0, z: 0 }));
  if (!driftOn(s)) return t;
  const walker = new DriftWalker(seed, s);
  for (let f = 1; f < frames; f++) t[f] = walker.step(frameSec);
  return t;
}

export function driftRange(t) {
  const b = { xLo: 0, xHi: 0, yLo: 0, yHi: 0, zLo: 0, zHi: 0 };
  for (const d of t) {
    b.xLo = Math.min(b.xLo, d.x); b.xHi = Math.max(b.xHi, d.x);
    b.yLo = Math.min(b.yLo, d.y); b.yHi = Math.max(b.yHi, d.y);
    b.zLo = Math.min(b.zLo, d.z); b.zHi = Math.max(b.zHi, d.z);
  }
  return b;
}
export const driftMaxXyNm = b => Math.max(Math.max(-b.xLo, b.xHi), Math.max(-b.yLo, b.yHi));

// DriftWidenZCull: [centre, half] grown to hold every frame's window.
export function driftWidenZCull(b, centreUm, halfUm) {
  return [centreUm - (b.zLo + b.zHi) / 2000.0, halfUm + (b.zHi - b.zLo) / 2000.0];
}

// DriftFocusGrid: foci dz = (k0 + k) x step, k = 0 .. n-1.
export function driftFocusGrid(b) {
  const lo = Math.floor(b.zLo / DRIFT_FOCUS_STEP_NM), hi = Math.ceil(b.zHi / DRIFT_FOCUS_STEP_NM);
  return { k0: lo, n: hi - lo + 1 };
}
export const driftGridDzNm = (g, k) => (g.k0 + k) * DRIFT_FOCUS_STEP_NM;
// [k, w]: image(dz) = (1 - w) image(k) + w image(k + 1).
export function driftGridWeights(g, dzNm) {
  if (g.n <= 1) return [0, 0.0];
  const t = dzNm / DRIFT_FOCUS_STEP_NM - g.k0;
  const k = Math.min(g.n - 2, Math.max(0, Math.floor(t)));
  return [k, Math.min(1.0, Math.max(0.0, t - k))];
}
