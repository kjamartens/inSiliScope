// Headless lab check (Node, seconds): the working-tree prototype vs a git baseline.
//   node web/lab/check.mjs [--base origin/main] [--seed N] [--cells 3] [--set mtWobbleTurn=1.2 ...]
// Fails on: a load error, NaN geometry, or a cell that differs with query history (address determinism).
// Prints shape metrics working vs baseline. No build, no C++.
import fs from 'fs';
import path from 'path';
import { execFileSync } from 'child_process';
import { fileURLToPath } from 'url';
import { loadPrototype } from '../../tests/parity/load_prototype.mjs';
import { makeField } from './field.js';
import { formatSummary } from './metrics.js';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const args = process.argv.slice(2);
const opt = (k, d) => { const i = args.indexOf(k); return i >= 0 ? args[i + 1] : d; };
const overrides = {};
args.forEach((a, i) => { if (a === '--set') { const [k, v] = args[i + 1].split('='); overrides[k] = v === 'true' ? true : v === 'false' ? false : +v; } });
let baseRef = opt('--base', 'origin/main');
const nCells = +opt('--cells', 3);

const git = (...a) => execFileSync('git', a, { cwd: ROOT, encoding: 'utf8', stdio: ['ignore', 'pipe', 'ignore'] });
const read = f => fs.readFileSync(path.join(ROOT, f), 'utf8');
const load = (html, mt) => makeField(loadPrototype(html, mt));

let fail = 0;
const bad = msg => { console.log('FAIL ' + msg); fail++; };

const work = () => load(read('web/prototype/index.html'), read('web/prototype/microtubules.js'));
const W = work();
let B = null;
for (const ref of [baseRef, 'main']) {
  try { B = load(git('show', `${ref}:web/prototype/index.html`), git('show', `${ref}:web/prototype/microtubules.js`)); baseRef = ref; break; }
  catch { /* try the next ref */ }
}
const seed = +opt('--seed', loadPrototype(read('web/prototype/index.html'), read('web/prototype/microtubules.js')).defaults.seed);

// Cells to look at: the first nCells chunks with a candidate, walking right from (0, 0).
const addrs = [];
for (let cx = 0, cy = 0; addrs.length < nCells;) {
  [cx, cy] = W.findCell(seed, overrides, cx, cy, 1);
  addrs.push([cx, cy]);
  cx++;
}

const t0 = performance.now();
const sig = c => JSON.stringify(c.mtsLocal);
const workCells = [];
for (const [cx, cy] of addrs) {
  const r = W.cells(seed, overrides, cx, cy, cx, cy);
  for (const c of r.cells) {
    workCells.push(c);
    for (const pts of c.mtsLocal) for (const q of pts) if (!isFinite(q.x) || !isFinite(q.y) || !isFinite(q.z)) { bad(`NaN in MT of cell ${cx},${cy}`); break; }
  }
}
const msWork = performance.now() - t0;

// Determinism: the same cells from a fresh instance, queried in reverse order with a far-away cell first.
const W2 = work();
W2.cells(seed, overrides, 40, -37, 40, -37, { withMts: false });
for (const [cx, cy] of [...addrs].reverse()) {
  const a = W2.cells(seed, overrides, cx, cy, cx, cy).cells.find(c => c.cx === cx && c.cy === cy);
  const b = workCells.find(c => c.cx === cx && c.cy === cy);
  if (!!a !== !!b || (a && sig(a) !== sig(b))) bad(`cell ${cx},${cy} depends on query history`);
}

const sw = W.metrics(workCells);
let sb = null;
if (B) {
  const baseCells = [];
  for (const [cx, cy] of addrs) baseCells.push(...B.cells(seed, overrides, cx, cy, cx, cy).cells);
  sb = B.metrics(baseCells);
}

console.log(`lab check: seed ${seed}, cells ${addrs.map(a => a.join(',')).join(' ')}` +
  (Object.keys(overrides).length ? `, overrides ${JSON.stringify(overrides)}` : '') + `, working ${msWork.toFixed(0)} ms`);
const rows = formatSummary(sw, sb);
const w0 = Math.max(...rows.map(r => r.name.length));
console.log(`${'metric'.padEnd(w0)}  ${'working'.padStart(10)}  ${(sb ? baseRef : '').padStart(12)}  ${sb ? 'delta' : ''}`);
for (const r of rows) console.log(`${r.name.padEnd(w0)}  ${r.a.padStart(10)}  ${r.b.padStart(12)}  ${r.d}`);
console.log(fail ? `lab check: ${fail} failure(s)` : 'lab check: PASS');
process.exit(fail ? 1 : 0);
