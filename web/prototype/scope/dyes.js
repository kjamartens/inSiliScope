// Dyes on microtubules and their blink kinetics: a line-for-line mirror of core/src/dyes.cpp (and
// BuildMtFrames from core/src/microtubules.cpp). Address-based (pcg4d), so a dye and its blinks are the
// same whatever window, block or order generates them. The JS reference for imaging: change the
// photophysics HERE first (web/lab), port to dyes.cpp when merging (web/lab/README.md).
import { pcgA, pcg4d, unit, hashUnit } from './rng.js';

export const MT_RADIUS_NM = 12.5;
export const MT_N_PROTOFILAMENTS = 13;
export const MT_DIMER_NM = 8;
export const MT_LATTICE_START = 3;
export const MT_BINDER_NM = 12;
export const MT_LINKER_MIN_NM = 2;
export const MT_LINKER_MAX_NM = 5;
export const MT_CH_SEAM_PHASE = 8000000;
export const DYE_SALT = 0x9E3779B9;
export const DYE_BLOCK_UM = 1.0;
// Per-site purpose channels (the 4th pcg4d word). Issue 16 added FLUOR (fluorescent fraction), INIT_ON (dSTORM
// initial ON time), AUX (unit-exponential bleach draw of a continuously emitting dye), ORIENT_U/ORIENT_PHI (Random
// orientation) and reserved MOTION (SPT) and OFFTARGET (off-target binding); 13-15 are free.
export const DYE_CH = { LABEL: 0, LINK_U: 1, LINK_PHI: 2, LINK_R: 3, ACT: 4, PERSIST: 5, FLUOR: 6, INIT_ON: 7, AUX: 8,
  ORIENT_U: 9, ORIENT_PHI: 10, MOTION: 11, OFFTARGET: 12, SCHED0: 16, SCHED_STRIDE: 8,
  ON: 0, BRIGHT1: 1, BRIGHT2: 2, BLEACH: 3, OFF: 4 };
export const DYE_MAX_BLINKS = 1000;
export const PERSIST_BIN_SEC = 1.0;
export const PERSIST_ON_CAP = 20.0;
const NM = 1e-3;
const LINK_MIN_UM = MT_LINKER_MIN_NM * NM, LINK_MAX_UM = MT_LINKER_MAX_NM * NM;

export const DEFAULT_KINETICS = { activationRatePerSec: 0.01, onSec: 0.05, offSec: 1.0, bleachProb: 1.0, photonCV: 0.5 };

// ---- labels (issue 16) ----
// A structure's label: which sites carry a dye, and how that dye emits. The world model knows no spectra or light:
// the imaging side turns a dye + light path into these rates (scope/dye_library.js).
//   density              fraction of the structure's binding sites with a label (the LABEL draw)
//   fluorescentFraction  fraction of labels whose dye is fluorescent (the FLUOR draw; 1 = no draw)
//   mode                 'dSTORM' | 'PALM' | 'DNA-PAINT' | 'WideField' (LABEL_MODES)
//   kinetics             DEFAULT_KINETICS fields, plus initialOnSec (dSTORM: ON at t = 0 for Exp(initialOnSec))
//   preState             PALM: the dye emits in a pre state until its first activation (photoconversion)
//   orientation          { mode: 'Free' | 'Fixed' | 'Random', polarDeg, azimuthDeg, wobbleDeg } (dyeOrientation)
//   motion               'Static' (SPT: future; the MOTION channel is reserved)
//   offTarget            [] (future: [{ structure, fraction }] of the dye stuck elsewhere; OFFTARGET reserved)
export const LABEL_MODES = ['dSTORM', 'PALM', 'DNA-PAINT', 'WideField'];
export const ORIENTATION_MODES = ['Free', 'Fixed', 'Random'];
export const MOTION_MODES = ['Static'];
// Event states: a blink of the main state, the pre-state window (PALM), the dSTORM initial ON window, the always-on
// window of a WideField dye. The last three are continuous (one window per dye from t = 0).
export const EVENT_STATE = { BLINK: 0, PRE: 1, INITIAL_ON: 2, ALWAYS_ON: 3 };

export function makeLabel(o = {}) {
  const l = {
    density: o.density ?? 0.7, fluorescentFraction: o.fluorescentFraction ?? 1.0, mode: o.mode ?? 'DNA-PAINT',
    kinetics: { ...DEFAULT_KINETICS, initialOnSec: 0, ...(o.kinetics || {}) }, preState: !!o.preState,
    orientation: { mode: 'Free', polarDeg: 90, azimuthDeg: 0, wobbleDeg: 0, ...(o.orientation || {}) },
    motion: o.motion ?? 'Static', offTarget: o.offTarget ?? [],
  };
  validateLabel(l);
  return l;
}
export function validateLabel(l) {
  if (!LABEL_MODES.includes(l.mode)) throw new Error(`label mode '${l.mode}' is not one of ${LABEL_MODES.join(', ')}`);
  if (!ORIENTATION_MODES.includes(l.orientation.mode))
    throw new Error(`orientation '${l.orientation.mode}' is not one of ${ORIENTATION_MODES.join(', ')}`);
  if (!MOTION_MODES.includes(l.motion)) throw new Error(`motion '${l.motion}' is not implemented yet (only Static)`);
  if (l.offTarget.length) throw new Error('off-target binding is not implemented yet (offTarget must be empty)');
  if (l.preState && l.mode !== 'PALM') throw new Error('a pre state needs mode PALM');
}

const normOr1 = n => (n === 0 || Number.isNaN(n)) ? 1 : n;

// Per-segment tangent T, parallel-transported normal U, binormal V, cumulative length (um).
export function buildMtFrames(pts) {
  const n = pts.length;
  const f = { cum: [], T: [], U: [], V: [] };
  if (n < 2) { f.cum = new Array(n).fill(0); f.length = 0; return f; }
  f.cum.push(0);
  let prevU = { x: 0, y: 0, z: 0 };
  for (let i = 0; i + 1 < n; i++) {
    const dx = pts[i + 1].x - pts[i].x, dy = pts[i + 1].y - pts[i].y, dz = pts[i + 1].z - pts[i].z;
    const len = Math.hypot(dx, dy, dz);
    f.cum.push(f.cum[i] + len);
    let t;
    if (len > 1e-12) t = { x: dx / len, y: dy / len, z: dz / len };
    else t = i > 0 ? f.T[i - 1] : { x: 1, y: 0, z: 0 };
    let u = i === 0 ? (Math.abs(t.z) < 0.9 ? { x: 0, y: 0, z: 1 } : { x: 1, y: 0, z: 0 }) : prevU;
    const d = u.x * t.x + u.y * t.y + u.z * t.z;
    u = { x: u.x - d * t.x, y: u.y - d * t.y, z: u.z - d * t.z };
    const ul = normOr1(Math.hypot(u.x, u.y, u.z));
    u = { x: u.x / ul, y: u.y / ul, z: u.z / ul };
    f.T.push(t); f.U.push(u);
    f.V.push({ x: t.y * u.z - t.z * u.y, y: t.z * u.x - t.x * u.z, z: t.x * u.y - t.y * u.x });
    prevU = u;
  }
  f.length = f.cum[n - 1];
  return f;
}

// First segment whose end reaches arc length S (clamped to the last).
export function mtSegmentAt(fr, S) {
  let lo = 0, hi = fr.T.length - 1;
  while (lo < hi) {
    const mid = (lo + hi) >>> 1;
    if (fr.cum[mid + 1] < S) lo = mid + 1; else hi = mid;
  }
  return lo;
}

export function pointAtArc(pts, fr, S) {
  const seg = mtSegmentAt(fr, S);
  const f = S - fr.cum[seg], t = fr.T[seg];
  return { x: pts[seg].x + t.x * f, y: pts[seg].y + t.y * f, z: pts[seg].z + t.z * f };
}

export const mtProtofilamentTheta = (phase, k) => phase + k * 2 * Math.PI / MT_N_PROTOFILAMENTS;
export const mtProtofilamentOffsetNm = k => (k * MT_LATTICE_START * MT_DIMER_NM / MT_N_PROTOFILAMENTS) % MT_DIMER_NM;
export const mtSeamPhase = (seed, cx, cy, mtIndex) => 2 * Math.PI * hashUnit(seed, cx, cy, MT_CH_SEAM_PHASE + mtIndex);
export const dyeH1 = (seed, cx, cy, mtIndex) => pcgA((seed ^ DYE_SALT) >>> 0, cx >>> 0, cy >>> 0, mtIndex >>> 0);

function siteDye(pts, fr, seg, S, ct, st, minL3, maxL3, r1, r2, r3) {
  const t = fr.T[seg], u = fr.U[seg], v = fr.V[seg];
  const f = S - fr.cum[seg];
  const cx = pts[seg].x + t.x * f, cy = pts[seg].y + t.y * f, cz = pts[seg].z + t.z * f;
  const rx = ct * u.x + st * v.x, ry = ct * u.y + st * v.y, rz = ct * u.z + st * v.z;
  const B = (MT_RADIUS_NM + MT_BINDER_NM) * NM;
  const tx = cx + rx * B, ty = cy + ry * B, tz = cz + rz * B;
  // mtDisplaceByLinker: uniform direction, radius uniform in volume.
  const lu = r1 * 2 - 1, phi = r2 * 2 * Math.PI, sn = Math.sqrt(1 - lu * lu);
  const r = Math.cbrt(minL3 + (maxL3 - minL3) * r3);
  return { x: tx + r * sn * Math.cos(phi), y: ty + r * sn * Math.sin(phi), z: tz + r * lu };
}

// Fluorescent dyes of block `blockIndex` (arc [b, b+1) um) of one microtubule; appends
// {pos (cell-local), mtIndex, k, n, id, theta}. A site is labelled when its LABEL draw u < density, and its dye is
// fluorescent when (fluorescentFraction >= 1 or) its FLUOR draw < fluorescentFraction: both nested, so a lower
// density or fraction keeps a subset of the same dyes. theta: the protofilament's azimuth (the dye's local frame).
export function dyesInBlock(seed, cx, cy, mtIndex, pts, fr, blockIndex, density, fluorescentFraction, out) {
  if (pts.length < 2 || blockIndex < 0) return;
  const total = fr.length;
  const blockNm0 = blockIndex * DYE_BLOCK_UM * 1000;
  const labelledBelow = Math.min(1.0, Math.max(0.0, density));
  const ff = Math.min(1.0, Math.max(0.0, fluorescentFraction));
  if (blockNm0 * NM >= total || !(labelledBelow > 0) || !(ff > 0)) return;
  const h1 = dyeH1(seed, cx, cy, mtIndex);
  const phase = mtSeamPhase(seed, cx, cy, mtIndex);
  const minL3 = Math.pow(LINK_MIN_UM, 3), maxL3 = Math.pow(LINK_MAX_UM, 3);
  for (let k = 0; k < MT_N_PROTOFILAMENTS; k++) {
    const off = mtProtofilamentOffsetNm(k);
    const theta = mtProtofilamentTheta(phase, k);
    const ct = Math.cos(theta), st = Math.sin(theta);
    for (let n = Math.max(0, Math.floor((blockNm0 - off) / MT_DIMER_NM) - 1); ; n++) {
      const sNm = off + n * MT_DIMER_NM;
      const blk = Math.floor(sNm / (DYE_BLOCK_UM * 1000));
      if (blk < blockIndex) continue;
      if (blk > blockIndex) break;
      const S = sNm * NM;
      if (S >= total) break;
      const label = pcgA(h1, k, n, DYE_CH.LABEL);
      const u = unit(label);
      if (u >= labelledBelow) continue;
      if (ff < 1.0 && unit(pcgA(h1, k, n, DYE_CH.FLUOR)) >= ff) continue;
      const r1 = unit(pcgA(h1, k, n, DYE_CH.LINK_U));
      const r2 = unit(pcgA(h1, k, n, DYE_CH.LINK_PHI));
      const r3 = unit(pcgA(h1, k, n, DYE_CH.LINK_R));
      const pos = siteDye(pts, fr, mtSegmentAt(fr, S), S, ct, st, minL3, maxL3, r1, r2, r3);
      out.push({ pos, mtIndex, k, n, id: label, theta });
    }
  }
}

// Log-normal factor with mean 1 and CV cv from two uniforms (Box-Muller).
export function logNormalMean1(cv, u1, u2) {
  const s2 = Math.log(1 + cv * cv), sigma = Math.sqrt(s2), mu = -s2 / 2;
  const z = Math.sqrt(-2 * Math.log(u1)) * Math.cos(2 * Math.PI * u2);
  return Math.exp(mu + sigma * z);
}

function poissonInverse(m, expM, u) {
  let p = expM, F = p, c = 0;
  while (u > F && c < 1000) { c++; p *= m / c; F += p; }
  return c;
}

function poissonFromUniform(m, u, u2) {
  if (!(m > 0)) return 0;
  if (m > 30) {
    const z = Math.sqrt(-2 * Math.log(u)) * Math.cos(2 * Math.PI * u2);
    return Math.max(0, Math.floor(m + Math.sqrt(m) * z + 0.5));
  }
  return poissonInverse(m, Math.exp(-m), u);
}

// Whole blink lifetime of bleaching dye (k, n) of the microtubule with hash h1: [{tOn, tOff, brightness}],
// simulated seconds, in time order.
export function dyeSchedule(h1, k, n, kin, out = []) {
  if (!(kin.activationRatePerSec > 0)) return out;
  const U = ch => unit(pcgA(h1, k >>> 0, n >>> 0, ch));
  const pBleach = Math.min(1.0, Math.max(0.01, kin.bleachProb));
  const cv = Math.max(0.0, kin.photonCV);
  let t = -Math.log(U(DYE_CH.ACT)) / kin.activationRatePerSec;
  for (let j = 0; j < DYE_MAX_BLINKS; j++) {
    const base = DYE_CH.SCHED0 + j * DYE_CH.SCHED_STRIDE;
    const on = -Math.log(U(base + DYE_CH.ON)) * kin.onSec;
    let b = 1;
    if (cv > 0) {
      const u1 = U(base + DYE_CH.BRIGHT1);
      const u2 = U(base + DYE_CH.BRIGHT2);
      b = logNormalMean1(cv, u1, u2);
    }
    out.push({ tOn: t, tOff: t + on, brightness: b });
    if (U(base + DYE_CH.BLEACH) < pBleach) break;
    t += on - Math.log(U(base + DYE_CH.OFF)) * kin.offSec;
  }
  return out;
}

// Blinks of persistent site (k, n) starting in time bins [b0, b1] that keep(tOn, on) accepts, in (bin, j)
// order: emit(bin, j, tOn, on, brightness). Addressed per (site, bin, j).
export function persistentGen(h1, k, n, kin, b0, b1, keep, emit) {
  const rate = kin.activationRatePerSec;
  if (!(rate > 0)) return;
  const key = pcgA(h1, k >>> 0, n >>> 0, DYE_CH.PERSIST);
  const U = (bin, j, ch) => unit(pcgA(key, bin >>> 0, j >>> 0, ch));
  const COUNT = 0, COUNT2 = 1, START = 2, ON = 3, BRIGHT1 = 4, BRIGHT2 = 5;
  const cv = Math.max(0.0, kin.photonCV);
  const maxOn = PERSIST_ON_CAP * kin.onSec;
  const m = rate * PERSIST_BIN_SEC;
  const small = m <= 30;
  const expM = small ? Math.exp(-m) : 0.0;
  for (let b = Math.max(0, b0); b <= b1; b++) {
    const c = small ? poissonInverse(m, expM, U(b, 0, COUNT)) : poissonFromUniform(m, U(b, 0, COUNT), U(b, 0, COUNT2));
    for (let j = 0; j < c; j++) {
      const tOn = (b + U(b, j, START)) * PERSIST_BIN_SEC;
      const on = Math.min(maxOn, -Math.log(U(b, j, ON)) * kin.onSec);
      if (!keep(tOn, on)) continue;
      let br = 1;
      if (cv > 0) {
        const u1 = U(b, j, BRIGHT1);
        const u2 = U(b, j, BRIGHT2);
        br = logNormalMean1(cv, u1, u2);
      }
      emit(b, j, tOn, on, br);
    }
  }
}

// Blinks of persistent site (k, n) overlapping [t0, t1).
export function persistentBlinks(h1, k, n, kin, t0, t1, out = []) {
  if (!(t1 > t0)) return out;
  const maxOn = PERSIST_ON_CAP * kin.onSec;
  const b0 = Math.max(0, Math.floor((t0 - maxOn) / PERSIST_BIN_SEC));
  const b1 = Math.floor(t1 / PERSIST_BIN_SEC);
  persistentGen(h1, k, n, kin, b0, b1, (tOn, on) => tOn < t1 && tOn + on > t0,
    (bin, j, tOn, on, br) => out.push({ tOn, tOff: tOn + on, brightness: br }));
  return out;
}

// The emission of label dye (k, n) as two lists, simulated seconds:
//   blinks:     [{tOn, tOff, brightness}] of the main state (dSTORM, PALM; DNA-PAINT blinks come from persistentGen)
//   continuous: [{tOn, tOff, state, aux}] windows from t = 0 (PALM pre state until the first activation, dSTORM
//               initial ON, WideField always on); aux = a unit-exponential draw (AUX) the imaging side scales into the
//               dye's own bleach time (photon budget / emission rate).
// dSTORM: ON from 0 for Exp(initialOnSec) (none at 0), then dyeSchedule shifted to start there. PALM: dyeSchedule;
// its pre window ends at the first blink (never, at activation rate 0). Pure functions of the address.
export function labelSchedule(h1, k, n, label, blinks = [], continuous = []) {
  const kin = label.kinetics;
  const U = ch => unit(pcgA(h1, k >>> 0, n >>> 0, ch));
  const aux = () => -Math.log(U(DYE_CH.AUX));
  if (label.mode === 'WideField') {
    continuous.push({ tOn: 0, tOff: Infinity, state: EVENT_STATE.ALWAYS_ON, aux: aux() });
    return { blinks, continuous };
  }
  if (label.mode === 'DNA-PAINT') return { blinks, continuous };
  let shift = 0;
  if (label.mode === 'dSTORM' && kin.initialOnSec > 0) {
    shift = -Math.log(U(DYE_CH.INIT_ON)) * kin.initialOnSec;
    continuous.push({ tOn: 0, tOff: shift, state: EVENT_STATE.INITIAL_ON, aux: 0 });
  }
  const first = blinks.length;
  dyeSchedule(h1, k, n, kin, blinks);
  if (shift) for (let i = first; i < blinks.length; i++) { blinks[i].tOn += shift; blinks[i].tOff += shift; }
  if (label.mode === 'PALM' && label.preState)
    continuous.push({ tOn: 0, tOff: blinks.length > first ? blinks[first].tOn : Infinity, state: EVENT_STATE.PRE, aux: aux() });
  return { blinks, continuous };
}

// Mean emission dipole of dye (k, n): null for Free (isotropic, no draws). Fixed: polar angle from the microtubule
// axis T and azimuth about it from the dye's radial direction (the protofilament's, theta in the U/V frame), in the
// segment frame at the dye. Random: uniform on the sphere (ORIENT_U, ORIENT_PHI), cell-local. Returns
// {x, y, z, wobbleDeg} (cell-local unit vector; the dye wobbles fast within a cone of that half-angle).
// Not used by the renderer yet (polarisation: future); tested for its statistics.
export function dyeOrientation(h1, k, n, label, fr, seg, theta) {
  const o = label.orientation;
  if (o.mode === 'Free') return null;
  if (o.mode === 'Random') {
    const cz = 2 * unit(pcgA(h1, k >>> 0, n >>> 0, DYE_CH.ORIENT_U)) - 1;
    const phi = 2 * Math.PI * unit(pcgA(h1, k >>> 0, n >>> 0, DYE_CH.ORIENT_PHI)), sz = Math.sqrt(1 - cz * cz);
    return { x: sz * Math.cos(phi), y: sz * Math.sin(phi), z: cz, wobbleDeg: o.wobbleDeg };
  }
  const t = fr.T[seg], u = fr.U[seg], v = fr.V[seg];
  const ct = Math.cos(theta), st = Math.sin(theta);
  const r = { x: ct * u.x + st * v.x, y: ct * u.y + st * v.y, z: ct * u.z + st * v.z };   // radial
  const q = { x: t.y * r.z - t.z * r.y, y: t.z * r.x - t.x * r.z, z: t.x * r.y - t.y * r.x };  // T x r
  const pol = o.polarDeg * Math.PI / 180, az = o.azimuthDeg * Math.PI / 180;
  const sp = Math.sin(pol), cp = Math.cos(pol), ca = Math.cos(az), sa = Math.sin(az);
  return { x: cp * t.x + sp * (ca * r.x + sa * q.x), y: cp * t.y + sp * (ca * r.y + sa * q.y),
    z: cp * t.z + sp * (ca * r.z + sa * q.z), wobbleDeg: o.wobbleDeg };
}

export { pcg4d };
