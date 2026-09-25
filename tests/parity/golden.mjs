// Golden-vector test: runs an isc_parity build (native exe, or the WASM .js
// under Node) on spec/golden/cases.txt and requires bit-identical output to
// the frozen JS-prototype reference spec/golden/ref_js.txt. Needs no JS
// prototype, so it runs anywhere (CI, both toolchains).
//
//   node tests/parity/golden.mjs <isc_parity exe | isc_parity.js>
//   node tests/parity/golden.mjs --freeze    # regenerate the golden files from web/prototype/index.html
//
// Freeze only when the prototype's generator changed on purpose; commit the
// new files together with that change. Freeze with Node 24 (V8 13.6, the CI
// pin): its Math.pow is the host's std::pow, older V8 used fdlibm's.
import fs from 'fs';
import os from 'os';
import path from 'path';
import { execFileSync } from 'child_process';
import { fileURLToPath } from 'url';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const ROOT = path.resolve(HERE, '../..');
const GOLD = path.join(ROOT, 'spec/golden');
const CASES = path.join(GOLD, 'cases.txt');
const REF = path.join(GOLD, 'ref_js.txt');
const MATH_SAMPLES = 2000;

const node = (args, opts = {}) => execFileSync(process.execPath, args, { stdio: 'inherit', ...opts });

const arg = process.argv[2];
if (arg === '--freeze') {
  fs.mkdirSync(GOLD, { recursive: true });
  node([path.join(HERE, 'make_cases.mjs'), CASES, String(MATH_SAMPLES)]);
  node([path.join(HERE, 'js_reference.mjs'), path.join(ROOT, 'web/prototype/index.html'), CASES, REF]);
  // The JS timings are machine noise; zero them so re-freezing only diffs on real changes.
  fs.writeFileSync(REF, fs.readFileSync(REF, 'utf8').replace(/ ms [0-9.]+$/gm, ' ms 0'));
  process.exit(0);
}
if (!arg) {
  console.error('usage: golden.mjs <isc_parity exe | isc_parity.js> | --freeze');
  process.exit(2);
}

const out = path.join(fs.mkdtempSync(path.join(os.tmpdir(), 'isc-golden-')), 'out.txt');
if (arg.endsWith('.js')) node([arg, CASES, out]);
else execFileSync(arg, [CASES, out], { stdio: 'inherit' });
try {
  node([path.join(HERE, 'compare.mjs'), REF, `${path.basename(arg)}=${out}`, '--tol', '0', '--geomtol', '1e-6'],
    { stdio: ['ignore', 'pipe', 'inherit'] });
  console.log(`golden vectors: PASS (${path.basename(arg)} matches the JS reference; see the table for equal/near)`);
} catch (e) {
  console.log(e.stdout?.toString() ?? '');
  console.error('golden vectors: FAIL');
  process.exit(1);
}
