// Live frame speed of the viewer's WASM module under Node (2026-11-11): isc_live_frame one frame at a time on one
// module (the live worker's), timed; not a check.
//
//   node tests/web/live_speed.mjs [--module web/insiliscope_module.js | --mt build/wasm/cli/insiliscope_scope_mt.js]
//                                 [--frames 60] [--profile] [--hash] 'spec k=v ...'
// --mt: the threaded module's build output (Emscripten pthreads on Node's worker_threads; loaded from its file).
//
// Prints ms per frame (each, and means per tenth of the run), with --profile the engine's phases of the last frame
// (isc_live_profile_json). --hash prints an FNV-1a of each frame's pixels (to compare two modules).
import { readFileSync } from 'node:fs';
import { createRequire } from 'node:module';
import path from 'node:path';
import { loadWasmScope } from '../parity/wasm_scope.mjs';

const args = process.argv.slice(2);
let modulePath = new URL('../../web/insiliscope_module.js', import.meta.url);
let frames = 60, profile = false, hash = false, mt = null;
const spec = [];
for (let i = 0; i < args.length; i++) {
  if (args[i] === '--module') modulePath = args[++i];
  else if (args[i] === '--frames') frames = +args[++i];
  else if (args[i] === '--profile') profile = true;
  else if (args[i] === '--hash') hash = true;
  else if (args[i] === '--mt') mt = args[++i];
  else spec.push(args[i]);
}
const M = mt ? await createRequire(import.meta.url)(path.resolve(mt))()
  : (await loadWasmScope(readFileSync(modulePath, 'utf8'))).M;
const str = s => { const n = M.lengthBytesUTF8(s) + 1, p = M._malloc(n); M.stringToUTF8(s, p, n); return p; };
const sp = str(spec.join(' ')), info = M._malloc(32 * 8), err = M._malloc(1024);
if (profile && M._isc_live_profile) M._isc_live_profile(1);
const ms = [];
for (let f = 0; f < frames; f++) {
  const t0 = performance.now();
  const n = M._isc_live_frame(sp, 1, info, err, 1024);
  const dt = performance.now() - t0;
  if (n < 0) { console.error('frame failed:', M.UTF8ToString(err)); process.exit(1); }
  ms.push(dt);
  let line = `frame ${String(f).padStart(3)}  ${dt.toFixed(1).padStart(8)} ms`;
  if (hash) {
    const p = M._isc_live_image() >>> 0, px = new Uint16Array(M.HEAPU16.buffer, p, n);
    let h = 0x811c9dc5;
    for (let i = 0; i < px.length; i++) h = Math.imul(h ^ px[i], 0x01000193) >>> 0;
    line += '  ' + h.toString(16).padStart(8, '0');
  }
  console.log(line);
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
