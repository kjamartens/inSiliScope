// Statistics of the nuclear pore placement (issue 15) on the working-tree prototype, Node, seconds:
//   node web/lab/npc_check.mjs [--seed N] [--cells 6] [--set npcDensity=8 ...]
// Fails on: a pore count != round(density x mesh area) below jamming, two centres closer than npcMinDistNm, a centre
// off the membrane (+-1 nm along its normal must be outside / inside the nucleus), a non-orthonormal frame, cap vs
// side counts off their area fractions (pooled, 4 sigma), a pore set that depends on query history, a wrong
// attachment-point geometry. Part of web/lab/check.mjs.
import fs from 'fs';
import path from 'path';
import { fileURLToPath } from 'url';
import { loadPrototype } from '../../tests/parity/load_prototype.mjs';
import { makeField } from './field.js';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');

// Returns the number of failures; prints one line per finding and a summary.
export function npcCheck({ seed: seedArg = null, nCells = 6, overrides = {}, log = console.log } = {}) {
  const read = f => fs.readFileSync(path.join(ROOT, f), 'utf8');
  const html = read('web/prototype/index.html'), mt = read('web/prototype/microtubules.js');
  const P = loadPrototype(html, mt), F = makeField(P), g = P.gen;
  const seed = seedArg ?? P.defaults.seed;
  const p = P.paramsFrom({ ...P.defaults, ...overrides });
  let fail = 0;
  const bad = m => { log('FAIL npc: ' + m); fail++; };
  const t0 = performance.now();
  const addrs = [];
  for (let cx = 0, cy = 0; addrs.length < nCells;) { [cx, cy] = F.findCell(seed, overrides, cx, cy, 1); addrs.push([cx, cy]); cx++; }
  const dMin = p.npcMinDistNm * 1e-3;
  const jamming = 0.547 / (Math.PI * (dMin / 2) ** 2);
  let pores = 0, area = 0, capArea = 0, capPores = 0, msBuild = 0;
  const sigs = [];
  for (const [cx, cy] of addrs) {
    const c = g.rawCandidate(seed, cx, cy, p);   // pores are cell-local: packing moves and turns the cell only
    const tb = performance.now();
    const list = g.buildNpcsForCell(seed, cx, cy, c, p);
    msBuild += performance.now() - tb;
    const M = g.nucSurfaceMesh(c);
    const want = Math.floor(p.npcDensity * M.area + 0.5);
    if (p.npcDensity < 0.8 * jamming && list.length !== want) bad(`cell ${cx},${cy}: ${list.length} pores, want ${want}`);
    pores += list.length; area += M.area;
    // The caps' share of the (flat) area.
    let ca = 0;
    for (let t = 0; t < M.tris.length; t++) if (M.tris[t][0] !== 0) ca += M.cdf[t] - (t ? M.cdf[t - 1] : 0);
    capArea += ca;
    sigs.push(JSON.stringify(list.map(q => [q.x, q.y, q.z, q.cand])));
    for (let i = 0; i < list.length; i++) {
      const q = list[i];
      const nn = Math.hypot(...q.n), n1 = Math.hypot(...q.e1), n2 = Math.hypot(...q.e2);
      const d1 = q.n[0] * q.e1[0] + q.n[1] * q.e1[1] + q.n[2] * q.e1[2];
      const d2 = q.n[0] * q.e2[0] + q.n[1] * q.e2[1] + q.n[2] * q.e2[2];
      const d3 = q.e1[0] * q.e2[0] + q.e1[1] * q.e2[1] + q.e1[2] * q.e2[2];
      if (Math.max(Math.abs(nn - 1), Math.abs(n1 - 1), Math.abs(n2 - 1), Math.abs(d1), Math.abs(d2), Math.abs(d3)) > 1e-9)
        { bad(`cell ${cx},${cy} pore ${i}: frame not orthonormal`); break; }
      // Right-handed: e1 x e2 = n.
      const cxv = q.e1[1] * q.e2[2] - q.e1[2] * q.e2[1], cyv = q.e1[2] * q.e2[0] - q.e1[0] * q.e2[2], czv = q.e1[0] * q.e2[1] - q.e1[1] * q.e2[0];
      if (Math.abs(cxv - q.n[0]) + Math.abs(cyv - q.n[1]) + Math.abs(czv - q.n[2]) > 1e-9) { bad(`cell ${cx},${cy} pore ${i}: e1 x e2 != n`); break; }
      // On the membrane, normal outward: 1 nm out is outside the nucleus, 1 nm in is inside.
      const inside = (x, y, z) => { const col = g.nucleusColumnLocal(c, x, y); return !!col && z > c.nucZ - col[0] && z < c.nucZ + col[1]; };
      const e = 1e-3;
      if (inside(q.x + e * q.n[0], q.y + e * q.n[1], q.z + e * q.n[2]) || !inside(q.x - e * q.n[0], q.y - e * q.n[1], q.z - e * q.n[2]))
        { bad(`cell ${cx},${cy} pore ${i}: not on the membrane with an outward normal`); break; }
      if (q.piece !== 0) capPores++;
      for (let j = 0; j < i; j++) {
        const o = list[j], dd = (q.x - o.x) ** 2 + (q.y - o.y) ** 2 + (q.z - o.z) ** 2;
        if (dd < dMin * dMin) { bad(`cell ${cx},${cy}: pores ${j}, ${i} ${(Math.sqrt(dd) * 1000).toFixed(1)} nm apart`); break; }
      }
    }
  }
  // History: a fresh instance, a far cell first, the cells in reverse.
  const P2 = loadPrototype(html, mt);
  P2.gen.buildNpcsForCell(seed, 40, -37, P2.gen.rawCandidate(seed, 40, -37, p), p);
  [...addrs].reverse().forEach(([cx, cy]) => {
    const c = P2.gen.rawCandidate(seed, cx, cy, p);
    const s = JSON.stringify(P2.gen.buildNpcsForCell(seed, cx, cy, c, p).map(q => [q.x, q.y, q.z, q.cand]));
    if (s !== sigs[addrs.findIndex(a => a[0] === cx && a[1] === cy)]) bad(`cell ${cx},${cy}: pores depend on query history`);
  });
  // Attachment points: 64, two rings at -/+ sep/2, mean radius = the mean of |(R + r cos phi, r sin phi)| over the arc.
  const S = g.npcAttachmentPoints(p);
  let r = 0, zs = 0;
  for (let k = 0; k < 64; k++) { r += Math.hypot(S[3 * k], S[3 * k + 1]); zs += S[3 * k + 2] * (k < 32 ? -1 : 1); }
  const ar = p.npcCornerSpreadNm / 2;
  const rWant = [-67.5, -22.5, 22.5, 67.5].reduce((s, d) => s + Math.hypot(p.npcRadiusNm + ar * Math.cos(d * Math.PI / 180), ar * Math.sin(d * Math.PI / 180)), 0) / 4;
  if (S.length !== 192 || Math.abs(r / 64 - rWant) > 1e-9 || Math.abs(zs / 64 - p.npcRingSepNm / 2) > 1e-9)
    bad(`attachment points: mean radius ${(r / 64).toFixed(2)} (want ${rWant.toFixed(2)}), half separation ${(zs / 64).toFixed(2)}`);
  // Area uniformity: pores on the caps vs the caps' share of the area (binomial, 4 sigma).
  const fCap = capArea / area, sigma = Math.sqrt(pores * fCap * (1 - fCap));
  if (pores > 0 && fCap > 0 && Math.abs(capPores - pores * fCap) > 4 * sigma + 1)
    bad(`cap pores ${capPores} vs ${(pores * fCap).toFixed(1)} expected (+-${sigma.toFixed(1)})`);
  log(`npc check: seed ${seed}, ${addrs.length} cells, ${pores} pores on ${area.toFixed(0)} um^2 (${(pores / area).toFixed(2)} /um^2, ` +
    `setting ${p.npcDensity}), caps ${capPores} pores / ${(pores * fCap).toFixed(1)} expected, build ${msBuild.toFixed(0)} ms, ` +
    `total ${(performance.now() - t0).toFixed(0)} ms`);
  return fail;
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  const args = process.argv.slice(2);
  const opt = (k, d) => { const i = args.indexOf(k); return i >= 0 ? args[i + 1] : d; };
  const overrides = {};
  args.forEach((a, i) => { if (a === '--set') { const [k, v] = args[i + 1].split('='); overrides[k] = +v; } });
  const f = npcCheck({ seed: opt('--seed', null) != null ? +opt('--seed') : null, nCells: +opt('--cells', 6), overrides });
  console.log(f ? `npc check: ${f} failure(s)` : 'npc check: PASS');
  process.exit(f ? 1 : 0);
}
