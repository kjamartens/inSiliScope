#!/usr/bin/env node
// SPDX-License-Identifier: BSD-3-Clause
// The viewer module's heap under Node (2026-10-31, the RAM cuts of the SIM path): each case in a process of its own
// (a WASM heap never shrinks: its size is the case's high-water mark).
//  - bounds: two WideField labels at a 40 nm pixel, 128 px, the viewer's 3000 nm kernel -- live, the movie job and
//    the WebGPU movie path's CPU images, SIM Off / SIM3D / LatticeSIM3D -- render without an abort under a heap bound
//    (measured 592-979 MB; were 1470-1855 MB and two aborts at the 2 GB cap);
//  - high pointers: with 2.2 GB taken first (every later pointer above 2 GB, -sMAXIMUM_MEMORY=4GB and
//    cli/module_unsigned_pointers.js), a live frame, a movie and the WebGPU path's CPU images = a fresh module's.
//   node tests/web/wasm_heap_check.mjs            (~70 s)
import { readFileSync } from 'node:fs';
import { execFileSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '../..');
const [, , mode, which, high] = process.argv;

if (mode === 'case') {
  const g = globalThis;
  g.self = g;
  (0, eval)(readFileSync(join(ROOT, 'web/insiliscope_module.js'), 'utf8'));
  (0, eval)(g.ISC_MODULE_SRC);
  let aborted = null;
  const M = await g.createInsiliscope({ onAbort: w => { aborted = String(w); } });
  const str = s => { const n = M.lengthBytesUTF8(s) + 1, p = M._malloc(n); M.stringToUTF8(s, p, n); return p; };
  const hash = a => createHash('sha256').update(Buffer.from(a.buffer, a.byteOffset, a.byteLength)).digest('hex').slice(0, 16);
  const err = M._malloc(512);
  let pad = 0;
  if (high === '1') {
    pad = M._malloc(2.2 * 2 ** 30);
    if (!pad) { console.log(JSON.stringify({ error: 'no 2.2 GB block' })); process.exit(0); }
  }
  const [path, type, px, size, nFrames] = which.split(':');
  const spec = `mt-mode=WideField npc-mode=WideField pixel-nm=${px} size=${size} psf-kernel-half-width-nm=3000 ` +
    `sim-type=${type} frames=${nFrames}`;
  const out = { heapMB: 0, hash: '', ptr: 0 };
  try {
    if (path === 'live') {
      const info = M._malloc(8 * 32), s = str(spec.replace(/ frames=\d+/, '')), hs = createHash('sha256');
      for (let f = 0; f < +nFrames; f++) {
        const n = M._isc_live_frame(s, 1, info, err, 512);
        if (n < 0) throw new Error(M.UTF8ToString(err));
        const p = M._isc_live_image();
        out.ptr = Math.max(out.ptr, p);
        hs.update(Buffer.from(M.HEAPU16.slice(p / 2, p / 2 + n).buffer));
      }
      out.hash = hs.digest('hex').slice(0, 16);
    } else {
      const s = str(spec), info = M._malloc(24);
      let h = -1;
      if (path === 'wfcpu') {
        h = M._isc_wf_begin(s, err, 512);
        if (h < 0) throw new Error(M.UTF8ToString(err));
        M._isc_wf_cpu_images(h, 0);
      }
      const need = path === 'wfcpu' ? +size * +size * +nFrames : M._isc_scope_movie(s, 0, 0, info, err, 512);
      if (need < 0) throw new Error(M.UTF8ToString(err));
      const o = M._malloc(need * 2);
      out.ptr = o;
      const got = path === 'wfcpu' ? M._isc_wf_movie(h, o, need, info, err, 512) : M._isc_scope_movie(s, o, need, info, err, 512);
      if (got < 0) throw new Error(M.UTF8ToString(err));
      out.hash = hash(M.HEAPU16.slice(o / 2, o / 2 + need));
      if (h >= 0) M._isc_wf_end(h);
    }
  } catch (e) {
    out.error = String(aborted || e.message || e).slice(0, 120);
  }
  out.heapMB = M.HEAPU8.length / 2 ** 20 - (pad ? 2.2 * 1024 : 0);
  console.log(JSON.stringify(out));
  process.exit(0);
}

let fail = 0;
const t0 = Date.now();
const check = (ok, what) => { if (!ok) fail++; console.log(`${ok ? 'ok  ' : 'FAIL'}  ${what}  [${((Date.now() - t0) / 1000).toFixed(0)} s]`); };
const run = (c, hi = '0') => {
  try { return JSON.parse(execFileSync(process.execPath, [fileURLToPath(import.meta.url), 'case', c, hi], { encoding: 'utf8' }).trim().split('\n').pop()); }
  catch (e) { return { error: 'exit ' + e.status + ' ' + String(e.stdout || '').slice(-120) }; }
};

// Heap bounds (MB): the measured value plus ~15 %.
for (const [c, max] of [['live:SIM3D:40:128:15', 700], ['movie:Off:40:128:3', 800], ['movie:SIM3D:40:128:15', 700],
  ['wfcpu:Off:40:128:3', 1150], ['wfcpu:LatticeSIM3D:40:128:13', 700]]) {
  const r = run(c);
  check(!r.error && r.heapMB <= max, `${c}: heap ${r.heapMB.toFixed(0)} MB (bound ${max})${r.error ? ' ' + r.error : ''}`);
}
// High pointers: the same bits as a fresh module.
for (const c of ['live:SIM2D:100:64:9', 'movie:SIM2D:100:64:9', 'wfcpu:Off:100:64:3']) {
  const a = run(c), b = run(c, '1');
  check(!a.error && !b.error && a.hash === b.hash && b.ptr > 2 ** 31,
    `${c} with every pointer above 2 GB (${b.ptr > 2 ** 31 ? (b.ptr / 2 ** 30).toFixed(2) + ' GB' : 'not above: ' + b.ptr}) = a fresh module's (${a.hash}${b.error ? ', ' + b.error : ''})`);
}
console.log(`wasm heap check: ${fail ? 'FAILED' : 'PASS'} (${((Date.now() - t0) / 1000).toFixed(0)} s)`);
process.exit(fail ? 1 : 0);
