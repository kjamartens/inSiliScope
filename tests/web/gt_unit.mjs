#!/usr/bin/env node
// SPDX-License-Identifier: BSD-3-Clause
// The ground truth at Save (docs/ground-truth.md) under Node, on the viewer's WASM module (web/insiliscope_module.js):
//   - isc_scope_gt on a fresh Record's file (dSTORM on the nanorulers, with beads): the files packed as the viewer
//     unpacks them (frames.csv, the localization tables, README.txt), a row per frame, beads as kind fiducial;
//   - a Record's file with a light history (recordFrom): the replay, then the recorded runs;
//   - a WideField file: dyes.csv and the density volume (an ImageJ float32 stack: its description's spacing and
//     unit), dyes.csv left out on request (the volume stays); the viewer's way (Module.onGtFile: each file out of the
//     heap as it is made) gives the same files, in order;
//   - with the cli (build/msvc/cli/Release/insiliscope_cli.exe, or ISC_CLI): insiliscope_cli --settings f --gt-out d
//     writes the same files, byte for byte (skipped without a cli).
//   node tests/web/gt_unit.mjs
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { loadWasmScope } from '../parity/wasm_scope.mjs';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const t0 = performance.now();
let fail = 0;
const check = (ok, what) => { if (!ok) fail++; console.log(`${ok ? 'ok  ' : 'FAIL'}  ${what}`); };
const { M } = await loadWasmScope(fs.readFileSync(path.join(ROOT, 'web/insiliscope_module.js'), 'utf8'));
const str = s => { const n = M.lengthBytesUTF8(s) + 1, p = M._malloc(n); M.stringToUTF8(s, p, n); return p; };

// isc_scope_gt's packed files without Module.onGtFile (the viewer sets it: each file out of the heap as it is made):
// name \n size \n bytes, repeated.
function unpack(bytes) {
  const files = new Map();
  let i = 0;
  const line = () => { const j = bytes.indexOf(10, i); const s = new TextDecoder().decode(bytes.subarray(i, j)); i = j + 1; return s; };
  while (i < bytes.length) {
    const name = line(), size = +line();
    files.set(name, bytes.slice(i, i + size));
    i += size;
  }
  return files;
}
function gt(text, { min = [5, 300], vol = [10, 10], dyeCsv = 1 } = {}) {
  const s = str(text);
  const n = M._isc_scope_gt(s, min[0], min[1], vol[0], vol[1], dyeCsv);
  M._free(s);
  if (n < 0) throw new Error(M.UTF8ToString(M._isc_scope_gt_error()));
  const p = M._isc_scope_gt_data(), out = unpack(M.HEAPU8.slice(p, p + n));
  M._isc_scope_gt_free();
  return out;
}
const file = parts => JSON.stringify({ format: 'insiliscope-settings', version: 1, made: { app: 'viewer' }, parts });
const text = b => new TextDecoder().decode(b);
const rows = b => text(b).trim().split('\n');

// ---- a fresh Record: dSTORM nanorulers, beads ----
const fresh = file({
  sample: { values: { specimen: 'Nanorulers', 'ruler-mode': 'dSTORM', 'world-seed': 7, seed: 7, 'fid-density': 0.5 } },
  settings: { values: { 'psf-model': 0, 'blink-min-photons': 0 } },
  measurement: { values: { size: 32, frames: 6, 'exposure-ms': 50, 'start-sec': 5 }, record: { mode: 'fresh', frames: 6 } },
});
let tf = performance.now();
const a = gt(fresh);
const msFresh = performance.now() - tf;
const loc = a.get('localizations_min5.csv'), loc300 = a.get('localizations_min300.csv');
const fr = a.get('frames.csv');
const head = loc ? rows(loc)[0].split(',') : [];
const col = k => head.indexOf(k);
const body = loc ? rows(loc).slice(1).map(r => r.split(',')) : [];
check(fr && rows(fr).length === 7 && loc && loc300 && a.has('README.txt'),
  `a fresh Record: frames.csv (6 rows), localizations_min5/300.csv, README.txt (${[...a.keys()].join(', ')}; ${msFresh.toFixed(0)} ms)`);
check(body.length > 0 && body.every(r => +r[col('intensity [photon]')] >= 5) &&
  rows(loc300).length - 1 === body.filter(r => +r[col('intensity [photon]')] >= 300).length,
  `the tables' thresholds: photons on the camera >= 5 (${body.length} rows), the 300 table its subset`);
check(body.some(r => r[col('kind')] === 'fiducial') && body.some(r => r[col('kind')] === 'emitter'),
  'beads in the localization tables as kind fiducial, beside the blinks');
check(body.filter(r => r[col('kind')] === 'emitter').every(r => r[col('site_x_world_um')] !== ''),
  'every blink row has its binding site');

// ---- a Record's file with a light history ----
const hist = file({
  sample: { values: { specimen: 'Nanorulers', 'ruler-mode': 'dSTORM', 'world-seed': 7, seed: 7 } },
  settings: { values: { 'psf-model': 0 } },
  measurement: {
    values: { size: 32, frames: 5, 'exposure-ms': 50 }, record: { mode: 'live', frames: 5 },
    history: { recordFrom: 1, records: [{ n: 4, lit: true, set: { size: 32, 'psf-model': 0, 'exposure-ms': 50 } },
      { n: 3, lit: true, set: {} }, { n: 2, lit: true, set: { 'laser-640': 3 } }] },
  },
});
const b = gt(hist);
const bf = rows(b.get('frames.csv'));
check(bf.length === 6 && Math.abs(+bf[1].split(',')[1] - 0.2) < 1e-9,
  `a Record after a light history (recordFrom 1): its 5 recorded frames, the first at t = 0.2 s (${bf[1].split(',')[1]})`);

// ---- WideField: the dye list and the volume ----
const wf = file({
  sample: { values: { specimen: 'Nanorulers', 'ruler-mode': 'WideField', 'world-seed': 7, seed: 7 } },
  settings: { values: { 'psf-model': 0 } },
  measurement: { values: { size: 24, frames: 3, 'exposure-ms': 50 }, record: { mode: 'fresh', frames: 3 } },
});
const c = gt(wf, { vol: [100, 50] });
const vol = [...c.keys()].find(k => k.startsWith('density_'));
const desc = vol ? text(c.get(vol).subarray(0, 600)) : '';
const dyes = c.has('dyes.csv') ? rows(c.get('dyes.csv')).slice(1) : [];
check(dyes.length > 0 && vol && /spacing=0\.05\n/.test(desc) && /unit=micron/.test(desc) && /insiliscope\.origin-um=/.test(desc),
  `WideField: dyes.csv (${dyes.length} dyes) and ${vol} (spacing 0.05 um, unit micron, the origin)`);
const c2 = gt(wf, { vol: [100, 50], dyeCsv: 0 });
check(!c2.has('dyes.csv') && c2.has(vol), 'dyes.csv left out on request, the volume kept');
{
  const got = [];
  M.onGtFile = (name, bytes) => got.push([name, bytes]);
  const s = str(wf), n = M._isc_scope_gt(s, 5, 300, 100, 50, 1);
  M._free(s);
  M.onGtFile = null;
  const same = got.length === c.size && got.every(([k, b], i) => k === [...c.keys()][i] && b.length === c.get(k).length && b.every((x, j) => x === c.get(k)[j]));
  check(n === got.reduce((t, f) => t + f[1].length, 0) && same, `Module.onGtFile (the viewer's): the same ${got.length} files, one at a time`);
}

// ---- the cli writes the same files ----
const cli = process.env.ISC_CLI || path.join(ROOT, 'build/msvc/cli/Release/insiliscope_cli.exe');
if (!fs.existsSync(cli)) console.log(`skip  the cli's --gt-out (no ${cli})`);
else {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'isc-gt-'));
  for (const [name, t, files] of [['fresh', fresh, a], ['history', hist, b], ['widefield', wf, null]]) {
    const d = path.join(dir, name);
    fs.mkdirSync(d);
    fs.writeFileSync(path.join(d, 'f.isc.json'), t);
    const vol = name === 'widefield' ? ['--gt-volume', '100,50'] : [];
    execFileSync(cli, ['--settings', path.join(d, 'f.isc.json'), '--out', path.join(d, 'm.tif'), '--gt-out', d, ...vol], { stdio: 'pipe' });
    const want = files || c;
    const same = [...want].every(([n, bytes]) => {
      const got = fs.readFileSync(path.join(d, n));
      return got.length === bytes.length && got.every((v, i) => v === bytes[i]);
    });
    check(same, `${name}: insiliscope_cli --settings --gt-out writes the module's ${want.size} files, byte for byte`);
  }
  fs.rmSync(dir, { recursive: true, force: true });
}

console.log(`gt unit: ${fail ? fail + ' FAILED' : 'all ok'}, ${((performance.now() - t0) / 1000).toFixed(2)} s`);
process.exit(fail ? 1 : 0);
