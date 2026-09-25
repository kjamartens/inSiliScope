// Runs the full parity check and writes build/parity/report.md.
//   node tests/parity/run.mjs [name=<native exe | wasm .js> ...]
// Defaults to the msvc and wasm preset outputs that exist. The JS reference
// is the prototype at $ISC_PROTOTYPE (default: web/index.html, falling back
// to the original C:/GitHub/websmlm/cell_field_sim/index.html).
import fs from 'fs';
import path from 'path';
import zlib from 'zlib';
import { execFileSync } from 'child_process';
import { fileURLToPath } from 'url';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const ROOT = path.resolve(HERE, '../..');
const OUT = path.join(ROOT, 'build/parity');
fs.mkdirSync(OUT, { recursive: true });

const proto = process.env.ISC_PROTOTYPE ||
  [path.join(ROOT, 'web/index.html'), 'C:/GitHub/websmlm/cell_field_sim/index.html'].find(p => fs.existsSync(p));
if (!proto) throw new Error('JS prototype not found; set ISC_PROTOTYPE');

let targets = process.argv.slice(2).map(s => s.split('='));
if (!targets.length) {
  targets = [
    ['native', path.join(ROOT, 'build/msvc/tests/parity/Release/isc_parity.exe')],
    ['native', path.join(ROOT, 'build/native/tests/parity/isc_parity')],
    ['wasm', path.join(ROOT, 'build/wasm/tests/parity/isc_parity.js')],
  ].filter(([, p]) => fs.existsSync(p));
}

const run = (cmd, args) => execFileSync(cmd, args, { stdio: ['ignore', 'inherit', 'inherit'] });
const cases = path.join(OUT, 'cases.txt');
const ref = path.join(OUT, 'ref_js.txt');
run(process.execPath, [path.join(HERE, 'make_cases.mjs'), cases]);
run(process.execPath, [path.join(HERE, 'js_reference.mjs'), proto, cases, ref]);
const cmpArgs = [];
for (const [name, exe] of targets) {
  const out = path.join(OUT, `out_${name}.txt`);
  const t0 = Date.now();
  if (exe.endsWith('.js')) run(process.execPath, [exe, cases, out]);
  else run(exe, [cases, out]);
  console.log(`${name}: ${((Date.now() - t0) / 1000).toFixed(1)} s`);
  cmpArgs.push(`${name}=${out}`);
}

let report = `# Parity report\n\nReference: \`${path.relative(ROOT, proto) || proto}\` under Node ${process.version} (V8 ${process.versions.v8}).\n\n`;
let code = 0;
try {
  report += execFileSync(process.execPath, [path.join(HERE, 'compare.mjs'), ref, ...cmpArgs], { encoding: 'utf8' });
} catch (e) { report += e.stdout; code = 1; }

// WASM size trajectory.
const wasm = path.join(ROOT, 'build/wasm/core/insilicell.wasm');
if (fs.existsSync(wasm)) {
  const raw = fs.readFileSync(wasm);
  const glue = fs.readFileSync(wasm.replace(/\.wasm$/, '.js'));
  const gz = zlib.gzipSync(raw, { level: 9 });
  const kb = n => (n / 1024).toFixed(1) + ' KB';
  report += `\n## WASM core module size (C ABI only)\n\n| | raw | gzip -9 | base64(raw) | base64(gzip) |\n|---|---|---|---|---|\n` +
    `| insilicell.wasm | ${kb(raw.length)} | ${kb(gz.length)} | ${kb(Math.ceil(raw.length / 3) * 4)} | ${kb(Math.ceil(gz.length / 3) * 4)} |\n` +
    `| Emscripten JS glue | ${kb(glue.length)} | ${kb(zlib.gzipSync(glue, { level: 9 }).length)} | | |\n`;
}
fs.writeFileSync(path.join(OUT, 'report.md'), report);
console.log(report);
process.exit(code);
