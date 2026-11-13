// Live frame speed of the viewer's WASM module under Node (2026-11-11): isc_live_frame one frame at a time on one
// module (the live worker's), timed; not a check.
//
//   node tests/web/live_speed.mjs [--module web/insiliscope_module.js | --mt build/wasm/cli/insiliscope_scope_mt.js]
//                                 [--frames 60] [--profile] [--hash] 'spec k=v ...'
// --xstep um [--x0 um] [--xfrom f] [--xevery k]: the stage (x) still for f frames, then moves by um every k frames;
// --dump file: the frames' uint16 pixels; --ystep um [--y0 um]: y too; --steps n: at most n steps, then the pose holds; --zstep um [--z0 um]: the focus steps too (with --xstep 0: only the focus), the same schedule;
// --slow ms: the engine's top phases of every frame slower than ms.
// --mt: the threaded module's build output (Emscripten pthreads on Node's worker_threads; loaded from its file).
//
// Prints ms per frame (each, and means per tenth of the run), with --profile the engine's phases of the last frame
// (isc_live_profile_json). --hash prints an FNV-1a of each frame's pixels (to compare two modules).
import { readFileSync, appendFileSync, writeFileSync } from 'node:fs';
import { createRequire } from 'node:module';
import path from 'node:path';
import { loadWasmScope } from '../parity/wasm_scope.mjs';

const args = process.argv.slice(2);
let modulePath = new URL('../../web/insiliscope_module.js', import.meta.url);
let frames = 60, profile = false, hash = false, mt = null, xstep = 0, x0 = 0, xfrom = 0, xevery = 1, slowMs = 0, paceMs = 0, prefetchUm = 0, zstep = 0, z0 = 0.5, maxSteps = Infinity, ystep = 0, y0 = 0, dump = null;
const spec = [];
for (let i = 0; i < args.length; i++) {
  if (args[i] === '--module') modulePath = args[++i];
  else if (args[i] === '--frames') frames = +args[++i];
  else if (args[i] === '--profile') profile = true;
  else if (args[i] === '--hash') hash = true;
  else if (args[i] === '--mt') mt = args[++i];
  else if (args[i] === '--xstep') xstep = +args[++i];
  else if (args[i] === '--x0') x0 = +args[++i];
  else if (args[i] === '--xfrom') xfrom = +args[++i];
  else if (args[i] === '--xevery') xevery = Math.max(1, +args[++i]);
  else if (args[i] === '--slow') slowMs = +args[++i];
  else if (args[i] === '--pace') paceMs = +args[++i];
  else if (args[i] === '--zstep') zstep = +args[++i];
  else if (args[i] === '--z0') z0 = +args[++i];
  else if (args[i] === '--steps') maxSteps = +args[++i];
  else if (args[i] === '--ystep') ystep = +args[++i];
  else if (args[i] === '--dump') { dump = args[++i]; writeFileSync(dump, new Uint8Array(0)); }
  else if (args[i] === '--y0') y0 = +args[++i];
  else if (args[i] === '--prefetch') prefetchUm = +args[++i];
  else spec.push(args[i]);
}
const M = mt ? await createRequire(import.meta.url)(path.resolve(mt))()
  : (await loadWasmScope(readFileSync(modulePath, 'utf8'))).M;
const str = s => { const n = M.lengthBytesUTF8(s) + 1, p = M._malloc(n); M.stringToUTF8(s, p, n); return p; };
let sp = str(spec.join(' '));
const info = M._malloc(32 * 8), err = M._malloc(1024);
if ((profile || slowMs) && M._isc_live_profile) M._isc_live_profile(1);
const ms = [];
for (let f = 0; f < frames; f++) {
  if (xstep || ystep || zstep) {   // the stage: still for xfrom frames, then x (and/or the focus z) moves by a step every xevery frames
    const k = Math.min(maxSteps, f < xfrom ? 0 : 1 + Math.floor((f - xfrom) / xevery));
    M._free(sp); sp = str(spec.join(' ') + (xstep ? ' x=' + (x0 + xstep * k) : '') + (ystep ? ' y=' + (y0 + ystep * k) : '') + (zstep ? ' z=' + (z0 + zstep * k).toFixed(4) : ''));
  }
  const t0 = performance.now();
  const n = M._isc_live_frame(sp, 1, info, err, 1024);
  const dt = performance.now() - t0;
  if (n < 0) { console.error('frame failed:', M.UTF8ToString(err)); process.exit(1); }
  ms.push(dt);
  let pf = 0;
  if (prefetchUm > 0 && paceMs > dt + 2) {   // a paced live loop's spare time spent warming around the FOV
    const tp = performance.now();
    M._isc_live_prefetch(sp, prefetchUm, paceMs - dt - 2);
    pf = performance.now() - tp;
  }
  let line = `frame ${String(f).padStart(3)}  ${dt.toFixed(1).padStart(8)} ms  (prefetch ${pf.toFixed(1)} ms)`;
  if (dump) {   // every frame's uint16 pixels, one after another (to compare two modules)
    const p = M._isc_live_image() >>> 0;
    appendFileSync(dump, new Uint8Array(M.HEAPU16.buffer.slice(p, p + 2 * n)));
  }
  if (hash) {
    const p = M._isc_live_image() >>> 0, px = new Uint16Array(M.HEAPU16.buffer, p, n);
    let h = 0x811c9dc5;
    for (let i = 0; i < px.length; i++) h = Math.imul(h ^ px[i], 0x01000193) >>> 0;
    line += '  ' + h.toString(16).padStart(8, '0');
  }
  console.log(line);
  if (slowMs && dt > slowMs && M._isc_live_profile_json) {   // the phases of a slow frame
    const j = JSON.parse(M.UTF8ToString(M._isc_live_profile_json()));
    const rows = Object.entries(j.phases).filter(([, v]) => v.totalMs > 2).sort((a, b) => b[1].totalMs - a[1].totalMs);
    console.log('     ' + rows.slice(0, 6).map(([k, v]) => `${k} ${v.totalMs.toFixed(0)}`).join(', '));
  }
}
const tenth = Math.max(1, Math.floor(frames / 10)), parts = [];
for (let i = 0; i < frames; i += tenth) {
  const s = ms.slice(i, i + tenth);
  parts.push((s.reduce((a, b) => a + b, 0) / s.length).toFixed(0));
}
const rest = ms.slice(1);
console.log(`mean frames 1..: ${(rest.reduce((a, b) => a + b, 0) / Math.max(1, rest.length)).toFixed(1)} ms; per tenth: ${parts.join(' ')}`);
if (profile && M._isc_live_profile_json) {
  const j = JSON.parse(M.UTF8ToString(M._isc_live_profile_json()));
  const rows = Object.entries(j.phases).filter(([, v]) => v.totalMs > 1).sort((a, b) => b[1].totalMs - a[1].totalMs);
  for (const [k, v] of rows) console.log(`  ${k.padEnd(34)} ${v.totalMs.toFixed(1).padStart(8)} ms  x${v.count}`);
}
process.exit(0);   // (the threaded module's workers keep Node alive)
