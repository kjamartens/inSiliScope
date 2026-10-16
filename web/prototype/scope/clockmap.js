// The clock map (ABI 14, spec/ALGORITHM.md "Exact light history"): each dye's kinetic clock read at its own place. A
// line-for-line mirror of core/src/clockmap.cpp (the arithmetic and its operand order; the C++ caches have no twin).
//
// Per structure a clock: GLOBAL (every dye on the sample's time T, the kinetics history in T; DNA-PAINT always reads
// its clock so), DOSE (a dye's clock is its dose: total(x) at tOpen, rate(x) per second after; epoch e >= 1 starts at
// its dose bound[e - 1](x)) or ACTIVATION (PALM: activated when its hazard -- the steps, then rate(x) per second from
// tOpen -- reaches its draw; then blinking in T). A field is a sum of terms scale x grid(x - ox - (z - zp) sx,
// y - oy - (z - zp) sy) x depth(z), bilinear on the grid (nodes outside it 0), linear in the depth table (its end
// values beyond); lerp form, so equal nodes give exactly that value.
//
// The flat buffer (isc_world_set_clock_map, core/include/insiliscope/insiliscope.h) is the exchange format:
// readClockMap / encodeClockMap.
//
// LICENSE: BSD-3-Clause (see LICENSE at the repository root)

export const CLOCK_MAP_FORMAT = 2;
export const CLOCK_GLOBAL = 0, CLOCK_DOSE = 1, CLOCK_ACTIVATION = 2;
export const CLOCK_TERM = 8;
const KIN_KEYS = ['activationRatePerSec', 'onSec', 'offSec', 'bleachProb', 'photonCV', 'initialOnSec'];

// ClockGrid::At: node (i, j) at (x0 + i h, y0 + j h) = v[j nx + i].
export function gridAt(g, u, w) {
  const fx = (u - g.x0) / g.h, fy = (w - g.y0) / g.h;
  const ix = Math.floor(fx), iy = Math.floor(fy);
  if (!(ix >= -1 && ix < g.nx && iy >= -1 && iy < g.ny)) return 0.0;
  const i = ix, j = iy;
  const tx = fx - ix, ty = fy - iy;
  const node = (a, b) => a < 0 || a >= g.nx || b < 0 || b >= g.ny ? 0.0 : g.v[b * g.nx + a];
  const v00 = node(i, j), v10 = node(i + 1, j), v01 = node(i, j + 1), v11 = node(i + 1, j + 1);
  const a = v00 + (v10 - v00) * tx;
  const b = v01 + (v11 - v01) * tx;
  return a + (b - a) * ty;
}

// ClockDepth::At: v[k] at z0 + k dz, linear between, the end values beyond; empty: 1.
export function depthAt(d, z) {
  const n = d.v.length;
  if (!n) return 1.0;
  const f = (z - d.z0) / d.dz;
  if (!(f > 0)) return d.v[0];
  if (!(f < n - 1)) return d.v[n - 1];
  const k = Math.floor(f);
  return d.v[k] + (d.v[k + 1] - d.v[k]) * (f - k);
}

export function termAt(m, t, x, y, z) {
  let g = 1.0;
  if (t.grid >= 0) {
    const dz = z - t.zp;
    g = gridAt(m.grids[t.grid], x - t.ox - dz * t.sx, y - t.oy - dz * t.sy);
  }
  const d = t.depth >= 0 ? depthAt(m.depths[t.depth], z) : 1.0;
  return t.scale * g * d;
}

// ClockMap::Eval: c, then the terms summed in order.
export function evalField(m, f, x, y, z, c = 0.0) {
  let s = c;
  for (const t of f) s += termAt(m, t, x, y, z);
  return s;
}

// ActivationInSteps from the start: {t, H} -- t the crossing within the steps (NaN: none), H the hazard summed over
// the steps walked.
export function activationInSteps(m, c, x, y, z, E) {
  let H = 0;
  for (const st of c.steps) {
    const inc = evalField(m, st.field, x, y, z, st.c);
    const Hn = H + inc;
    if (inc > 0 && E <= Hn) {
      const t = st.t0 + (st.t1 - st.t0) * ((E - H) / inc);
      return { t: Math.min(st.t1, Math.max(st.t0, t)), H };
    }
    H = Hn;
  }
  return { t: NaN, H };
}

// A dye's activation time under an activation clock (World::ExactAct): in the steps, else at the open rate.
export function activationTime(m, c, x, y, z, E) {
  const r = activationInSteps(m, c, x, y, z, E);
  if (!Number.isNaN(r.t)) return r.t;
  const S = evalField(m, c.rate, x, y, z, c.rateC);
  return S > 0 ? c.tOpen + (E - r.H) / S : Infinity;
}

// BleachEnd: when a budgeted window of draw aux bleaches under clock c at (x, y, z), in T (Infinity without bleach
// fields; at a rate <= 0 Infinity if aux > bleachTotal, else -Infinity).
export function bleachEnd(m, c, x, y, z, aux) {
  if (!c.bleachTotal.length && !c.bleachRate.length) return Infinity;
  const B = evalField(m, c.bleachTotal, x, y, z), r = evalField(m, c.bleachRate, x, y, z);
  if (r > 0) return c.tOpen + (aux - B) / r;
  return aux > B ? Infinity : -Infinity;
}
// world.cpp DoseHistory: a dose clock's kinetics history at (x, y, z) -- epoch e >= 1 from its bound there; an epoch
// the dye had no dose in gives way to the next.
export function doseHistory(m, c, x, y, z) {
  const out = { tStart: [0.0], kin: [c.hist.kin[0]] };
  for (let e = 1; e < c.hist.tStart.length; e++) {
    const B = evalField(m, c.bound[e - 1], x, y, z);
    if (B > out.tStart[out.tStart.length - 1]) {
      out.tStart.push(B);
      out.kin.push(c.hist.kin[e]);
    } else {
      out.kin[out.kin.length - 1] = c.hist.kin[e];
    }
  }
  return out;
}

// The checks of ClockMap::Finish (the same inputs refused; the C++ fingerprints have no twin). Throws.
export function validateClockMap(m, nStructures) {
  const fin = Number.isFinite;
  if (m.s.length !== nStructures) throw new Error('the clock map needs one clock per structure');
  for (const g of m.grids)
    if (!(g.nx >= 1 && g.ny >= 1 && fin(g.x0) && fin(g.y0) && fin(g.h) && g.h > 0) || g.v.length !== g.nx * g.ny ||
        !g.v.every(fin))
      throw new Error('a clock grid is malformed');
  for (const d of m.depths)
    if (!(fin(d.z0) && fin(d.dz) && d.dz > 0) || !d.v.every(fin)) throw new Error('a depth table is malformed');
  const field = f => {
    for (const t of f) {
      if (t.grid < -1 || t.grid >= m.grids.length || t.depth < -1 || t.depth >= m.depths.length)
        throw new Error('a clock term names a grid or depth table that is not there');
      if (![t.ox, t.oy, t.zp, t.sx, t.sy, t.scale].every(fin)) throw new Error('a clock term holds a value that is not finite');
    }
  };
  for (const c of m.s) {
    if (!(c.mode >= CLOCK_GLOBAL && c.mode <= CLOCK_ACTIVATION)) throw new Error('unknown clock mode');
    const h = c.hist;
    if (!h.tStart.length || h.kin.length !== h.tStart.length) throw new Error('a clock needs its kinetics');
    h.kin.forEach((q, i) => {
      if (!(fin(q.activationRatePerSec) && q.activationRatePerSec >= 0 && fin(q.onSec) && q.onSec > 0 && fin(q.offSec) &&
            q.offSec >= 0 && fin(q.bleachProb) && q.bleachProb > 0 && fin(q.photonCV) && q.photonCV >= 0 &&
            fin(q.initialOnSec) && q.initialOnSec >= 0))
        throw new Error('bad kinetics in a clock');
      const t = h.tStart[i];
      if (!fin(t) || (i === 0 ? t !== 0 : !(t > h.tStart[i - 1]))) throw new Error('epoch starts must be 0, then increasing');
    });
    if (!(fin(c.tOpen) && c.tOpen >= 0 && fin(c.totalC) && fin(c.rateC))) throw new Error('bad clock constants');
    if (c.mode === CLOCK_DOSE && h.tStart.length > 1 ? c.bound.length !== h.tStart.length - 1 : c.bound.length !== 0)
      throw new Error('a dose clock needs one bound per epoch after the first');
    c.bound.forEach(field);
    field(c.total);
    field(c.rate);
    field(c.bleachTotal);
    field(c.bleachRate);
    if (c.mode === CLOCK_ACTIVATION) {
      let t = 0;
      for (const st of c.steps) {
        if (!(fin(st.t0) && fin(st.t1) && fin(st.c) && st.t0 === t && st.t1 > st.t0))
          throw new Error('activation steps must run contiguously from 0');
        t = st.t1;
        field(st.field);
      }
      if (c.steps.length && t !== c.tOpen) throw new Error('activation steps must end at tOpen');
    } else if (c.steps.length) {
      throw new Error('only an activation clock has steps');
    }
  }
}

// The flat buffer -> {grids, depths, s} (validated). Throws on a bad buffer.
export function readClockMap(v, nStructures) {
  let i = 0;
  const D = () => {
    if (i >= v.length) throw new Error('clock map: short buffer');
    return v[i++];
  };
  const Int = (lo, hi) => {
    const d = D();
    if (!(d === Math.floor(d) && d >= lo && d <= hi)) throw new Error('clock map: bad count');
    return d;
  };
  const field = () => {
    const f = [];
    for (let k = Int(0, v.length - i); k > 0; k--) {
      const grid = Int(-1, 2147483647), depth = Int(-1, 2147483647);
      f.push({ grid, depth, ox: D(), oy: D(), zp: D(), sx: D(), sy: D(), scale: D() });
    }
    return f;
  };
  if (Int(CLOCK_MAP_FORMAT, CLOCK_MAP_FORMAT) !== CLOCK_MAP_FORMAT) throw new Error('clock map: format');
  const nGrids = Int(0, v.length), nDepths = Int(0, v.length);
  const m = { grids: [], depths: [], s: [] };
  for (let g = 0; g < nGrids; g++) {
    const x0 = D(), y0 = D(), h = D(), nx = Int(1, v.length), ny = Int(1, v.length);
    if (nx * ny > v.length - i) throw new Error('clock map: short grid');
    m.grids.push({ x0, y0, h, nx, ny, v: Array.from(v.slice(i, i + nx * ny)) });
    i += nx * ny;
  }
  for (let d = 0; d < nDepths; d++) {
    const z0 = D(), dz = D(), k = Int(0, v.length - i);
    m.depths.push({ z0, dz, v: Array.from(v.slice(i, i + k)) });
    i += k;
  }
  for (let s = 0; s < nStructures; s++) {
    const c = { mode: Int(CLOCK_GLOBAL, CLOCK_ACTIVATION), hist: { tStart: [], kin: [] }, bound: [], total: [], totalC: 0,
      rate: [], rateC: 0, tOpen: 0, steps: [], bleachTotal: [], bleachRate: [] };
    for (let e = Int(1, v.length - i); e > 0; e--) {
      c.hist.tStart.push(D());
      const q = {};
      for (const key of KIN_KEYS) q[key] = D();
      c.hist.kin.push(q);
    }
    for (let b = Int(0, v.length - i); b > 0; b--) c.bound.push(field());
    c.total = field();
    c.totalC = D();
    c.rate = field();
    c.rateC = D();
    c.tOpen = D();
    for (let k = Int(0, v.length - i); k > 0; k--) {
      const t0 = D(), t1 = D(), cc = D();
      c.steps.push({ t0, t1, c: cc, field: field() });
    }
    c.bleachTotal = field();
    c.bleachRate = field();
    m.s.push(c);
  }
  if (i !== v.length) throw new Error('clock map: trailing values');
  validateClockMap(m, nStructures);
  return m;
}

// {grids, depths, s} -> the flat buffer (missing fields: their defaults).
export function encodeClockMap(m) {
  const v = [CLOCK_MAP_FORMAT, m.grids.length, m.depths.length];
  for (const g of m.grids) v.push(g.x0, g.y0, g.h, g.nx, g.ny, ...g.v);
  for (const d of m.depths) v.push(d.z0, d.dz, d.v.length, ...d.v);
  const field = (f = []) => {
    v.push(f.length);
    for (const t of f) v.push(t.grid ?? -1, t.depth ?? -1, t.ox ?? 0, t.oy ?? 0, t.zp ?? 0, t.sx ?? 0, t.sy ?? 0, t.scale ?? 1);
  };
  for (const c of m.s) {
    v.push(c.mode, c.hist.tStart.length);
    c.hist.tStart.forEach((t, e) => v.push(t, ...KIN_KEYS.map(k => c.hist.kin[e][k] ?? 0)));
    const bound = c.bound ?? [];
    v.push(bound.length);
    bound.forEach(field);
    field(c.total);
    v.push(c.totalC ?? 0);
    field(c.rate);
    v.push(c.rateC ?? 0, c.tOpen ?? 0);
    const steps = c.steps ?? [];
    v.push(steps.length);
    for (const st of steps) {
      v.push(st.t0, st.t1, st.c ?? 0);
      field(st.field);
    }
    field(c.bleachTotal);
    field(c.bleachRate);
  }
  return v;
}
