// The fiducial beads' checks (Node, seconds): the field (determinism under any query order, density, the three height
// modes, the diameter spread), the light (bleach half-time at the reference, gold's scatter only when the filters
// let it through), the bead kernel (photons kept, wider than a point, a tiny bead = a point) and the cost (a movie
// with beads within 5 % of the same movie without in its frame loop).
//   node web/lab/fiducial_check.mjs [--no-timing]
// Exits 1 on a failed check. BSD-3-Clause.
import fs from 'fs';
import path from 'path';
import { fileURLToPath } from 'url';
import { loadPrototype } from '../../tests/parity/load_prototype.mjs';
import { World } from '../prototype/scope/world.js';
import { FiducialField, FIDUCIAL_TYPE_IDS, fiducialPhotophysics, beadKernel, splatBead, goldScatteringUm2 } from '../prototype/scope/fiducials.js';
import { renderScopeMovie, scopeKernel, parseSpec } from '../prototype/scope/scope_movie.js';
import { planSplat, splatRows, nearestZIndex } from '../prototype/scope/psf.js';
import { GRID_N, collectionEfficiency } from '../prototype/scope/spectra.js';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const args = process.argv.slice(2);
const t0 = performance.now();
let ok = true;
const check = (name, cond, detail = '') => { ok = ok && !!cond; console.log(`[${cond ? 'PASS' : 'FAIL'}] ${name} ${detail}`); };
const read = f => fs.readFileSync(path.join(ROOT, f), 'utf8');
const P = loadPrototype(read('web/prototype/index.html'), read('web/prototype/microtubules.js'));
const world = (o, seed = 1234) => new World(P, seed, { ...P.paramsFrom(P.defaults), ...o });

// ---- the field ----
{
  const flat = () => 0.0, F1 = new FiducialField(7, { fidDensity: 0.5 }, flat), F2 = new FiducialField(7, { fidDensity: 0.5 }, flat);
  const a = F1.inRect(-20, -20, 20, 20);
  F2.inRect(100, 100, 104, 104); F2.inRect(0, -20, 20, 20);
  const b = F2.inRect(-20, -20, 20, 20);
  check('same beads under any query order', JSON.stringify(a) === JSON.stringify(b), `(${a.length} beads)`);
  const big = new FiducialField(9, { fidDensity: 0.5 }, flat).inRect(-100, -100, 100, 100), want = 0.5 * 200 * 200;
  check('density = beads per um^2', Math.abs(big.length - want) < 4 * Math.sqrt(want), `(${big.length} vs ${want})`);
  const ds = big.map(b => b.d), m = ds.reduce((s, v) => s + v, 0) / ds.length;
  const sd = Math.sqrt(ds.reduce((s, v) => s + (v - m) ** 2, 0) / ds.length);
  check('diameter mean and spread', Math.abs(m - 200) < 1 && Math.abs(sd / m - 0.12) < 0.005, `(${m.toFixed(1)} nm, cv ${(sd / m * 100).toFixed(1)} %)`);
  check('Coverslip: centre at r', big.every(b => Math.abs(b.z - b.d / 2000) < 1e-12));
  const inner = big.filter(b => Math.abs(b.x) < 80 && Math.abs(b.y) < 80).slice(0, 400), nn = [];
  for (const a of inner) { let best = Infinity; for (const o of big) if (o !== a) best = Math.min(best, Math.hypot(a.x - o.x, a.y - o.y)); nn.push(best); }
  // Complete spatial randomness: mean nearest-neighbour distance 1 / (2 sqrt(density)) (Clark-Evans), no hard core.
  const mnn = nn.reduce((s, v) => s + v, 0) / nn.length;
  check('placement is Poisson (Clark-Evans)', Math.abs(mnn / (0.5 / Math.sqrt(0.5)) - 1) < 0.08 && Math.min(...nn) < 0.1,
    `(mean NN ${mnn.toFixed(3)} um vs ${(0.5 / Math.sqrt(0.5)).toFixed(3)}, min ${Math.min(...nn).toFixed(3)})`);
  const none = new FiducialField(9, { fidDensity: 0 }, flat).inRect(-100, -100, 100, 100);
  check('density 0: no beads', none.length === 0);
}
// Heights on the cell field: OnTop sits on the surface, Layer stays out of the cells.
{
  const W0 = world({ fidDensity: 2, fidHeight: 1 }), x0 = -30, y0 = -30, x1 = 30, y1 = 30;
  const top = W0.fiducialsInRect(x0, y0, x1, y1);
  const onCells = top.filter(b => b.z - b.d / 2000 > 0.05);
  check('OnTop: z = surface + r', top.every(b => Math.abs(b.z - b.d / 2000 - W0.surfaceHeightAt(b.x, b.y)) < 1e-12) && onCells.length > 0,
    `(${onCells.length} of ${top.length} on a cell, highest ${Math.max(...top.map(b => b.z)).toFixed(2)} um)`);
  const W1 = world({ fidDensity: 2, fidHeight: 2, fidLayerUm: 10 }), lay = W1.fiducialsInRect(x0, y0, x1, y1);
  const W2 = world({ fidDensity: 2, fidHeight: 0 }), all = W2.fiducialsInRect(x0, y0, x1, y1);
  check('Layer: within the layer, outside the cells', lay.every(b => b.z >= b.d / 2000 && b.z <= 10 - b.d / 2000 && b.z >= W1.surfaceHeightAt(b.x, b.y) + b.d / 2000)
    && lay.length < all.length, `(${lay.length} of ${all.length} candidates kept)`);
  check('same candidates in every mode (x, y, d)', lay.every(b => all.some(c => c.x === b.x && c.y === b.y && c.d === b.d)));
  const Wn = new World(P, 1234, { ...P.paramsFrom(P.defaults), specimen: 1, fidDensity: 2, fidHeight: 1 });
  check('nanorulers: OnTop at r', Wn.fiducialsInRect(-5, -5, 5, 5).every(b => b.z === b.d / 2000));
}

// ---- the light ----
{
  const ones = new Float64Array(GRID_N).fill(1), zeros = new Float64Array(GRID_N);
  const t = FIDUCIAL_TYPE_IDS.indexOf('FluoSpheresDarkRed');
  const lp = nm => ({ lasers: [{ nm, kWPerCm2: 1 }], dichroic: { T: zeros }, emissionFilter: { T: ones }, qe: ones, eta: collectionEfficiency(1.4, 1.518) });
  const c = fiducialPhotophysics(t, lp(651), { dyes: -1, halfSec: 28800, refKWcm2: 1, blockingOD: 6 })[0];
  check('bleach half-time at the reference = the option', Math.abs(Math.LN2 / c.bleachPerSec / 28800 - 1) < 1e-9, `(${(Math.LN2 / c.bleachPerSec).toFixed(1)} s)`);
  const c561 = fiducialPhotophysics(t, lp(561), { dyes: -1, halfSec: 28800, refKWcm2: 1, blockingOD: 6 })[0];
  check('dark red at 561: ~1 % excitation, ~100x slower bleaching', c561.bleachPerSec / c.bleachPerSec > 0.003 && c561.bleachPerSec / c.bleachPerSec < 0.03,
    `(x ${(c561.bleachPerSec / c.bleachPerSec).toFixed(4)})`);
  const g = FIDUCIAL_TYPE_IDS.indexOf('Gold');
  // A dichroic that reflects the laser to the sample also reflects its scatter back: only the blocking leaks it. A 50/50
  // beam splitter and no emission filter let it through.
  const half = new Float64Array(GRID_N).fill(0.5);
  const gold = (dT, emT) => fiducialPhotophysics(g, { ...lp(561), dichroic: { T: dT }, emissionFilter: { T: emT } }, { dyes: -1, halfSec: 28800, refKWcm2: 1, blockingOD: 6 });
  const blocked = gold(zeros, zeros).find(x => x.scatter), open = gold(half, ones).find(x => x.scatter);
  check('gold scatter: blocked at OD 6, bright without filters', blocked && open && open.perUm2 / blocked.perUm2 > 1e5,
    `(${(blocked.perUm2 * goldScatteringUm2(561, 100)).toExponential(2)} vs ${(open.perUm2 * goldScatteringUm2(561, 100)).toExponential(2)} photons/s per 100 nm bead)`);
}

// ---- the bead kernel ----
{
  const spec = parseSpec('psf-kernel-half-width-nm=3000');
  const K = scopeKernel(spec, 680), B = beadKernel(K, 100), N = 61, c = 30.2;
  const img = d => { const a = new Float32Array(N * N); if (d) splatBead(a, N, N, B, nearestZIndex(K, 0), d, c, c, 1.0); else splatRows(a, N, N, 0, N, K, planSplat(K, nearestZIndex(K, 0), c, c, 1.0, K.interpMode), 1.0); return a; };
  const mom = a => { let s = 0, r2 = 0; for (let y = 0; y < N; y++) for (let x = 0; x < N; x++) { s += a[y * N + x]; r2 += a[y * N + x] * ((x - c) ** 2 + (y - c) ** 2); } return { s, r: Math.sqrt(r2 / s) }; };
  const p = mom(img(0)), b200 = mom(img(200)), b400 = mom(img(400)), b10 = img(10), p0 = img(0);
  check('bead kernel keeps the photons', Math.abs(b200.s / p.s - 1) < 2e-3 && Math.abs(b400.s / p.s - 1) < 5e-3, `(${b200.s.toFixed(5)}, ${b400.s.toFixed(5)} vs ${p.s.toFixed(5)})`);
  // The centre's second moment: the sphere adds r^2 / 5 per axis, 2 r^2 / 5 in the plane (the halo dominates the full one).
  const core = a => { let s = 0, r2 = 0; for (let y = 0; y < N; y++) for (let x = 0; x < N; x++) { const d2 = (x - c) ** 2 + (y - c) ** 2; if (d2 > 9) continue; s += a[y * N + x]; r2 += a[y * N + x] * d2; } return r2 / s; };
  const w0 = core(p0), w2 = core(img(200)), w4 = core(img(400));
  check('a 200 / 400 nm bead is wider than a point', w4 > w2 && w2 > w0, `(core RMS ${Math.sqrt(w0).toFixed(3)} -> ${Math.sqrt(w2).toFixed(3)} -> ${Math.sqrt(w4).toFixed(3)} px)`);
  const dmax = b10.reduce((m, v, i) => Math.max(m, Math.abs(v - p0[i])), 0), peak = Math.max(...p0);
  check('a 10 nm bead = a point', dmax / peak < 0.02, `(max diff ${(dmax / peak * 100).toFixed(2)} % of the peak)`);
}

// ---- movies: bleaching over time, cost ----
{
  const base = 'size=64 frames=1 psf-kernel-half-width-nm=3000 mt-dye=None npc-dye=None light-preset=-1 laser-640=1 illum-profile=0 fid-density=0.2';
  const sum = spec => { let s = 0; renderScopeMovie(P, spec, (f, adu, ph) => { for (const v of ph) s += v; }); return s; };
  const s0 = sum(`${base} start-sec=0`), info = { v: null };
  renderScopeMovie(P, `${base} start-sec=0`, () => {}, {});
  const r = renderScopeMovie(P, `${base} start-sec=0`, () => {});
  const half = r.fiducials.components[0].bleachHalfSec;
  const s1 = sum(`${base} start-sec=${half}`);
  check('bead signal halves after its half-time', Math.abs(s1 / s0 - 0.5) < 2e-3, `(${(s1 / s0).toFixed(4)} after ${half.toFixed(0)} s at 640 nm)`);
  void info;
  // Frame by frame under a flat beam (every bead one rate): sum_f / sum_0 = e^-L f exp, through the Taylor images
  // (a 6.5 s half-time at 640 nm: order ~5) and through per-frame splats (0.65 s: no order reaches the tolerance).
  for (const halfSec of [5, 0.5]) {
    const sums = [], spec = `${base.replace('frames=1', 'frames=20')} start-sec=0 fid-bleach-half-sec=${halfSec}`;
    const ri = renderScopeMovie(P, spec, (f, adu, ph) => { let t = 0; for (const v of ph) t += v; sums.push(t); });
    const L = Math.LN2 / ri.fiducials.components[0].bleachHalfSec;
    const err = sums.reduce((m, v, f) => Math.max(m, Math.abs(v / sums[0] / Math.exp(-L * f * 0.05) - 1)), 0);
    check(`bleaching frame by frame (half-time ${ri.fiducials.components[0].bleachHalfSec.toFixed(2)} s)`, err < 1e-5, `(max rel. error ${err.toExponential(1)})`);
  }
}
if (!args.includes('--no-timing')) {
  // The beads' own time (their query, kernels' bead corrections, static images, per-frame adds; info.fiducials.sec)
  // against the whole movie, on a warm world and kernel; with drift every bead is splatted every frame.
  for (const extra of ['', ' drift-xy-speed-nm-per-sec=20', ' fid-bleach-half-sec=60']) {
    const spec = `size=128 frames=60 psf-kernel-half-width-nm=3000 fid-density=0.05 fid-type=TetraSpeck${extra}`;
    renderScopeMovie(P, spec, () => {});
    const info = renderScopeMovie(P, spec, () => {}), share = info.fiducials.sec / info.totalSec;
    check(`beads < 5 % of the movie${extra}`, share < 0.05,
      `(${(info.fiducials.sec * 1000).toFixed(0)} ms of ${info.totalSec.toFixed(2)} s: ${(share * 100).toFixed(1)} %, ${info.fiducials.beads} TetraSpecks, 60 frames 128 px)`);
  }
}
console.log(`${ok ? 'fiducial_check: all passed' : 'fiducial_check: FAILED'} (${((performance.now() - t0) / 1000).toFixed(1)} s)`);
process.exit(ok ? 0 : 1);
