// Imaging parity: the JS reference (web/prototype/scope/) against the C++ (the committed viewer WASM,
// web/insiliscope_module.js: core world + ScopeMovie). Needs no build.
//   node tests/parity/scope_parity.mjs [--quick] [--module <insiliscope_module.js>]
// Checks: blink events (count, order, values to 1e-9), then SR and WideField movies pixel by pixel.
// Expected after a port: SR 100% identical ADU, WideField >= 99.9% (the C++ convolves in float32).
// In iteration mode (PORT_PENDING.md) the JS is ahead on purpose and this fails: run it in the port.
import fs from 'fs';
import path from 'path';
import { fileURLToPath } from 'url';
import { loadPrototype } from './load_prototype.mjs';
import { loadWasmScope } from './wasm_scope.mjs';
import { World } from '../../web/prototype/scope/world.js';
import { renderScopeMovie } from '../../web/prototype/scope/scope_movie.js';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const args = process.argv.slice(2);
const quick = args.includes('--quick');
const modPath = args.includes('--module') ? args[args.indexOf('--module') + 1] : path.join(ROOT, 'web/insiliscope_module.js');
const read = f => fs.readFileSync(path.join(ROOT, f), 'utf8');
const P = loadPrototype(read('web/prototype/index.html'), read('web/prototype/microtubules.js'));
const C = await loadWasmScope(fs.readFileSync(modPath, 'utf8'));

let fails = 0;
const report = (ok, msg) => { console.log(`${ok ? 'ok  ' : 'FAIL'} ${msg}`); if (!ok) fails++; };

// ---- events ----
{
  const seed = 1249, core = { labelEfficiency: 0.02, labelNonBleaching: 0.05 };
  const kin = { activationRatePerSec: 0.05, onSec: 0.05, offSec: 1, bleachProb: 0.5, photonCV: 0.5 };
  const W = new World(P, seed, { ...P.paramsFrom(P.defaults), ...core }, kin);
  const rect = quick ? [61, 1, 64, 4] : [60, 0, 66, 6];
  const js = W.eventsInWindow(...rect, 0, 4, 0, 30);
  const cc = C.events(seed, core, kin, rect, 0, 4, 0, 30);
  const n = cc.length / 7;
  let maxd = 0, idBad = 0;
  for (let i = 0; i < Math.min(n, js.length); i++) {
    const a = js[i], b = cc.subarray(i * 7, i * 7 + 7);
    maxd = Math.max(maxd, Math.abs(a.x - b[0]), Math.abs(a.y - b[1]), Math.abs(a.z - b[2]), Math.abs(a.tOn - b[3]),
      Math.abs(a.tOff - b[4]), Math.abs(a.brightness - b[5]));
    if ((a.id >>> 0) !== (b[6] >>> 0)) idBad++;
  }
  report(js.length === n && idBad === 0 && maxd < 1e-9,
    `events: ${js.length} JS / ${n} C++, ids in order ${idBad ? idBad + ' differ' : 'equal'}, max |d| ${maxd.toExponential(2)}`);
}

// ---- movies ----
const CASES = [
  ['SR Gaussian', 'world-seed=1249 x=63 y=3 size=48 frames=12 psf-model=0', 0.999],
  ['SR GibsonLanniZernike, Cubic', 'world-seed=1249 x=63 y=3 size=48 frames=12 psf-kernel-half-width-nm=2500', 0.999],
  ['SR double helix, Linear, bleaching dyes', 'world-seed=1249 x=58 y=-2 size=32 frames=10 psf-kernel-half-width-nm=2000 psf-mask=DoubleHelix psf-interp=Linear labeling-pct-bleaching=5 milli-activation-rate=20 bleach-prob=0.3 zern.5=0.2', 0.999],
  ['WideField GibsonLanniZernike', 'world-seed=1249 x=63 y=3 size=48 frames=3 modality=WideField psf-kernel-half-width-nm=2500 labeling-pct-bleaching=10', 0.999],
  ['WideField Gaussian, upscale 2, sub-pixel pose', 'world-seed=1249 x=63.04 y=3.07 size=40 frames=3 modality=1 psf-model=0 wf-upscale=2', 0.999],
  ['WideField upscale 3, bleaching from t = 30 s', 'world-seed=1249 x=63 y=3 size=40 frames=3 modality=1 psf-kernel-half-width-nm=2500 wf-upscale=3 start-sec=30 labeling-pct-bleaching=20 wf-photon-budget=500', 0.999],
];
for (const [name, spec, need] of quick ? CASES.slice(0, 2) : CASES) {
  const t0 = performance.now();
  const c = C.movie(spec);
  const t1 = performance.now();
  const n = c.frames.length, px = c.info.width * c.info.height, js = new Uint16Array(n);
  renderScopeMovie(P, spec, (f, adu) => { js.set(adu, f * px); });
  const t2 = performance.now();
  let same = 0, maxd = 0;
  for (let i = 0; i < n; i++) { const d = Math.abs(js[i] - c.frames[i]); if (!d) same++; maxd = Math.max(maxd, d); }
  report(same / n >= need, `${name}: ${(100 * same / n).toFixed(3)}% of ${n} pixels identical, max |d| ${maxd} ADU ` +
    `(C++ ${((t1 - t0) / 1000).toFixed(1)} s, JS ${((t2 - t1) / 1000).toFixed(1)} s)`);
}
console.log(fails ? `scope parity: ${fails} failure(s)` : 'scope parity: PASS');
process.exit(fails ? 1 : 0);
