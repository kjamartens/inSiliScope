// Imaging parity: the JS reference (web/prototype/scope/) against the C++ (the committed viewer WASM,
// web/insiliscope_module.js: core world + ScopeMovie). Needs no build.
//   node tests/parity/scope_parity.mjs [--quick] [--module <insiliscope_module.js>]
// Checks: blink events (count, order, values to 1e-9), the BrightField optical volume (bit-identical), then SR,
// WideField and BrightField movies pixel by pixel. Expected after a port: SR 100% identical ADU, WideField >= 99.9%
// (the C++ convolves in float32), BrightField >= 99.5% (complex float32 FFTs through the slices: single-electron
// Poisson flips) and its intensity within 1e-4 relative.
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

// ---- optical volume (BrightField) ----
{
  const seed = 1249, W = new World(P, seed, { ...P.paramsFrom(P.defaults), labelEfficiency: 0, labelNonBleaching: 0.7 });
  const [rect, nz, sub, n] = quick ? [[60, 0, 64, 4], 6, 1, 40] : [[58, -2, 68, 8], 6, 2, 50];
  const js = new Float32Array(3 * n * n * nz);
  const jc = W.opticalVolume(...rect, 0, 6, n, n, nz, sub, js);
  const cc = C.opticalVolume(seed, {}, rect, 0, 6, n, n, nz, sub);
  let same = 0, maxd = 0;
  for (let i = 0; i < js.length; i++) { const d = Math.abs(js[i] - cc[i]); if (!d) same++; maxd = Math.max(maxd, d); }
  report(jc === cc.cells && same === js.length,
    `optical volume: ${jc} / ${cc.cells} cells, ${(100 * same / js.length).toFixed(3)}% of ${js.length} voxels identical, max |d| ${maxd.toExponential(2)}`);
}

// ---- movies ----
const CASES = [
  ['SR Gaussian', 'world-seed=1249 x=63 y=3 size=48 frames=12 psf-model=0', 0.999],
  ['SR GibsonLanniZernike, Cubic', 'world-seed=1249 x=63 y=3 size=48 frames=12 psf-kernel-half-width-nm=2500', 0.999],
  ['SR double helix, Linear, bleaching dyes', 'world-seed=1249 x=58 y=-2 size=32 frames=10 psf-kernel-half-width-nm=2000 psf-mask=DoubleHelix psf-interp=Linear labeling-pct-bleaching=5 milli-activation-rate=20 bleach-prob=0.3 zern.5=0.2', 0.999],
  ['WideField GibsonLanniZernike', 'world-seed=1249 x=63 y=3 size=48 frames=3 modality=WideField psf-kernel-half-width-nm=2500 labeling-pct-bleaching=10', 0.999],
  ['WideField Gaussian, upscale 2, sub-pixel pose', 'world-seed=1249 x=63.04 y=3.07 size=40 frames=3 modality=1 psf-model=0 wf-upscale=2', 0.999],
  ['WideField upscale 3, bleaching from t = 30 s', 'world-seed=1249 x=63 y=3 size=40 frames=3 modality=1 psf-kernel-half-width-nm=2500 wf-upscale=3 start-sec=30 labeling-pct-bleaching=20 wf-photon-budget=500', 0.999],
  ['BrightField thin object (quality 1)', 'world-seed=1249 x=63 y=3 size=40 frames=3 modality=BrightField bf-quality=1', 0.995],
  // The lamp pinned at 40000 photons/px/s (the default before 2026-10-05): the share of identical pixels falls with the
  // photon count (the ~1e-5 relative intensity difference flips more Poisson draws), 99.35% at 80000.
  ['BrightField multislice (quality 3), defocused', 'world-seed=1249 x=63 y=3 size=40 frames=3 modality=BrightField z=2 bf-photons-per-px-per-sec=40000', 0.995],
  ['BrightField coherent, absorbing, no aberrations', 'world-seed=1249 x=60 y=0 size=32 frames=2 modality=2 bf-quality=2 bf-absorption-per-um=0.05 bf-aberrations=0 bf-condenser-na=0', 0.995],
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
// ---- BrightField intensity: huge flux, coarse gain, no other noise, so ADU ~ transmitted intensity ----
{
  const spec = 'world-seed=1249 x=63 y=3 size=40 frames=1 modality=BrightField bf-quality=2 qe=1 dark-per-sec=0 offset=0 ' +
    'offset-std=0 read-noise=0 gain-std-pct=0 read-noise-std-pct=0 gain=20000 bf-photons-per-px-per-sec=2e10';
  const c = C.movie(spec);
  let js;
  renderScopeMovie(P, spec, (f, adu) => { js = adu; });
  let maxd = 0, mean = 0;
  for (let i = 0; i < js.length; i++) { maxd = Math.max(maxd, Math.abs(js[i] - c.frames[i])); mean += c.frames[i] / js.length; }
  report(maxd / mean < 1e-4, `BrightField intensity (quality 2): max |d| ${maxd} ADU of ${mean.toFixed(0)} (${(maxd / mean).toExponential(1)} relative)`);
}
console.log(fails ? `scope parity: ${fails} failure(s)` : 'scope parity: PASS');
process.exit(fails ? 1 : 0);
