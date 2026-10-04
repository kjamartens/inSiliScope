// Lab generation on top of a loadPrototype() instance: packed cells around a chunk, their microtubules
// in world coordinates, dye sites, metrics. Used by lab_worker.js (browser) and check.mjs (Node).
import { summarize } from './metrics.js';

export function makeField(P) {
  const g = P.gen;
  let packCache = null;

  function params(overrides) {
    return P.paramsFrom({ ...P.defaults, ...overrides });
  }

  // Packed cells for chunks [cx0..cx1] x [cy0..cy1], packing over a padded window (like the viewer).
  function packed(seed, p, cx0, cy0, cx1, cy1) {
    const m = g.interactionChunks(p) + 2;
    const key = JSON.stringify([seed, p, cx0, cy0, cx1, cy1]);
    if (packCache && packCache.key === key) return packCache;
    const map = g.buildCandidateMap(seed, cx0 - m, cy0 - m, cx1 + m, cy1 + m, p);
    const removed = p.enablePacking ? g.packMap(map, p) : 0;
    packCache = { key, map, removed };
    return packCache;
  }

  const toWorld = (c, lx, ly) => {
    const r = c.packRot || 0;
    if (!r) return [c.x + lx, c.y + ly];
    const cr = Math.cos(r), sr = Math.sin(r);
    return [c.x + lx * cr - ly * sr, c.y + lx * sr + ly * cr];
  };

  // Cells (with world-space MTs) whose chunk lies in the window.
  function cells(seed, overrides, cx0, cy0, cx1, cy1, { withMts = true } = {}) {
    const p = params(overrides);
    g.ensureCytoCacheFresh(seed, p);
    const t0 = performance.now();
    const pk = packed(seed, p, cx0, cy0, cx1, cy1);
    const tPack = performance.now() - t0;
    const out = [];
    for (const c of pk.map.values()) {
      if (c.cx < cx0 || c.cx > cx1 || c.cy < cy0 || c.cy > cy1) continue;
      const outline = g.cellOutlineLocal(c, 96).map(([lx, ly]) => toWorld(c, lx, ly));
      const [nx, ny] = toWorld(c, c.nucOffX, c.nucOffY);
      const cell = {
        cx: c.cx, cy: c.cy, x: c.x, y: c.y, packRot: c.packRot || 0, height: c.height, outline,
        nuc: { x: nx, y: ny, a: c.nucLong / 2, b: c.nucShort / 2, rot: c.nucRot + (c.packRot || 0), z: c.nucZ, h: c.nucHeight,
          // footprint outline (world), shaped nucleus only
          poly: c.nucShaped ? c.nucPoly.filter((_, i) => i % 4 === 0).map(([lx, ly]) => toWorld(c, lx, ly)) : null },
      };
      if (withMts) {
        const local = g.buildMicrotubulesForCell(seed, c.cx, c.cy, c, p);
        cell.mtsLocal = local;
        cell.mts = local.map(pts => pts.map(q => { const [x, y] = toWorld(c, q.x, q.y); return { x, y, z: q.z }; }));
      }
      out.push(cell);
    }
    return { p, cells: out, removed: pk.removed, msPack: tPack, msTotal: performance.now() - t0 };
  }

  // Next chunk (row-major walk, direction dir = +1/-1, starting AT (cx, cy)) that holds a cell candidate.
  function findCell(seed, overrides, cx, cy, dir = 1) {
    const p = params(overrides);
    for (let i = 0; i < 400; i++) {
      if (g.rawCandidate(seed, cx, cy, p).present) return [cx, cy];
      cx += dir;
      if (cx > 8) { cx = -8; cy++; } else if (cx < -8) { cx = 8; cy--; }
    }
    return [cx, cy];
  }

  function metrics(cellList) {
    return summarize(cellList.flatMap(c => c.mtsLocal || []));
  }

  return { params, cells, metrics, findCell, gen: g };
}
