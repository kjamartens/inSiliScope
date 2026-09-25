// Smoke test of the WASM C ABI as a JS consumer would use it: packs a few
// parity cases through isc_pack_window and checks them against the JS
// reference output (build/parity/ref_js.txt from run.mjs).
//   node tests/parity/wasm_abi_smoke.mjs
import fs from 'fs';
import path from 'path';
import { createRequire } from 'module';
import { fileURLToPath } from 'url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const require = createRequire(import.meta.url);
const createInsilicell = require(path.join(ROOT, 'build/wasm/core/insilicell.js'));
const M = await createInsilicell();

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
process.exit(bad ? 1 : 0);
