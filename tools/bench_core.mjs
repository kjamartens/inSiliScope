// Phase timing of the core's WASM module under Node (the viewer's cost
// profile: serial). Packing, per-cell assets, dyes, schedules, the density
// and optical-volume queries, for one window.
//
//   node tools/bench_core.mjs [--module build/wasm/core/insiliscope.js] [--seed 42] [--size 128] [--x 0 --y 0]
//
// The native equivalent (per phase, inside the library) is tests/parity/core_bench.cpp.
import fs from 'node:fs';
import path from 'node:path';
import { createRequire } from 'node:module';

const args = process.argv.slice(2);
const opt = (name, def) => { const i = args.indexOf('--' + name); return i >= 0 ? args[i + 1] : def; };
const modulePath = path.resolve(opt('module', 'build/wasm/core/insiliscope.js'));
const seed = +opt('seed', 42), size = +opt('size', 128), cx = +opt('x', 0), cy = +opt('y', 0);

let createInsiliscope;
try { createInsiliscope = createRequire(import.meta.url)(modulePath); } catch (e) { createInsiliscope = undefined; }
if (typeof createInsiliscope !== 'function') {
  const src = fs.readFileSync(modulePath, 'utf8');
  (0, eval)(src + '\n;globalThis.__isc_factory = createInsiliscope;');
  createInsiliscope = globalThis.__isc_factory;
}
const M = await createInsiliscope();
const now = () => performance.now();
const line = (what, ms, extra = '') => console.log('  ' + what.padEnd(34) + (ms.toFixed(1) + ' ms').padStart(12) + '  ' + extra);

const W = size * 0.1, x0 = cx - W / 2, y0 = cy - W / 2, x1 = cx + W / 2, y1 = cy + W / 2;
console.log(`core bench (WASM, serial): seed ${seed}, window ${W.toFixed(1)} um at (${cx}, ${cy})`);
const pp = M._isc_params_new();
const w = M._isc_world_new(seed >>> 0, pp);

let t0 = now();
const cap = 4096, cb = M._malloc(cap * 14 * 8);
const nc = M._isc_cells_in_window(w, x0, y0, x1, y1, cb, cap);
line('cells_in_window cold (packing)', now() - t0, `(${nc} cells)`);
const cells = M.HEAPF64.slice(cb / 8, cb / 8 + nc * 14);

const ob = M._malloc(4096 * 2 * 8), dims = M._malloc(8), mb = M._malloc(400000 * 3 * 8);
const xyz = M._malloc(3000000 * 3 * 8), lens = M._malloc(20000 * 4), tot = M._malloc(4);
let tOut = 0, tMesh = 0, tMt = 0, nMt = 0, nPts = 0, nVerts = 0;
for (let i = 0; i < nc; i++) {
  const ccx = cells[i * 14], ccy = cells[i * 14 + 1];
  let a = now(); M._isc_cell_outline(w, ccx, ccy, ob, 4096); tOut += now() - a;
  a = now(); nVerts += M._isc_cell_mesh(w, ccx, ccy, dims, mb, 400000); tMesh += now() - a;
  a = now(); nMt += M._isc_cell_microtubules(w, ccx, ccy, xyz, 3000000, lens, 20000, tot); tMt += now() - a;
  nPts += M.HEAP32[tot / 4];
}
line('cell_outline (all cells)', tOut, `(${nc} cells)`);
line('cell_mesh (assets build)', tMesh, `(${nVerts} vertices)`);
line('cell_microtubules', tMt, `(${nMt} MTs, ${nPts} points)`);

const sc = 1 << 22, sb = M._malloc(sc * 4 * 8);
t0 = now();
const ns = M._isc_sites_in_window(w, x0, y0, x1, y1, -Infinity, Infinity, sb, sc);
line('sites_in_window cold (dyes)', now() - t0, `(${ns} dyes)`);
t0 = now();
M._isc_sites_in_window(w, x0, y0, x1, y1, -Infinity, Infinity, sb, sc);
line('sites_in_window warm', now() - t0);

M._isc_world_set_kinetics(w, 0.00143, 0.05, 1, 1, 0.5);
const eb = M._malloc((1 << 20) * 7 * 8);
t0 = now();
const ne = M._isc_events_in_window(w, x0, y0, x1, y1, -3, 4, 0, 0.05, eb, 1 << 20);
line('events_in_window first (schedules)', now() - t0, `(${ne} events)`);
t0 = now();
let nev = 0;
for (let f = 1; f <= 100; f++) nev += M._isc_events_in_window(w, x0, y0, x1, y1, -3, 4, f * 0.05, (f + 1) * 0.05, eb, 1 << 20);
line('events_in_window steady / frame', (now() - t0) / 100, `(100 frames, ${nev} events)`);

const db = M._malloc(size * size * 280 * 4);
t0 = now();
M._isc_density3d_in_window(w, x0, y0, x1, y1, -3, 4, size, size, 280, 3, db);
line(`density3d ${size}x${size}x280`, now() - t0);
const vb = M._malloc(3 * size * size * 20 * 4);
t0 = now();
M._isc_optical_volume_in_window(w, x0, y0, x1, y1, -0.5, 9.5, size, size, 20, 2, vb);
line(`optical_volume ${size}x${size}x20 sub 2`, now() - t0);

// A second window elsewhere: everything cold again (the viewer's pan).
const w2 = M._isc_world_new(seed >>> 0, pp);
t0 = now();
const n2 = M._isc_cells_in_window(w2, x0 + 30, y0 + 10, x1 + 30, y1 + 10, cb, cap);
line('cells cold at (+30, +10)', now() - t0, `(${n2} cells)`);
t0 = now();
M._isc_sites_in_window(w2, x0 + 30, y0 + 10, x1 + 30, y1 + 10, -Infinity, Infinity, sb, sc);
line('sites cold at (+30, +10)', now() - t0);
