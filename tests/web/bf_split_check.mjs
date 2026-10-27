#!/usr/bin/env node
// The viewer's BrightField movie split across its workers (spec/PORT.md 15), on the viewer's own worker code
// (iscEngine from web/index.html, on the committed web/insiliscope_module.js) under Node: the movie worker's bf-begin,
// two helpers' bf-sources (every other condenser source each, as onBfBegin hands them out), bf-movie with their images
// = the single-worker movie job, frame for frame; a repeat at the same focus (the image cached: no helpers) = the same.
// The engine's own split (the scene from the phase screens, the image from the sources' images) is ctest brightfield.
//   node tests/web/bf_split_check.mjs
import fs from 'fs';
import path from 'path';
import { fileURLToPath } from 'url';
import { loadWasmScope } from '../parity/wasm_scope.mjs';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const read = f => fs.readFileSync(path.join(ROOT, f), 'utf8').replace(/\r\n/g, '\n');
const t0 = performance.now();
const viewer = read('web/index.html');
const moduleText = read('web/insiliscope_module.js');
const s = viewer.indexOf('\nfunction iscEngine(M) {'), e = viewer.indexOf('\n}\n', s);
if (s < 0 || e < 0) throw new Error('iscEngine not found in web/index.html');
// One module instance per engine, as one per worker (the movie cache lives in the module).
const engine = async () => new Function(viewer.slice(s, e + 3) + '\nreturn iscEngine;')()((await loadWasmScope(moduleText)).M);
const [movieWorker, single, ...helpers] = [await engine(), await engine(), await engine(), await engine()];
const run = (eng, job) => eng.handle(structuredClone(job))[0];

let fails = 0;
const report = (ok, msg) => { console.log(`${ok ? 'ok  ' : 'FAIL'} ${msg}`); if (!ok) fails++; };
const firstDiff = (a, b) => {
  if (!a || !b) return 'no frames';
  if (a.length !== b.length) return `${a.length} vs ${b.length} pixels`;
  for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return `pixel ${i}: ${a[i]} vs ${b[i]}`;
  return '';
};

// Quality 3: 12 sources over 2 multislice screens (below quality 2 the viewer keeps the movie on one worker).
const spec = 'modality=BrightField bf-quality=3 size=24 frames=2 world-seed=1249 x=63 y=3';
const want = run(single, { type: 'movie', id: 1, spec, rect: [0, 0, 1, 1] });
report(!want.error && want.frames?.length === 24 * 24 * 2, `single-worker movie: ${want.error || want.frames.length + ' pixels'}`);

const b = run(movieWorker, { type: 'bf-begin', id: 2, spec });
report(!b.error && b.sources > helpers.length && !b.cached && b.phase?.length === b.slices * b.nx * b.ny,
  `bf-begin: ${b.error || `${b.sources} sources, ${b.slices} slices of ${b.nx} x ${b.ny}, phase screens handed out`}`);
const P = b.w * b.hgt, images = new Float32Array(P * b.sources);
helpers.forEach((h, j) => {
  const mine = [];
  for (let src = j; src < b.sources; src += helpers.length) mine.push(src);
  const r = run(h, { type: 'bf-sources', id: 2, spec, slices: b.slices, zTop: b.zTop, objectZ: b.objectZ, w: b.w, hgt: b.hgt,
    phase: b.phase, atten: b.atten, sources: mine });
  report(!r.error && r.images?.length === P * mine.length, `helper ${j}: ${r.error || `sources ${mine.join(' ')}`}`);
  if (!r.error) mine.forEach((src, k) => images.set(r.images.subarray(k * P, (k + 1) * P), src * P));
});
const split = run(movieWorker, { type: 'bf-movie', id: 2, h: b.h, spec, rect: [0, 0, 1, 1], images, helpers: helpers.length });
const d = split.error || firstDiff(split.frames, want.frames);
report(!d && split.split === helpers.length, `the movie from ${helpers.length} helpers' source images = the single-worker movie ${d}`);

// A repeat at the same focus: the movie worker holds the image, bf-movie without images.
const r = run(movieWorker, { type: 'bf-begin', id: 3, spec });
const again = r.error ? r : run(movieWorker, { type: 'bf-movie', id: 3, h: r.h, spec, rect: [0, 0, 1, 1], images: null, helpers: 0 });
const d2 = again.error || firstDiff(again.frames, want.frames);
report(r.cached && !d2, `a repeat: the image cached (${r.cached}), no helpers, the same movie ${d2}`);
console.log(fails ? `bf split: ${fails} FAILED` : `bf split: PASS (${((performance.now() - t0) / 1000).toFixed(1)} s)`);
process.exit(fails ? 1 : 0);
