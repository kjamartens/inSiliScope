// Imaging parity: the JS reference (web/prototype/scope/) against the C++ (the committed viewer WASM,
// web/insiliscope_module.js: core world + ScopeMovie). Needs no build.
//   node tests/parity/scope_parity.mjs [--quick] [--only <part of a case name>] [--jobs N] [--module <insiliscope_module.js>]
// The full run spreads the movies over N processes (--jobs, default min(4, cores); each --shard i/N), so it stays
// within a few minutes.
// Checks: blink events of a dSTORM label (count, order, values to 1e-9), the BrightField optical volume
// (bit-identical), then fluorescence movies (issue 16: every label mode, a PALM pre state, several lasers, per-dye and
// mean-field continuous populations, a Gaussian-spectrum slot dye, light-preset=auto, start 0 and 60 s) and
// BrightField movies pixel by pixel, for both specimens (the DNA-origami nanorulers: ABI 12), and the cell field's two
// targets (issue 15, ABI 13: the default field x=63 y=3 lies over a nucleus, so the default movies image the pores'
// DNA-PAINT Cy3B too; pores alone, two colours under the combined auto preset, pores off, the label linkage). Expected after a port: blinks only 100% identical ADU, mean-field movies >= 99.9%
// (the C++ convolves in float32), BrightField >= 99.5% (complex float32 FFTs through the slices: single-electron
// Poisson flips) and its intensity within 1e-4 relative.
// In iteration mode (PORT_PENDING.md) the JS is ahead on purpose and this fails: run it in the port.
import fs from 'fs';
import os from 'os';
import path from 'path';
import { spawn } from 'child_process';
import { fileURLToPath } from 'url';
import { loadPrototype } from './load_prototype.mjs';
import { loadWasmScope } from './wasm_scope.mjs';
import { World } from '../../web/prototype/scope/world.js';
import { renderScopeMovie } from '../../web/prototype/scope/scope_movie.js';
import { makeLabel } from '../../web/prototype/scope/dyes.js';
import { STRUCTURES } from '../../web/prototype/scope/world.js';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const args = process.argv.slice(2);
const quick = args.includes('--quick');
const modPath = args.includes('--module') ? args[args.indexOf('--module') + 1] : path.join(ROOT, 'web/insiliscope_module.js');
const only = args.includes('--only') ? args[args.indexOf('--only') + 1] : null;   // cases whose name contains it
const shard = args.includes('--shard') ? args[args.indexOf('--shard') + 1].split('/').map(Number) : null;
const jobs = args.includes('--jobs') ? Math.max(1, Number(args[args.indexOf('--jobs') + 1])) : Math.min(4, os.cpus().length);
if (!shard && !quick && !only && jobs > 1) {
  // The shards in parallel, their output in shard order.
  const t0 = performance.now();
  const runs = await Promise.all(Array.from({ length: jobs }, (_, i) => new Promise(resolve => {
    const k = spawn(process.execPath, [fileURLToPath(import.meta.url), ...args, '--shard', `${i}/${jobs}`], { stdio: ['ignore', 'pipe', 'inherit'] });
    let out = '';
    k.stdout.on('data', d => { out += d; });
    k.on('close', code => resolve({ out, code }));
  })));
  let bad = 0;
  for (const r of runs) {
    process.stdout.write(r.out.split('\n').filter(l => l && !l.startsWith('scope parity:')).map(l => l + '\n').join(''));
    if (r.code) bad++;
  }
  console.log(bad ? `scope parity: ${bad} shard(s) failed` : `scope parity: PASS (${jobs} processes, ${((performance.now() - t0) / 1000).toFixed(0)} s)`);
  process.exit(bad ? 1 : 0);
}
const head = !shard || shard[0] === 0;   // the events, optical volume and BrightField intensity checks
const read = f => fs.readFileSync(path.join(ROOT, f), 'utf8');
const P = loadPrototype(read('web/prototype/index.html'), read('web/prototype/microtubules.js'));
const C = await loadWasmScope(fs.readFileSync(modPath, 'utf8'));

let fails = 0;
const report = (ok, msg) => { console.log(`${ok ? 'ok  ' : 'FAIL'} ${msg}`); if (!ok) fails++; };

// ---- events ----
if (head) {
  // dSTORM, 7% of the sites, with an initial ON phase (the full mode table is tests/parity/label_parity.mjs).
  const seed = 1249;
  const kin = { activationRatePerSec: 0.05, onSec: 0.05, offSec: 1, bleachProb: 0.5, photonCV: 0.5, initialOnSec: 2 };
  const label = makeLabel({ mode: 'dSTORM', density: 0.07, kinetics: kin });
  const W = new World(P, seed, P.paramsFrom(P.defaults), STRUCTURES.map((_, s) => s ? makeLabel() : label));
  const rect = quick ? [61, 1, 64, 4] : [60, 0, 66, 6];
  const js = W.eventsInWindow(...rect, 0, 4, 0, 30);
  const o = label.orientation, k = label.kinetics;   // isc_world_set_label's layout (ISC_LABEL_*)
  const vec = [label.density, label.fluorescentFraction, 0, k.activationRatePerSec, k.onSec, k.offSec, k.bleachProb,
    k.photonCV, k.initialOnSec, 0, ['Free', 'Fixed', 'Random'].indexOf(o.mode), o.polarDeg, o.azimuthDeg, o.wobbleDeg, 0, 0];
  const cc = C.events(seed, {}, vec, rect, 0, 4, 0, 30);
  const n = cc.length / 10;
  let maxd = 0, idBad = 0;
  for (let i = 0; i < Math.min(n, js.length); i++) {
    const a = js[i], b = cc.subarray(i * 10, i * 10 + 10);
    maxd = Math.max(maxd, Math.abs(a.x - b[0]), Math.abs(a.y - b[1]), Math.abs(a.z - b[2]), Math.abs(a.tOn - b[3]),
      Math.abs(a.tOff - b[4]), Math.abs(a.brightness - b[5]), Math.abs(a.structure - b[7]), Math.abs(a.state - b[8]),
      Math.abs(a.aux - b[9]));
    if ((a.id >>> 0) !== (b[6] >>> 0)) idBad++;
  }
  report(js.length === n && idBad === 0 && maxd < 1e-9,
    `events: ${js.length} JS / ${n} C++, ids in order ${idBad ? idBad + ' differ' : 'equal'}, max |d| ${maxd.toExponential(2)}`);
}

// ---- optical volume (BrightField) ----
if (head) {
  const seed = 1249, W = new World(P, seed, P.paramsFrom(P.defaults));
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
const B = 'world-seed=1249 x=63 y=3';
const N = 'specimen=Nanorulers world-seed=1249 x=3 y=3 origami-density=2';
const CASES = [
  ['DNA-PAINT ATTO 655 + imager background, Gaussian PSF', `${B} size=48 frames=12 psf-model=0`, 1],
  ['DNA-PAINT, GibsonLanniZernike, Cubic', `${B} size=48 frames=12 psf-kernel-half-width-nm=2500`, 1],
  ['DNA-PAINT, drift xy 30, z 40 nm/sqrt(s)', `${B} size=48 frames=40 psf-kernel-half-width-nm=2500 drift-xy-nm-per-sqrt-sec=30 drift-z-nm-per-sqrt-sec=40`, 0.999],
  ['DNA-PAINT, directed drift, wandering', `${B} size=48 frames=40 psf-kernel-half-width-nm=2500 drift-xy-speed-nm-per-sec=400 drift-z-speed-nm-per-sec=150 drift-z-direction=-1 drift-z-angle-wander-deg=120 drift-xy-angle-wander-deg=30 drift-speed-wander-pct=40 drift-wander-time-sec=0.5 drift-xy-nm-per-sqrt-sec=10`, 0.999],
  // The blink render regimes: binned FFT (approximate SMLM) and blink mean-field, forced by a zero threshold.
  ['DNA-PAINT, blinks binned (FFT, upscale 2)', `${B} size=40 frames=6 psf-kernel-half-width-nm=2500 blink-binned-max-emitters=0`, 0.999],
  ['dSTORM AF647, blinks binned (upscale 3), Gaussian PSF', `${B} size=40 frames=8 psf-model=0 mt-dye=AF647 light-preset=auto blink-binned-max-emitters=0 blink-binned-upscale=3`, 0.999],
  ['DNA-PAINT, blinks mean-field', `${B} size=48 frames=12 psf-kernel-half-width-nm=2500 blink-mean-field-max-emitters=0`, 0.999],
  ['dSTORM AF647 from t = 0, blinks mean-field', `${B} size=40 frames=8 psf-model=0 mt-dye=AF647 light-preset=auto start-sec=0 blink-mean-field-max-emitters=0`, 0.999],
  ['DNA-PAINT, blinks mean-field, drift xy 30, z 40 nm/sqrt(s)', `${B} size=40 frames=12 psf-kernel-half-width-nm=2500 blink-mean-field-max-emitters=0 drift-xy-nm-per-sqrt-sec=30 drift-z-nm-per-sqrt-sec=40`, 0.999],
  ['dSTORM AF647 from t = 0 (initial ON, mean field), light-preset=auto', `${B} size=40 frames=8 psf-model=0 mt-dye=AF647 light-preset=auto start-sec=0`, 0.999],
  ['dSTORM AF647 at 60 s, double helix, Linear', 'world-seed=1249 x=58 y=-2 size=32 frames=10 psf-kernel-half-width-nm=2000 psf-mask=DoubleHelix psf-interp=Linear zern.5=0.2 mt-dye=AF647 light-preset=auto', 1],
  ['PALM mEos3.2, pre state, 405 + 488 + 561 nm', `${B} size=40 frames=8 psf-model=0 mt-dye=mEos3.2 light-preset=auto laser-405=0.05 laser-488=0.5`, 0.999],
  ['PALM mEos3.2 primed (488 + 730 nm)', `${B} size=40 frames=8 psf-kernel-half-width-nm=2500 mt-dye=mEos3.2 light-preset=PALM-primed`, 0.999],
  ['WideField mEGFP, mean field', `${B} size=48 frames=3 psf-kernel-half-width-nm=2500 mt-dye=mEGFP light-preset=auto`, 0.999],
  ['WideField mEGFP, per dye, from t = 0', `${B} size=32 frames=4 psf-model=0 mt-dye=mEGFP light-preset=auto mt-label-pct=3 mean-field-density-per-um2=1e9 mean-field-max-emitters=1e9 start-sec=0 mt-dye.photon-budget=2000`, 1],
  ['WideField mEGFP, mean field, drift xy 40, z 30 nm/sqrt(s)', `${B} size=40 frames=8 psf-kernel-half-width-nm=2500 mt-dye=mEGFP light-preset=auto drift-xy-nm-per-sqrt-sec=40 drift-z-nm-per-sqrt-sec=30`, 0.999],
  ['WideField mEGFP, per dye, directed drift', `${B} size=32 frames=6 psf-model=0 mt-dye=mEGFP light-preset=auto mt-label-pct=3 mean-field-density-per-um2=1e9 mean-field-max-emitters=1e9 start-sec=0 drift-xy-speed-nm-per-sec=300 drift-z-speed-nm-per-sec=100`, 0.999],
  ['WideField Gaussian, upscale 2, sub-pixel pose', 'world-seed=1249 x=63.04 y=3.07 size=40 frames=3 psf-model=0 wf-upscale=2 mt-dye=mEGFP light-preset=auto', 0.999],
  ['Gaussian-spectrum slot dye (Custom in Dye1)', `${B} size=40 frames=6 psf-model=0 dye1.source=Custom mt-dye=Dye1 light-preset=auto`, 0.999],
  // The label model (specimen registry): the global mode with each target Global and its Typical dye, and Cy3B.
  ['Global mode dSTORM, Typical dye (AF647), typical labelling', `${B} size=40 frames=8 psf-model=0 mode=dSTORM mt-mode=Global mt-dye=Typical light-preset=auto`, 0.999],
  ['DNA-PAINT Cy3B imager, 561 nm', `${B} size=40 frames=8 psf-model=0 mt-dye=Cy3B light-preset=auto`, 1],
  // A laser clean-up filter (vendor curve) in front of the dichroic: each line x its transmission.
  ['dSTORM AF647, 561 + 640 nm through a quad clean-up (ZET405/488/561/640xv2)', `${B} size=40 frames=8 psf-model=0 mt-dye=AF647 light-preset=auto laser-561=1 ex-filter=ChromaZET405-488-561-640xv2`, 0.999],
  // Two targets (issue 15): the pores alone, two colours (the auto preset's multi-band set), pores WideField, pores off,
  // the label linkage (presets and overrides, a Fixed pore dipole), sparse pores.
  ['Pores alone: DNA-PAINT Cy3B, 561 nm, 20 nM imager (mt-dye=None)', `${B} size=40 frames=8 psf-model=0 mt-dye=None npc-imager-nm=20`, 1],
  ['Two colours: microtubules dSTORM AF647 + pores DNA-PAINT Cy3B, auto multi-band', `${B} size=40 frames=8 psf-kernel-half-width-nm=2500 mt-dye=AF647`, 0.999],
  ['Pores WideField mEGFP (mean field) + microtubules DNA-PAINT', `${B} size=40 frames=3 psf-kernel-half-width-nm=2500 npc-mode=WideField npc-dye=mEGFP`, 0.999],
  ['Pores off (npc-density=0)', `${B} size=40 frames=6 psf-model=0 npc-density=0`, 1],
  ['Linkage: microtubules Legacy, binder 5 nm; pores IgG1-PAINT, linker to 8 nm, Fixed', `${B} size=40 frames=6 psf-model=0 mt-binder=Legacy mt-binder-nm=5 npc-binder=IgG1-PAINT npc-linker-max-nm=8 npc-orient=Fixed`, 1],
  ['Sparse pores (4 /um^2, 300 nm apart, radius 60 nm), microtubules off', `${B} size=40 frames=6 psf-model=0 mt-label-pct=0 npc-density=4 p.npcMinDistNm=300 p.npcRadiusNm=60`, 1],
  ['EMCCD, Fft placement, sparse', 'world-seed=1249 x=63 y=3 size=24 frames=3 psf-kernel-half-width-nm=1200 psf-interp=Fft camera-type=EMCCD mt-label-pct=5', 1],
  // The excitation geometry (excitation.js; the defaults: FlatTop 100 %, epi; DNA-PAINT HILO, the nanorulers TIRF):
  ['Excitation Flat epi (uniform: the old paths)', `${B} size=40 frames=6 psf-model=0 illum-profile=Flat illum-bfp=0`, 1],
  ['Excitation Gaussian 60 %, epi, dSTORM AF647 at 60 s', `${B} size=40 frames=8 psf-model=0 mt-dye=AF647 light-preset=auto illum-profile=Gaussian illum-fwhm=60`, 0.999],   // its initial ON: mean-field
  ['Excitation HILO 0.9, dSTORM AF647 from t = 0 (local clock, initial ON)', `${B} size=40 frames=8 psf-model=0 mt-dye=AF647 light-preset=auto start-sec=0 illum-bfp=0.9`, 0.999],
  ['Excitation HILO 0.9, dSTORM blinks mean-field (dose nodes)', `${B} size=40 frames=8 psf-model=0 mt-dye=AF647 light-preset=auto start-sec=0 blink-mean-field-max-emitters=0 illum-bfp=0.9`, 0.999],
  ['Excitation HILO 0.95, PALM mEos3.2 pre state, 405 nm', `${B} size=40 frames=8 psf-model=0 mt-dye=mEos3.2 light-preset=auto laser-405=0.05 illum-bfp=0.95`, 0.999],
  ['Excitation TIRF 1.03, WideField mEGFP mean field (depth planes)', `${B} size=40 frames=3 psf-kernel-half-width-nm=2500 mt-dye=mEGFP light-preset=auto illum-bfp=1.03`, 0.999],
  ['Excitation TIRF 1.03, WideField mEGFP per dye, from t = 0', `${B} size=32 frames=4 psf-model=0 mt-dye=mEGFP light-preset=auto mt-label-pct=3 mean-field-density-per-um2=1e9 mean-field-max-emitters=1e9 start-sec=0 mt-dye.photon-budget=2000 illum-bfp=1.03`, 0.999],
  ['Excitation TIRF 1.03, DNA-PAINT, blinks binned, drift', `${B} size=40 frames=8 psf-kernel-half-width-nm=2500 blink-binned-max-emitters=0 illum-bfp=1.03 drift-xy-nm-per-sqrt-sec=30 drift-z-nm-per-sqrt-sec=40`, 0.999],
  // A focus between the background maps' grid foci (0.25 um apart): the map blended from the two around it.
  ['Excitation HILO 0.95, DNA-PAINT, focus 0.6 um (the blended background map)', `${B} size=40 frames=4 psf-model=0 illum-bfp=0.95 z=0.6`, 0.999],
  ['Excitation epi FlatTop, DNA-PAINT, focus 1.1 um (the blended background map)', `${B} size=40 frames=4 psf-model=0 illum-bfp=0 z=1.1`, 0.999],
  ['Excitation clipped (NA 1.40, bfp 1.1): no light', `${B} size=32 frames=3 psf-model=0 na=1.4 illum-bfp=1.1`, 1],
  ['BrightField thin object (quality 1)', `${B} size=40 frames=3 modality=BrightField bf-quality=1`, 0.995],
  // The lamp pinned at 40000 photons/px/s (the default before 2026-10-05): the share of identical pixels falls with the
  // photon count (the ~1e-5 relative intensity difference flips more Poisson draws), 99.35% at 80000.
  ['BrightField multislice (quality 3), defocused', `${B} size=40 frames=3 modality=BrightField z=2 bf-photons-per-px-per-sec=40000`, 0.995],
  ['BrightField drift xy 40, z 30 nm/sqrt(s) (quality 2)', `${B} size=40 frames=6 modality=BrightField bf-quality=2 bf-photons-per-px-per-sec=40000 drift-xy-nm-per-sqrt-sec=40 drift-z-nm-per-sqrt-sec=30`, 0.995],
  // The shutters: both lights summed before one noise chain (the lamp x its QE), none: the camera's noise alone.
  ['Both lights: DNA-PAINT + BrightField (quality 1)', `${B} size=40 frames=3 psf-model=0 light-epi=1 light-trans=1 bf-quality=1 bf-photons-per-px-per-sec=20000`, 0.995],
  ['Both lights, drift xy 40 nm/sqrt(s) (quality 1)', `${B} size=32 frames=4 psf-model=0 modality=BrightField light-epi=1 bf-quality=1 bf-photons-per-px-per-sec=20000 drift-xy-nm-per-sqrt-sec=40`, 0.995],
  ['No light: dark frames', `${B} size=32 frames=3 light-epi=0`, 1],
  ['BrightField coherent, absorbing, no aberrations', 'world-seed=1249 x=60 y=0 size=32 frames=2 modality=1 bf-quality=2 bf-absorption-per-um=0.05 bf-aberrations=0 bf-condenser-na=0', 0.995],
  // The DNA-origami nanorulers (specimen 1): the docking strands' label, focus at the strands by default.
  ['Nanorulers DNA-PAINT, GibsonLanniZernike', `${N} size=48 frames=20 psf-kernel-half-width-nm=2500 ruler-imager-nm=10`, 1],
  ['Nanorulers dSTORM AF647, Lp 273.1 nm, 3 strands, 3D heights, drift', `${N} size=40 frames=10 psf-model=0 ruler-dye=AF647 light-preset=auto origami-lp-nm=273.1 origami-strands-per-spot=3 origami-heights=1 drift-xy-nm-per-sqrt-sec=20`, 0.999],
  ['Nanorulers WideField mEGFP, mean field', `${N} size=40 frames=3 psf-kernel-half-width-nm=2500 ruler-mode=WideField ruler-dye=mEGFP light-preset=auto`, 0.999],
  ['2D origami TU Delft logo, 20 nm spacing, DNA-PAINT', `${N} size=40 frames=20 psf-model=0 origami-design=TUDelftLogo origami-spacing-nm=20 ruler-imager-nm=10`, 1],
  ['Nanorulers 40 nm spacing, DNA-PAINT', `${N} size=40 frames=20 psf-model=0 origami-spacing-nm=40 ruler-imager-nm=10`, 1],
  ['Nanorulers BrightField (no contrast: noise only)', `${N} size=32 frames=2 modality=BrightField bf-quality=1`, 0.995],
];
const mine = quick ? CASES.slice(0, 2) : only ? CASES.filter(c => c[0].includes(only))
  : shard ? CASES.filter((c, i) => i % shard[1] === shard[0]) : CASES;
for (const [name, spec, need] of mine) {
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
if (head) {
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
