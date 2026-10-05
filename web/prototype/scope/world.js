// The queryable world of the imaging path: a mirror of core/src/world.cpp (fixed-block packing, cell
// assets, 1 um dye blocks, blink events, dye density), built on a loadPrototype() instance for the
// geometry (cells, packing, microtubules are the prototype's own functions). Same answers and the same
// event ORDER as the C++ (the renderer sums in that order). Caches are for speed only.
import { hashUnit } from './rng.js';
import { buildMtFrames, pointAtArc, dyesInBlock, labelSchedule, persistentGen, dyeH1, dyeOrientation, mtSegmentAt,
  mtProtofilamentOffsetNm, makeLabel, validateLabel, DYE_BLOCK_UM, MT_DIMER_NM, MT_RADIUS_NM, MT_BINDER_NM,
  MT_LINKER_MAX_NM, PERSIST_BIN_SEC, PERSIST_ON_CAP } from './dyes.js';

export const PACK_BLOCK_CHUNKS = 8;
// The structures that carry labels (issue 16): one label each, indexed by their position here. Only the
// microtubules for now; a new structure (nucleus DNA, NPCs, ...) adds an entry and its site generator.
export const STRUCTURES = [{ id: 'microtubules', prefix: 'mt', name: 'Microtubules' }];
export const STRUCTURE_MT = 0;
// eventsInWindow kinds: the blinks (dSTORM/PALM/DNA-PAINT), the continuous windows (EVENT_STATE PRE, INITIAL_ON,
// ALWAYS_ON; continuousInWindow).
export const EVENTS_BLINKS = 1;
// = ISC_WORLD_VERSION (core/include/insiliscope/insiliscope.h): the generator's version, the
// date spec/golden was last re-frozen. Bump both when a cell moves (engine_check compares them).
export const WORLD_VERSION = '2026-10-05';
const DYE_REACH_UM = (MT_RADIUS_NM + MT_BINDER_NM + MT_LINKER_MAX_NM) * 1e-3;
const floorDiv = (a, b) => Math.floor(a / b);

function rectDist(px, py, x0, y0, x1, y1) {
  const dx = Math.max(Math.max(x0 - px, 0.0), px - x1);
  const dy = Math.max(Math.max(y0 - py, 0.0), py - y1);
  return Math.hypot(dx, dy);
}

export function localToWorld(c, lx, ly) {
  const rot = c.packRot;
  if (rot === 0 || Number.isNaN(rot) || rot == null) return [c.x + lx, c.y + ly];
  const cr = Math.cos(rot), sr = Math.sin(rot);
  return [c.x + lx * cr - ly * sr, c.y + lx * sr + ly * cr];
}

export class World {
  // P: loadPrototype() instance. params: prototype params (P.paramsFrom(...)), geometry only. labels: one label
  // (dyes.js makeLabel) per STRUCTURES entry.
  constructor(P, seed, params, labels = [makeLabel()]) {
    this.g = P.gen;
    this.seed = seed >>> 0;
    this.p = params;
    this.blocks = new Map();   // packing blocks
    this.assets = new Map();   // cell assets by chunk
    this.dyeBlocks = new Map();
    this.labels = [];
    this.setLabels(labels);
  }

  // A change of density or fluorescent fraction redraws the dyes (the cells and microtubules stay); any other label
  // change only re-schedules them (isc_world_set_label).
  setLabels(labels) {
    if (labels.length !== STRUCTURES.length) throw new Error(`setLabels: ${STRUCTURES.length} labels expected`);
    const next = labels.map(l => { const c = structuredClone(l); validateLabel(c); return c; });
    const dyesChange = next.some((l, i) => !this.labels[i] || l.density !== this.labels[i].density ||
      l.fluorescentFraction !== this.labels[i].fluorescentFraction);
    this.labels = next;
    if (dyesChange) this.dyeBlocks.clear();
    else for (const b of this.dyeBlocks.values()) { b.scheduled = false; b.events = null; b.continuous = null; }
  }

  cellReachUm() {
    const p = this.p;
    const worstSemiMajor = (p.cellDiamMax / 2) / Math.sqrt(Math.max(0.05, p.cellElongMin));
    return 2 * worstSemiMajor * this.g.CELL_MOD_MAX * (p.cellRough > 0 && p.cellBlob > 0 ? 1 + this.g.TAIL_MAX : 1) + p.chunkSize;
  }

  packBlock(bx, by) {
    const g = this.g, p = this.p;
    const cx0 = bx * PACK_BLOCK_CHUNKS, cy0 = by * PACK_BLOCK_CHUNKS;
    const cx1 = cx0 + PACK_BLOCK_CHUNKS - 1, cy1 = cy0 + PACK_BLOCK_CHUNKS - 1;
    const cells = [];
    if (p.enablePacking) {
      const M = g.interactionChunks(p) + 2;
      const map = g.buildCandidateMap(this.seed, cx0 - M, cy0 - M, cx1 + M, cy1 + M, p);
      g.packMap(map, p);
      for (const c of map.values()) if (c.cx >= cx0 && c.cx <= cx1 && c.cy >= cy0 && c.cy <= cy1) cells.push(c);
    } else {
      for (let cx = cx0; cx <= cx1; cx++)
        for (let cy = cy0; cy <= cy1; cy++) {
          const c = g.rawCandidate(this.seed, cx, cy, p);
          if (c.present) cells.push(c);
        }
    }
    return cells;
  }

  packedBlock(bx, by) {
    const key = bx + ',' + by;
    let b = this.blocks.get(key);
    if (!b) { b = this.packBlock(bx, by); this.blocks.set(key, b); }
    return b;
  }

  // isc_world_set_block (ABI 7): block (bx, by) as another world of the same seed and params packed it
  // (rows of 14 per cell as cellsInRect's: only cx, cy, x, y, packRot are read -- relax moves those and
  // prune drops cells; every other field is rawCandidate's, recomputed here). Skipped if cached.
  setPackedBlock(bx, by, rows) {
    const key = bx + ',' + by;
    if (this.blocks.has(key)) return false;
    const cells = [];
    for (let i = 0; i < rows.length; i += 14) {
      const c = this.g.rawCandidate(this.seed, rows[i], rows[i + 1], this.p);
      if (!c.present) throw new Error('setPackedBlock: not a cell of this world');
      c.x = rows[i + 2]; c.y = rows[i + 3]; c.packRot = rows[i + 4];
      cells.push(c);
    }
    this.blocks.set(key, cells);
    return true;
  }

  // Cells whose footprint circle (rOuter) meets the rect, in the C++ order.
  cellsInRect(x0, y0, x1, y1) {
    const S = this.p.chunkSize, reach = this.cellReachUm();
    const cxLo = Math.floor((x0 - reach) / S), cxHi = Math.floor((x1 + reach) / S);
    const cyLo = Math.floor((y0 - reach) / S), cyHi = Math.floor((y1 + reach) / S);
    const bx0 = floorDiv(cxLo, PACK_BLOCK_CHUNKS), bx1 = floorDiv(cxHi, PACK_BLOCK_CHUNKS);
    const by0 = floorDiv(cyLo, PACK_BLOCK_CHUNKS), by1 = floorDiv(cyHi, PACK_BLOCK_CHUNKS);
    const out = [];
    for (let bx = bx0; bx <= bx1; bx++)
      for (let by = by0; by <= by1; by++)
        for (const c of this.packedBlock(bx, by))
          if (rectDist(c.x, c.y, x0, y0, x1, y1) <= c.rOuter) out.push(c);
    return out;
  }

  cellAssets(c) {
    const key = c.cx + ',' + c.cy;
    let a = this.assets.get(key);
    if (a) return a;
    this.g.ensureCytoCacheFresh(this.seed, this.p);
    const mts = this.g.buildMicrotubulesForCell(this.seed, c.cx, c.cy, c, this.p);
    a = { cell: c, mts, frames: new Array(mts.length), mtReach: mts.map(pts => pts.reduce((r, q) => Math.max(r, Math.hypot(q.x, q.y)), 0)) };
    this.assets.set(key, a);
    return a;
  }

  frames(a, i) { return a.frames[i] || (a.frames[i] = buildMtFrames(a.mts[i])); }

  // fn(block) for every 1 um dye block that can reach the rect/z range, in the C++ order.
  forEachDyeBlock(x0, y0, x1, y1, zMin, zMax, fn) {
    const blockReach = DYE_BLOCK_UM / 2 + DYE_REACH_UM;
    for (const c of this.cellsInRect(x0, y0, x1, y1)) {
      const A = this.cellAssets(c);
      const centreDist = rectDist(c.x, c.y, x0, y0, x1, y1);
      for (let i = 0; i < A.mts.length; i++) {
        const pts = A.mts[i];
        if (pts.length < 2 || centreDist > A.mtReach[i] + DYE_REACH_UM) continue;
        const fr = this.frames(A, i);
        const len = fr.length;
        const nBlocks = Math.ceil(len / DYE_BLOCK_UM);
        for (let b = 0; b < nBlocks; b++) {
          const mid = pointAtArc(pts, fr, Math.min((b + 0.5) * DYE_BLOCK_UM, len));
          if (mid.z + blockReach < zMin || mid.z - blockReach >= zMax) continue;
          const [wx, wy] = localToWorld(c, mid.x, mid.y);
          if (rectDist(wx, wy, x0, y0, x1, y1) > blockReach) continue;
          fn(this.dyeBlock(c, A, i, b));
        }
      }
    }
  }

  dyeBlock(c, A, mtIndex, block) {
    const key = c.cx + ',' + c.cy + ',' + mtIndex + ',' + block;
    let blk = this.dyeBlocks.get(key);
    if (blk) return blk;
    const raw = [], label = this.labels[STRUCTURE_MT];
    dyesInBlock(this.seed, c.cx, c.cy, mtIndex, A.mts[mtIndex], this.frames(A, mtIndex), block,
      label.density, label.fluorescentFraction, raw);
    blk = { dyes: [], zLo: Infinity, zHi: -Infinity, scheduled: false, events: null, continuous: null, maxOn: 0,
      persistent: [], structure: STRUCTURE_MT };
    for (const d of raw) {
      const [wx, wy] = localToWorld(c, d.pos.x, d.pos.y);
      blk.dyes.push({ x: wx, y: wy, z: d.pos.z, id: d.id, cx: c.cx, cy: c.cy, mtIndex: d.mtIndex, k: d.k, n: d.n,
        theta: d.theta, structure: STRUCTURE_MT });
      blk.zLo = Math.min(blk.zLo, d.pos.z);
      blk.zHi = Math.max(blk.zHi, d.pos.z);
    }
    this.dyeBlocks.set(key, blk);
    return blk;
  }

  // The block's blinks (sorted by tOn, ties in dye order) and continuous windows (dye order), or its persistent
  // (DNA-PAINT) dyes, by its structure's label mode.
  schedule(b) {
    if (b.scheduled) return;
    b.events = []; b.continuous = []; b.persistent = []; b.maxOn = 0;
    const label = this.labels[b.structure], s = b.structure;
    let h1 = 0, lastCx = 0, lastCy = 0, lastMt = -1;
    const blinks = [], cont = [];
    for (let i = 0; i < b.dyes.length; i++) {
      const d = b.dyes[i];
      if (label.mode === 'DNA-PAINT') { b.persistent.push(i); continue; }
      if (d.mtIndex !== lastMt || d.cx !== lastCx || d.cy !== lastCy) {
        h1 = dyeH1(this.seed, d.cx, d.cy, d.mtIndex); lastCx = d.cx; lastCy = d.cy; lastMt = d.mtIndex;
      }
      blinks.length = 0; cont.length = 0;
      labelSchedule(h1, d.k, d.n, label, blinks, cont);
      for (const bl of blinks) {
        b.events.push({ x: d.x, y: d.y, z: d.z, tOn: bl.tOn, tOff: bl.tOff, brightness: bl.brightness, id: d.id,
          structure: s, state: 0, aux: 0 });
        b.maxOn = Math.max(b.maxOn, bl.tOff - bl.tOn);
      }
      for (const w of cont)
        b.continuous.push({ x: d.x, y: d.y, z: d.z, tOn: w.tOn, tOff: w.tOff, brightness: 1, id: d.id, structure: s,
          state: w.state, aux: w.aux });
    }
    b.events.sort((a, e) => a.tOn - e.tOn); // stable: ties keep dye order
    b.scheduled = true;
  }

  // Labelled dyes in [x0,x1) x [y0,y1) x [zMin,zMax), world um.
  sitesInWindow(x0, y0, x1, y1, zMin, zMax) {
    const out = [];
    this.forEachDyeBlock(x0, y0, x1, y1, zMin, zMax, b => {
      if (!b.dyes.length || b.zHi < zMin || b.zLo >= zMax) return;
      for (const d of b.dyes) if (d.z >= zMin && d.z < zMax && d.x >= x0 && d.x < x1 && d.y >= y0 && d.y < y1) out.push(d);
    });
    return out;
  }

  // Blinks (tOn < t1, tOff > t0) of the dyes in the window: [{x, y, z, tOn, tOff, brightness, id, structure, state,
  // aux}] (state 0 = EVENT_STATE.BLINK, aux 0). The continuous windows: continuousInWindow.
  eventsInWindow(x0, y0, x1, y1, zMin, zMax, t0, t1) {
    const out = [];
    const inWin = d => d.z >= zMin && d.z < zMax && d.x >= x0 && d.x < x1 && d.y >= y0 && d.y < y1;
    this.forEachDyeBlock(x0, y0, x1, y1, zMin, zMax, b => {
      if (!b.dyes.length || b.zHi < zMin || b.zLo >= zMax) return;
      this.schedule(b);
      const kin = this.labels[b.structure].kinetics;
      const persist = kin.activationRatePerSec > 0 && t1 > t0;
      const maxOn = PERSIST_ON_CAP * kin.onSec;
      const b0 = Math.max(0, Math.floor((t0 - maxOn) / PERSIST_BIN_SEC));
      const b1 = Math.floor(t1 / PERSIST_BIN_SEC);
      const ev = b.events, tLo = t0 - b.maxOn;
      let lo = 0, hi = ev.length;
      while (lo < hi) { const m = (lo + hi) >>> 1; if (ev[m].tOn < tLo) lo = m + 1; else hi = m; }
      for (let i = lo; i < ev.length && ev[i].tOn < t1; i++) {
        const e = ev[i];
        if (e.tOff > t0 && inWin(e)) out.push(e);
      }
      if (!b.persistent.length || !persist) return;
      let h1 = 0, lastCx = 0, lastCy = 0, lastMt = -1;
      for (const di of b.persistent) {
        const d = b.dyes[di];
        if (!inWin(d)) continue;
        if (d.mtIndex !== lastMt || d.cx !== lastCx || d.cy !== lastCy) {
          h1 = dyeH1(this.seed, d.cx, d.cy, d.mtIndex); lastCx = d.cx; lastCy = d.cy; lastMt = d.mtIndex;
        }
        persistentGen(h1, d.k, d.n, kin, b0, b1, (tOn, on) => tOn < t1 && tOn + on > t0,
          (bin, j, tOn, on, br) => out.push({ x: d.x, y: d.y, z: d.z, tOn, tOff: tOn + on, brightness: br, id: d.id,
            structure: b.structure, state: 0, aux: 0 }));
      }
    });
    return out;
  }

  // The continuous windows of the dyes in the window (every time; dye order per block): [{x, y, z, tOn, tOff,
  // brightness 1, id, structure, state (EVENT_STATE PRE / INITIAL_ON / ALWAYS_ON), aux}].
  continuousInWindow(x0, y0, x1, y1, zMin, zMax) {
    const out = [];
    this.forEachDyeBlock(x0, y0, x1, y1, zMin, zMax, b => {
      if (!b.dyes.length || b.zHi < zMin || b.zLo >= zMax) return;
      this.schedule(b);
      for (const e of b.continuous) if (e.z >= zMin && e.z < zMax && e.x >= x0 && e.x < x1 && e.y >= y0 && e.y < y1) out.push(e);
    });
    return out;
  }

  // Mean emission dipole of a dye from sitesInWindow (dyes.js dyeOrientation; null for Free), cell-local.
  dyeOrientation(d) {
    const label = this.labels[d.structure];
    if (label.orientation.mode === 'Free') return null;
    const A = this.cellAssets(this.packedBlock(floorDiv(d.cx, PACK_BLOCK_CHUNKS), floorDiv(d.cy, PACK_BLOCK_CHUNKS))
      .find(c => c.cx === d.cx && c.cy === d.cy));
    const fr = this.frames(A, d.mtIndex), S = (mtProtofilamentOffsetNm(d.k) + d.n * MT_DIMER_NM) * 1e-3;
    return dyeOrientation(dyeH1(this.seed, d.cx, d.cy, d.mtIndex), d.k, d.n, label, fr, mtSegmentAt(fr, S), d.theta);
  }

  // Fluorescent-dye counts on an nx x ny x nz grid, out[(k*ny + iy)*nx + ix]; structureMask: bit s = STRUCTURES[s]
  // (isc_density3d_in_window, ABI 10). Returns {total, grid}.
  density3d(x0, y0, x1, y1, zMin, zMax, nx, ny, nz, structureMask, out = new Float32Array(nx * ny * nz)) {
    out.fill(0);
    if (!structureMask) return { total: 0, grid: out };
    const sx = nx / (x1 - x0), sy = ny / (y1 - y0), sz = nz > 1 ? nz / (zMax - zMin) : 0.0;
    let total = 0;
    this.forEachDyeBlock(x0, y0, x1, y1, zMin, zMax, b => {
      if (!b.dyes.length || b.zHi < zMin || b.zLo >= zMax || !((structureMask >> b.structure) & 1)) return;
      for (const d of b.dyes) {
        if (!(d.z >= zMin && d.z < zMax && d.x >= x0 && d.x < x1 && d.y >= y0 && d.y < y1)) continue;
        const ix = Math.min(nx - 1, Math.floor((d.x - x0) * sx));
        const iy = Math.min(ny - 1, Math.floor((d.y - y0) * sy));
        const iz = nz > 1 ? Math.min(nz - 1, Math.floor((d.z - zMin) * sz)) : 0;
        out[(iz * ny + iy) * nx + ix] += 1;
        total++;
      }
    });
    return { total, grid: out };
  }

  // Tallest cell (c.height) whose footprint meets the rect; 0 if none (CellFieldSource::MaxCellHeight).
  maxCellHeight(x0, y0, x1, y1) {
    let h = 0;
    for (const c of this.cellsInRect(x0, y0, x1, y1)) h = Math.max(h, c.height);
    return h;
  }

  // Optical volume (World::OpticalVolumeInWindow, ABI 6, BrightField): per voxel of an nx x ny x nz grid over
  // the rect and [zMin, zMax) (finite), the volume fractions of cytoplasm (body minus nucleus), nucleus and
  // microtubule (12.5 nm tubes), channel-major: out[((ch*nz + k)*ny + iy)*nx + ix]. Each column is sampled at
  // sub x sub points per voxel footprint; z overlaps are exact. Returns the number of cells reaching the rect.
  opticalVolume(x0, y0, x1, y1, zMin, zMax, nx, ny, nz, sub, out = new Float32Array(3 * nx * ny * nz)) {
    const g = this.g, p = this.p;
    const plane = nx * ny, chan = plane * nz;
    out.fill(0, 0, 3 * chan);
    const cells = this.cellsInRect(x0, y0, x1, y1);
    const px = (x1 - x0) / nx, py = (y1 - y0) / ny, dz = (zMax - zMin) / nz;
    const subW = 1.0 / (sub * sub);
    const mtArea = Math.PI * (MT_RADIUS_NM / 1000) * (MT_RADIUS_NM / 1000);
    const mtStep = 0.5 * Math.min(px, Math.min(py, dz));
    const overlap = (a0, a1, b0, b1) => Math.max(0.0, Math.min(a1, b1) - Math.max(a0, b0));
    for (const c of cells) {
      const A = this.cellAssets(c);
      const rotated = !(c.packRot === 0 || Number.isNaN(c.packRot) || c.packRot == null);
      const cr = rotated ? Math.cos(c.packRot) : 1, sr = rotated ? Math.sin(c.packRot) : 0;
      // CellInnerRadiusBound (speed only): closer to the centre is inside without evaluating the outline.
      const rIn = (c.semiMinor < c.semiMajor ? c.semiMinor : c.semiMajor) * c.modFloor * (1 - c.tailBound) * (1 - 1e-12);
      const ix0 = Math.max(0, Math.floor((c.x - c.rOuter - x0) / px));
      const ix1 = Math.min(nx - 1, Math.floor((c.x + c.rOuter - x0) / px));
      const iy0 = Math.max(0, Math.floor((c.y - c.rOuter - y0) / py));
      const iy1 = Math.min(ny - 1, Math.floor((c.y + c.rOuter - y0) / py));
      for (let iy = iy0; iy <= iy1; iy++)
        for (let ix = ix0; ix <= ix1; ix++)
          for (let sv = 0; sv < sub; sv++)
            for (let su = 0; su < sub; su++) {
              const dx = x0 + (ix + (su + 0.5) / sub) * px - c.x;
              const dy = y0 + (iy + (sv + 0.5) / sub) * py - c.y;
              const lx = dx * cr + dy * sr, ly = -dx * sr + dy * cr;
              const rr = Math.hypot(lx, ly);
              if (rr > c.rOuter || (rr > rIn && rr > g.cellRadiusAt(c, Math.atan2(ly, lx)))) continue;
              const h = g.sampleCytoMeshHeight(c, p, lx, ly);
              if (!(h > 0)) continue;
              // Nucleus chord through this column (g.nucleusColumnLocal: the shaped nucleus), clipped to the body.
              const col = g.nucleusColumnLocal(c, lx, ly);   // [below, above] nucZ, or null
              let zn0 = 0, zn1 = 0;
              if (col && (col[0] > 0 || col[1] > 0)) {
                zn0 = Math.max(0.0, c.nucZ - col[0]);
                zn1 = Math.min(h, c.nucZ + col[1]);
                if (zn1 < zn0) zn0 = zn1 = 0;
              }
              const k0 = Math.max(0, Math.floor((0 - zMin) / dz));
              const k1 = Math.min(nz - 1, Math.floor((h - zMin) / dz));
              for (let k = k0; k <= k1; k++) {
                const zl = zMin + k * dz, zh = zl + dz;
                const nuc = overlap(zl, zh, zn0, zn1);
                const body = overlap(zl, zh, 0, h);
                const v = (k * ny + iy) * nx + ix;
                out[v] += Math.fround((body - nuc) / dz * subW);
                out[chan + v] += Math.fround(nuc / dz * subW);
              }
            }
      // Microtubules: their tube volume, deposited along the centrelines.
      const voxVol = px * py * dz;
      for (const pts of A.mts) {
        for (let i = 0; i + 1 < pts.length; i++) {
          const a = pts[i], b = pts[i + 1];
          const len = Math.hypot(b.x - a.x, b.y - a.y, b.z - a.z);
          if (!(len > 0)) continue;
          const n = Math.max(1, Math.ceil(len / mtStep));
          for (let j = 0; j < n; j++) {
            const t = (j + 0.5) / n;
            const lx = a.x + (b.x - a.x) * t, ly = a.y + (b.y - a.y) * t, z = a.z + (b.z - a.z) * t;
            const wx = c.x + lx * cr - ly * sr, wy = c.y + lx * sr + ly * cr;
            if (!(wx >= x0 && wx < x1 && wy >= y0 && wy < y1 && z >= zMin && z < zMax)) continue;
            const ix = Math.min(nx - 1, Math.floor((wx - x0) / px));
            const iy = Math.min(ny - 1, Math.floor((wy - y0) / py));
            const k = Math.min(nz - 1, Math.floor((z - zMin) / dz));
            out[2 * chan + (k * ny + iy) * nx + ix] += Math.fround(len / n * mtArea / voxVol);
          }
        }
      }
    }
    return cells.length;
  }
}

export { hashUnit };
