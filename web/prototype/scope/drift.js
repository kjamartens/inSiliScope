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
export const driftOn = s => s.xyNmPerSqrtSec > 0 || s.zNmPerSqrtSec > 0;

// The step from frame f - 1 to frame f (f >= 1), nm: x, y, z drawn in that order.
export function driftStep(seed32, f, frameSec, s) {
  const u = new CounterRng(seed32, f);
  u.pixel(0);
  const gx = counterGauss(u);
  const gy = counterGauss(u);
  const gz = counterGauss(u);
  const r = Math.sqrt(Math.max(0.0, frameSec));
  return { x: s.xyNmPerSqrtSec * r * gx, y: s.xyNmPerSqrtSec * r * gy, z: s.zNmPerSqrtSec * r * gz };
}

// d(0) = 0, d(f) = d(f - 1) + driftStep(f): [{x, y, z}] nm per frame.
export function driftTrajectory(seed, frames, frameSec, s) {
  const t = Array.from({ length: Math.max(0, frames) }, () => ({ x: 0, y: 0, z: 0 }));
  if (!driftOn(s)) return t;
  const seed32 = driftSeed(seed);
  for (let f = 1; f < frames; f++) {
    const st = driftStep(seed32, f, frameSec, s), p = t[f - 1];
    t[f] = { x: p.x + st.x, y: p.y + st.y, z: p.z + st.z };
  }
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
