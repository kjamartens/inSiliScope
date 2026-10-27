// Statistics of the nuclear lamina points (2026-10-27) on the working-tree prototype, Node, seconds:
//   node web/lab/lamin_check.mjs [--seed N] [--cells 4] [--set laminDensity=50 ...]
// Fails on: a point count != round(density x mesh area), a point within laminNpcExclNm / 2 of a pore centre, a point
// off the membrane (+-1 nm along its normal must be outside / inside the nucleus), cap vs side counts off their area
// fractions (pooled, 4 sigma), points that depend on query history, pores that depend on the lamina's params, and, in
// a JS World, lamina dyes that are not structure STRUCTURE_LAMIN, lie farther from a point than the linkage reaches,
// change the other targets' dyes, or appear with the lamina's label off. Part of web/lab/check.mjs.
import fs from 'fs';
import path from 'path';
import { fileURLToPath } from 'url';
import { loadPrototype } from '../../tests/parity/load_prototype.mjs';
import { makeField } from './field.js';
import { World, STRUCTURES, STRUCTURE_LAMIN, localToWorld } from '../prototype/scope/world.js';
import { makeLabel, linkageReachNm } from '../prototype/scope/dyes.js';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');

// Returns the number of failures; prints one line per finding and a summary.
export function laminCheck({ seed: seedArg = null, nCells = 4, overrides = {}, log = console.log } = {}) {
  const read = f => fs.readFileSync(path.join(ROOT, f), 'utf8');
  const html = read('web/prototype/index.html'), mt = read('web/prototype/microtubules.js');
  const P = loadPrototype(html, mt), F = makeField(P), g = P.gen;
  const seed = seedArg ?? P.defaults.seed;
  // The world model's default is no lamina (laminDensity 0); the engine's lamin-density is 50, and the exclusion is
  // widened here so the rejection shows.
  const set = { laminDensity: 50, laminNpcExclNm: 120, ...overrides };
  const p = P.paramsFrom({ ...P.defaults, ...set });
  let fail = 0;
  const bad = m => { log('FAIL lamin: ' + m); fail++; };
  const t0 = performance.now();
  const addrs = [];
  for (let cx = 0, cy = 0; addrs.length < nCells;) { [cx, cy] = F.findCell(seed, set, cx, cy, 1); addrs.push([cx, cy]); cx++; }
  const rEx = p.laminNpcExclNm / 2 * 1e-3;
  let points = 0, area = 0, capArea = 0, capPoints = 0, msBuild = 0, nearest = Infinity;
  const sigs = [];
  for (const [cx, cy] of addrs) {
    const c = g.rawCandidate(seed, cx, cy, p);   // cell-local: packing moves and turns the cell only
    const pores = g.buildNpcsForCell(seed, cx, cy, c, p);
    const tb = performance.now();
    const list = g.buildLaminForCell(seed, cx, cy, c, p, pores);
    msBuild += performance.now() - tb;
    const M = g.nucSurfaceMesh(c);
    const want = Math.floor(p.laminDensity * M.area + 0.5);
    if (list.length !== want) bad(`cell ${cx},${cy}: ${list.length} points, want ${want}`);
    points += list.length; area += M.area;
    let ca = 0;
    for (let t = 0; t < M.tris.length; t++) if (M.tris[t][0] !== 0) ca += M.cdf[t] - (t ? M.cdf[t - 1] : 0);
    capArea += ca;
    sigs.push(JSON.stringify(list.map(q => [q.x, q.y, q.z, q.cand])));
    const inside = (x, y, z) => { const col = g.nucleusColumnLocal(c, x, y); return !!col && z > c.nucZ - col[0] && z < c.nucZ + col[1]; };
    const e = 1e-3;
    for (let i = 0; i < list.length; i++) {
      const q = list[i];
      if (Math.abs(Math.hypot(...q.n) - 1) > 1e-9) { bad(`cell ${cx},${cy} point ${i}: normal not unit`); break; }
      if (inside(q.x + e * q.n[0], q.y + e * q.n[1], q.z + e * q.n[2]) || !inside(q.x - e * q.n[0], q.y - e * q.n[1], q.z - e * q.n[2]))
        { bad(`cell ${cx},${cy} point ${i}: not on the membrane with an outward normal`); break; }
      if (i && !(q.cand > list[i - 1].cand)) { bad(`cell ${cx},${cy}: not in candidate order`); break; }
      if (q.piece !== 0) capPoints++;
    }
    // No point within rEx of a pore centre (every pair: the grid in buildLaminForCell is for speed only).
    for (const o of pores)
      for (const q of list) {
        const d = Math.hypot(q.x - o.x, q.y - o.y, q.z - o.z);
        nearest = Math.min(nearest, d);
        if (d < rEx) { bad(`cell ${cx},${cy}: a point ${(d * 1000).toFixed(1)} nm from a pore centre`); break; }
      }
    // The pores do not depend on the lamina's params.
    const p0 = P.paramsFrom({ ...P.defaults, ...overrides, laminDensity: 0 });
    if (JSON.stringify(g.buildNpcsForCell(seed, cx, cy, c, p0).map(q => [q.x, q.y, q.z, q.cand])) !==
        JSON.stringify(pores.map(q => [q.x, q.y, q.z, q.cand]))) bad(`cell ${cx},${cy}: the pores depend on the lamina's params`);
  }
  // History: a fresh instance, a far cell first, the cells in reverse.
  const P2 = loadPrototype(html, mt);
  { const c = P2.gen.rawCandidate(seed, 40, -37, p); P2.gen.buildLaminForCell(seed, 40, -37, c, p, P2.gen.buildNpcsForCell(seed, 40, -37, c, p)); }
  [...addrs].reverse().forEach(([cx, cy]) => {
    const c = P2.gen.rawCandidate(seed, cx, cy, p);
    const s = JSON.stringify(P2.gen.buildLaminForCell(seed, cx, cy, c, p, P2.gen.buildNpcsForCell(seed, cx, cy, c, p)).map(q => [q.x, q.y, q.z, q.cand]));
    if (s !== sigs[addrs.findIndex(a => a[0] === cx && a[1] === cy)]) bad(`cell ${cx},${cy}: points depend on query history`);
  });
  const fCap = capArea / area, sigma = Math.sqrt(points * fCap * (1 - fCap));
  if (points > 0 && fCap > 0 && Math.abs(capPoints - points * fCap) > 4 * sigma + 1)
    bad(`cap points ${capPoints} vs ${(points * fCap).toFixed(1)} expected (+-${sigma.toFixed(1)})`);

  // The World: the lamina's dyes over the first cell's nucleus, with the pores and microtubules labelled too.
  const tw = performance.now();
  const lk = { binderNm: 19.3, binderFlexNm: 15.3, linkerMinNm: 0.5, linkerMaxNm: 0.5 };   // IgG2-PAINT
  // The others thin (1 %: their dyes only need to be there and unchanged).
  const labels = on => STRUCTURES.map((s, i) => makeLabel(i === STRUCTURE_LAMIN ? { density: on ? 0.7 : 0, linkage: lk } : { density: 0.01 }));
  const W = new World(P, seed, p, labels(true)), W0 = new World(P, seed, p, labels(false));
  const [cx0, cy0] = addrs[0];
  const c0 = W.packedBlock(Math.floor(cx0 / 8), Math.floor(cy0 / 8)).find(q => q.cx === cx0 && q.cy === cy0);
  if (!c0) bad('no packed cell at the first address');
  else {
    const [wx, wy] = localToWorld(c0, c0.nucOffX, c0.nucOffY), h = 4;
    const win = [wx - h, wy - h, wx + h, wy + h, -1, 20];
    const all = W.sitesInWindow(...win), off = W0.sitesInWindow(...win);
    const lam = all.filter(d => d.structure === STRUCTURE_LAMIN), rest = all.filter(d => d.structure !== STRUCTURE_LAMIN);
    if (!lam.length) bad('no lamina dyes with the label on');
    if (off.some(d => d.structure === STRUCTURE_LAMIN)) bad('lamina dyes with the label off');
    if (JSON.stringify(rest) !== JSON.stringify(off)) bad('the lamina changes the other targets\' dyes');
    const pts = W.laminOfCell(cx0, cy0), reach = linkageReachNm(lk) * 1e-3 + 1e-9;
    const byCand = new Map(pts.map(q => [q.cand, q]));
    for (const d of lam) {
      const q = byCand.get(d.n);
      if (d.cx !== cx0 || d.cy !== cy0) continue;   // dyes of a neighbouring cell
      if (!q || d.k !== 0) { bad(`a lamina dye with no point (n ${d.n}, k ${d.k})`); break; }
      const [px, py] = localToWorld(c0, q.x, q.y);
      if (Math.hypot(d.x - px, d.y - py, d.z - q.z) > reach) { bad(`a lamina dye ${(Math.hypot(d.x - px, d.y - py, d.z - q.z) * 1000).toFixed(1)} nm from its point`); break; }
    }
    log(`lamin world: ${lam.length} lamina dyes, ${rest.length} others in an 8 um window over a nucleus, ${(performance.now() - tw).toFixed(0)} ms`);
  }
  log(`lamin check: seed ${seed}, ${addrs.length} cells, ${points} points on ${area.toFixed(0)} um^2 (${(points / area).toFixed(2)} /um^2, ` +
    `setting ${p.laminDensity}), caps ${capPoints} / ${(points * fCap).toFixed(1)} expected, nearest to a pore ${(nearest * 1000).toFixed(1)} nm ` +
    `(exclusion ${p.laminNpcExclNm} nm diameter), build ${msBuild.toFixed(0)} ms, total ${(performance.now() - t0).toFixed(0)} ms`);
  return fail;
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  const args = process.argv.slice(2);
  const opt = (k, d) => { const i = args.indexOf(k); return i >= 0 ? args[i + 1] : d; };
  const overrides = {};
  args.forEach((a, i) => { if (a === '--set') { const [k, v] = args[i + 1].split('='); overrides[k] = +v; } });
  const f = laminCheck({ seed: opt('--seed', null) != null ? +opt('--seed') : null, nCells: +opt('--cells', 4), overrides });
  console.log(f ? `lamin check: ${f} failure(s)` : 'lamin check: PASS');
  process.exit(f ? 1 : 0);
}
