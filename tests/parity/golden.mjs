// Golden-vector test: runs an isc_parity build (native exe, or the WASM .js
// under Node) on spec/golden/cases.txt and requires bit-identical output to
// the frozen reference spec/golden/ref.txt (mesh / microtubule lines within
// 1e-6 um: they depend on the libm's pow). Runs anywhere (CI, both toolchains).
//
//   node tests/parity/golden.mjs <isc_parity exe | isc_parity.js>
//   node tests/parity/golden.mjs --freeze <isc_parity exe | isc_parity.js>   # regenerate the golden files
//
// ref.txt was frozen from the JS reference (web/prototype, retired 2026-10-28)
// with Node 24, which the C++ matched bit for bit. Freeze again only when the
// generator changed on purpose (and bump ISC_WORLD_VERSION when a cell moves);
// commit the new files with that change, frozen from the native MSVC build.
import fs from 'fs';
import os from 'os';
import path from 'path';
import { execFileSync } from 'child_process';
import { fileURLToPath } from 'url';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const ROOT = path.resolve(HERE, '../..');
const GOLD = path.join(ROOT, 'spec/golden');
const CASES = path.join(GOLD, 'cases.txt');
const REF = path.join(GOLD, 'ref.txt');
const MATH_SAMPLES = 100;   // per function (2026-10-27, was 2000)

const node = (args, opts = {}) => execFileSync(process.execPath, args, { stdio: 'inherit', ...opts });

const freeze = process.argv[2] === '--freeze', arg = process.argv[freeze ? 3 : 2];
if (!arg) {
  console.error('usage: golden.mjs [--freeze] <isc_parity exe | isc_parity.js>');
  process.exit(2);
}
const run = (cases, out) => (arg.endsWith('.js') ? node([arg, cases, out]) : execFileSync(arg, [cases, out], { stdio: 'inherit' }));
if (freeze) {
  fs.mkdirSync(GOLD, { recursive: true });
  node([path.join(HERE, 'make_cases.mjs'), CASES, String(MATH_SAMPLES)]);
  run(CASES, REF);
  // The timings are machine noise; zero them so re-freezing only diffs on real changes.
  fs.writeFileSync(REF, fs.readFileSync(REF, 'utf8').replace(/ ms [0-9.]+$/gm, ' ms 0'));
  process.exit(0);
}

const out = path.join(fs.mkdtempSync(path.join(os.tmpdir(), 'isc-golden-')), 'out.txt');
run(CASES, out);
try {
  node([path.join(HERE, 'compare.mjs'), REF, `${path.basename(arg)}=${out}`, '--tol', '0', '--geomtol', '1e-6'],
    { stdio: ['ignore', 'pipe', 'inherit'] });
  console.log(`golden vectors: PASS (${path.basename(arg)} matches spec/golden/ref.txt; see the table for equal/near)`);
} catch (e) {
  console.log(e.stdout?.toString() ?? '');
  console.error('golden vectors: FAIL');
  process.exit(1);
}
