// Smoke test of the WASM C ABI as a JS consumer would use it: packs a few
// parity cases through isc_pack_window and checks them against the JS
// reference output (build/parity/ref_js.txt from run.mjs), then drives the
// world queries (cells / sites / density / events in a window; labels, ABI 10).
//   node tests/parity/wasm_abi_smoke.mjs
import fs from 'fs';
import path from 'path';
import { createRequire } from 'module';
import { fileURLToPath } from 'url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const require = createRequire(import.meta.url);
const createInsiliscope = require(path.join(ROOT, 'build/wasm/core/insiliscope.js'));
const M = await createInsiliscope();

const cases = fs.readFileSync(path.join(ROOT, 'build/parity/cases.txt'), 'utf8').split(/\r?\n/);
const ref = fs.readFileSync(path.join(ROOT, 'build/parity/ref_js.txt'), 'utf8').split(/\r?\n/);

const paramSets = {};
for (const l of cases) {
  const t = l.split(' ');
  if (t[0] === 'params') paramSets[t[1]] = t.slice(2).map(kv => kv.split('='));
}
function makeParams(kvs) {
  const p = M._isc_params_new();
  for (const [k, v] of kvs) {
    const n = M.lengthBytesUTF8(k) + 1, s = M._malloc(n);
    M.stringToUTF8(k, s, n);
    if (M._isc_params_set(p, s, +v) !== 0) throw new Error('unknown param ' + k);
    M._free(s);
  }
  return p;
}

let checked = 0, bad = 0;
for (const l of cases.filter(l => l.startsWith('case ')).filter((_, i) => i % 5 === 0)) {
  const [, id, seed, pname, cx0, cy0, cx1, cy1] = l.split(' ');
  const p = makeParams(paramSets[pname]);
  const cap = 4096, buf = M._malloc(cap * 5 * 8), rem = M._malloc(4);
  const n = M._isc_pack_window(+seed >>> 0, +cx0, +cy0, +cx1, +cy1, p, buf, cap, rem);
  const got = new Map();
  for (let i = 0; i < n; i++) {
    const o = buf / 8 + i * 5;
    got.set(`${M.HEAPF64[o]},${M.HEAPF64[o + 1]}`, [M.HEAPF64[o + 2], M.HEAPF64[o + 3], M.HEAPF64[o + 4]]);
  }
  M._free(buf); M._free(rem); M._isc_params_free(p);

  const start = ref.indexOf(ref.find(r => r.startsWith(`case ${id} `)));
  const want = new Map();
  for (let i = start + 1; i < ref.length && !ref[i].startsWith('case '); i++) {
    const t = ref[i].split(' ');
    if (t[0] === 'pk') want.set(`${t[1]},${t[2]}`, t.slice(3).map(Number));
  }
  let ok = want.size === got.size;
  for (const [k, v] of want) {
    const g = got.get(k);
    ok &&= !!g && v.every((x, j) => Object.is(x, g[j]));
  }
  checked++;
  if (!ok) bad++;
  console.log(`${id}: ${n} cells via isc_pack_window -> ${ok ? 'bit-exact vs JS' : 'MISMATCH'}`);
}
console.log(`${checked - bad}/${checked} cases bit-exact through the WASM C ABI`);

// ---- world queries ----
{
  const p = makeParams([]);
  const w = M._isc_world_new(1249, p);
  // ABI 10: the microtubules' label, PALM at 5 % (density, fluorescent fraction, mode).
  const lab = M._malloc(16 * 8);
  M.HEAPF64.set([0.05, 1, 1], lab / 8);
  if (M._isc_world_set_label(w, 0, lab, 3) !== 0) throw new Error('isc_world_set_label failed');
  const win = [-4, -7, 0, -3, -Infinity, Infinity];   // beside the nucleus of cell (-1,-1)
  const t0 = performance.now();
  const n = M._isc_sites_in_window(w, ...win, 0, 0);
  const buf = M._malloc(Math.max(1, n) * 5 * 8);
  const n2 = M._isc_sites_in_window(w, ...win, buf, n);
  const ms = performance.now() - t0;
  let sum = 0, zmin = Infinity, zmax = -Infinity;
  for (let i = 0; i < n2; i++) {
    const o = buf / 8 + i * 5;
    sum += M.HEAPF64[o] + M.HEAPF64[o + 1];
    zmin = Math.min(zmin, M.HEAPF64[o + 2]); zmax = Math.max(zmax, M.HEAPF64[o + 2]);
  }
  const g = M._malloc(16 * 16 * 4);
  const n3 = M._isc_density_in_window(w, ...win, 16, 16, g);
  let gs = 0;
  for (let i = 0; i < 256; i++) gs += M.HEAPF32[g / 4 + i];
  const nc = M._isc_cells_in_window(w, win[0], win[1], win[2], win[3], 0, 0);
  // ABI 5: z-resolved density (ABI 10: by structure), summed over 4 planes.
  const g3 = M._malloc(16 * 16 * 4 * 4);
  const n4 = M._isc_density3d_in_window(w, ...win.slice(0, 4), -10, 60, 16, 16, 4, 1, g3);
  let g3s = 0;
  for (let i = 0; i < 1024; i++) g3s += M.HEAPF32[g3 / 4 + i];
  M._free(g3);
  const cstr = p => { const u = new Uint8Array(M.HEAP32.buffer, p, 64); let i = 0; while (i < 64 && u[i]) i++; return String.fromCharCode(...u.subarray(0, i)); };
  // ABI 9: the nucleus rings of the window's first cell (17 x 24 finite points).
  const cb = M._malloc(Math.max(1, nc) * 14 * 8);
  M._isc_cells_in_window(w, win[0], win[1], win[2], win[3], cb, nc);
  const rb = M._malloc(17 * 24 * 3 * 8);
  const nr = M._isc_cell_nucleus_rings(w, M.HEAPF64[cb / 8], M.HEAPF64[cb / 8 + 1], 17, 24, rb, 17 * 24);
  let ringsOk = nr === 17 * 24;
  for (let i = 0; i < nr * 3; i++) ringsOk = ringsOk && Number.isFinite(M.HEAPF64[rb / 8 + i]);
  M._free(cb); M._free(rb);
  const ok = n > 0 && n2 === n && n3 === n && gs === n && nc > 0 && n4 === g3s && n4 === n && ringsOk &&
    M._isc_abi_version() === 10 && cstr(M._isc_world_version()).length >= 10;   // ABI 8: the generator's version
  console.log(`world: ${nc} cells, ${n} dyes (z ${zmin.toFixed(2)}..${zmax.toFixed(2)} um, xy checksum ${sum.toFixed(6)}) ` +
    `in ${ms.toFixed(0)} ms, density sum ${gs} -> ${ok ? 'ok' : 'MISMATCH'}`);
  if (!ok) bad++;
  M.HEAPF64.set([0.05, 1, 1, 0.5, 0.05, 0.5, 0.3, 0.4], lab / 8);
  M._isc_world_set_label(w, 0, lab, 8);
  const te = performance.now();
  const ne = M._isc_events_in_window(w, win[0], win[1], win[2], win[3], win[4], win[5], 1.0, 1.1, 0, 0);
  const eb = M._malloc(Math.max(1, ne) * 10 * 8);
  const ne2 = M._isc_events_in_window(w, win[0], win[1], win[2], win[3], win[4], win[5], 1.0, 1.1, eb, ne);
  let eok = ne > 0 && ne2 === ne;
  for (let i = 0; i < ne2; i++) {
    const o = eb / 8 + i * 10;
    eok &&= M.HEAPF64[o + 3] < 1.1 && M.HEAPF64[o + 4] > 1.0 && M.HEAPF64[o + 5] > 0 && M.HEAPF64[o + 7] === 0 && M.HEAPF64[o + 8] === 0;
  }
  console.log(`events: ${ne} blinks overlapping [1.0, 1.1) s in ${(performance.now() - te).toFixed(0)} ms -> ${eok ? 'ok' : 'MISMATCH'}`);
  if (!eok) bad++;
  M._free(eb);
  // ABI 10: continuous windows (dSTORM initial ON, 2 s): one per dye.
  M.HEAPF64.set([0.05, 1, 0, 0.5, 0.05, 0.5, 0.3, 0.4, 2], lab / 8);
  M._isc_world_set_label(w, 0, lab, 9);
  const nw = M._isc_continuous_in_window(w, ...win, -Infinity, 0, 0);
  const cok = nw === n;
  console.log(`continuous: ${nw} initial-ON windows for ${n} dyes -> ${cok ? 'ok' : 'MISMATCH'}`);
  if (!cok) bad++;
  M._free(lab);
  M._free(buf); M._free(g); M._isc_world_free(w); M._isc_params_free(p);
}
process.exit(bad ? 1 : 0);
