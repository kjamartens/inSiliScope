#!/usr/bin/env node
// SPDX-License-Identifier: BSD-3-Clause
// The ground truth at Save (docs/ground-truth.md) under Node, on the viewer's WASM module (web/insiliscope_module.js):
//   - isc_scope_gt on a fresh Record's file (dSTORM on the nanorulers, with beads): the files packed as the viewer
//     unpacks them (frames.csv, localizations.csv, README.txt), a row per frame, beads as kind fiducial, the rows
//     at a higher threshold of core photons the lower one's with that many;
//   - a Record's file with a light history (recordFrom): the replay, then the recorded runs;
//   - a WideField file: dyes.csv and the density volume (an ImageJ float32 stack: its description's spacing and
//     unit), dyes.csv left out on request (the volume stays); the viewer's way (Module.onGtFile: each file out of the
//     heap as it is made) gives the same files, in order;
//   - in two steps (isc_scope_gt_run from a lower threshold / with the dye list, then isc_scope_gt_files): the same;
//   - frames the lamp alone draws (and dark ones) get their rows, the frames after numbered on; a BrightField Record;
//   - with the cli (build/msvc/cli/Release/insiliscope_cli.exe, or ISC_CLI): insiliscope_cli --settings f --gt-out d
//     writes the same files, byte for byte (skipped without a cli) -- the module drops the frames (no camera noise,
//     no lamp: GroundTruth::framesUnused), the cli renders them.
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
function gt(text, { min = 300, vol = [10, 10], dyeCsv = 1 } = {}) {
  const s = str(text);
  const n = M._isc_scope_gt(s, min, vol[0], vol[1], dyeCsv);
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
const a = gt(fresh, { min: 5 });
const msFresh = performance.now() - tf;
const a300 = gt(fresh);
const loc = a.get('localizations.csv'), loc300 = a300.get('localizations.csv');
const fr = a.get('frames.csv');
const head = loc ? rows(loc)[0].split(',') : [];
const col = k => head.indexOf(k);
const body = loc ? rows(loc).slice(1).map(r => r.split(',')) : [];
check(fr && rows(fr).length === 7 && loc && loc300 && a.has('README.txt') && ![...a.keys()].some(k => k.startsWith('localizations_min')),
  `a fresh Record: frames.csv (6 rows), localizations.csv, README.txt (${[...a.keys()].join(', ')}; ${msFresh.toFixed(0)} ms)`);
{
  const core = col('core_photons'), hi = rows(loc300);
  const want = [rows(loc)[0], ...rows(loc).slice(1).filter(r => +r.split(',')[core] >= 300)];
  check(core > 0 && body.length > 0 && body.every(r => +r[core] >= 5 && +r[core] <= +r[col('intensity [photon]')] + 1e-3) &&
    hi.length > 1 && hi.length < body.length + 1 && hi.join('\n') === want.join('\n'),
    `the threshold on core photons: >= 5 (${body.length} rows, each no more than its photons on the camera); at 300 the same rows with >= 300 (${hi.length - 1})`);
}
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
const vol = [...c.keys()].find(k => k.startsWith('gt_volume_'));
const desc = vol ? text(c.get(vol).subarray(0, 600)) : '';
const dyes = c.has('dyes.csv') ? rows(c.get('dyes.csv')).slice(1) : [];
check(dyes.length > 0 && vol && /spacing=0\.05\n/.test(desc) && /unit=micron/.test(desc) && /insiliscope\.origin-um=/.test(desc),
  `WideField: dyes.csv (${dyes.length} dyes) and ${vol} (spacing 0.05 um, unit micron, the origin)`);
const c2 = gt(wf, { vol: [100, 50], dyeCsv: 0 });
check(!c2.has('dyes.csv') && c2.has(vol), 'dyes.csv left out on request, the volume kept');
{
  const got = [];
  M.onGtFile = (name, bytes) => got.push([name, bytes]);
  const s = str(wf), n = M._isc_scope_gt(s, 300, 100, 50, 1);
  M._free(s);
  M.onGtFile = null;
  const same = got.length === c.size && got.every(([k, b], i) => k === [...c.keys()][i] && b.length === c.get(k).length && b.every((x, j) => x === c.get(k)[j]));
  check(n === got.reduce((t, f) => t + f[1].length, 0) && same, `Module.onGtFile (the viewer's): the same ${got.length} files, one at a time`);
}

// ---- in two steps (the Save box starts the run while it is open): rows from a lower threshold, the dye list's
// sites found for a files call without it: the same files ----
function gtSplit(text, runMin, runDyes, { min = 300, vol = [10, 10], dyeCsv = 1, keepFirst = false } = {}) {
  const s = str(text), rc = M._isc_scope_gt_run(s, runMin, runDyes);
  M._free(s);
  if (rc < 0) throw new Error(M.UTF8ToString(M._isc_scope_gt_error()));
  if (keepFirst) {   // files for other options first, the run kept (the viewer's files made as the run ends)
    if (M._isc_scope_gt_files(1e9, 50, 50, 1 - dyeCsv, 1) < 0) throw new Error(M.UTF8ToString(M._isc_scope_gt_error()));
    M._isc_scope_gt_free();
  }
  const n = M._isc_scope_gt_files(min, vol[0], vol[1], dyeCsv, 0);
  if (n < 0) throw new Error(M.UTF8ToString(M._isc_scope_gt_error()));
  const p = M._isc_scope_gt_data(), out = unpack(M.HEAPU8.slice(p, p + n));
  M._isc_scope_gt_free();
  return out;
}
const sameFiles = (x, y) => x.size === y.size && [...x].every(([k, v], i) => k === [...y.keys()][i] && v.length === y.get(k).length && v.every((q, j) => q === y.get(k)[j]));
check(sameFiles(gtSplit(fresh, 0, 1, { min: 5 }), a) && sameFiles(gtSplit(fresh, 5, 1), a300) && sameFiles(gtSplit(hist, 2, 1), b),
  'isc_scope_gt_run from a lower threshold (0, 5, 2), then isc_scope_gt_files at 5 or 300: the same files (fresh, history)');
check(sameFiles(gtSplit(wf, 5, 1, { vol: [100, 50], dyeCsv: 0 }), c2) && sameFiles(gtSplit(wf, 5, 1, { vol: [100, 50] }), c),
  'a run with the dye list serves files without it (and with it): the same files');
check(sameFiles(gtSplit(fresh, 0, 1, { min: 5, keepFirst: true }), a) && sameFiles(gtSplit(wf, 5, 1, { vol: [100, 50], keepFirst: true }), c),
  'files for other options first (the run kept), then these: the same files');
{
  const n = M._isc_scope_gt_files(300, 10, 10, 1, 0);
  check(n < 0 && /no ground truth rendered/.test(M.UTF8ToString(M._isc_scope_gt_error())), 'isc_scope_gt_files with no run: an error');
}

// ---- frames with no fluorescence movie (the lamp alone, no light) and the lamp beside the fluorescence: a row each,
// the frames after numbered on (the module drops the frames: no noise, no lamp; the cli renders them) ----
const lamp = file({
  sample: { values: { specimen: 'Nanorulers', 'ruler-mode': 'dSTORM', 'world-seed': 7, seed: 7, 'fid-density': 0.5 } },
  settings: { values: { 'psf-model': 0, 'bf-quality': 1 } },
  measurement: {
    values: { size: 24, frames: 6, 'exposure-ms': 50 }, record: { mode: 'live', frames: 6 },
    history: { recordFrom: 0, records: [{ n: 2, lit: true, set: { size: 24, 'psf-model': 0, 'bf-quality': 1, 'exposure-ms': 50, 'light-epi': 0, 'light-trans': 1 } },
      { n: 2, lit: true, set: { 'light-epi': 1, 'light-trans': 0 } }, { n: 2, lit: true, set: { 'light-trans': 1 } }] },
  },
});
const d = gt(lamp, { min: 0 });
const dRows = rows(d.get('frames.csv')).slice(1).map(r => +r.split(',')[0]);
const dLoc = d.has('localizations.csv') ? rows(d.get('localizations.csv')).slice(1).map(r => +r.split(',')[0]) : [];
check(dRows.join() === '0,1,2,3,4,5' && dLoc.length > 0 && dLoc.every(f => f >= 2),
  `lamp alone, fluorescence, both: frames.csv rows ${dRows.join(',')}, the beads and blinks at frames ${[...new Set(dLoc)].join(',')}`);
const bfOnly = file({
  sample: { values: { specimen: 'Nanorulers', 'world-seed': 7, seed: 7 } },
  settings: { values: { 'bf-quality': 1, modality: 1 } },
  measurement: { values: { size: 24, frames: 3, 'exposure-ms': 50 }, record: { mode: 'fresh', frames: 3 } },
});
const e = gt(bfOnly);
check(rows(e.get('frames.csv')).length === 4, `a BrightField Record: frames.csv, a row per frame (${rows(e.get('frames.csv')).length - 1})`);

// ---- the cli writes the same files ----
const cli = process.env.ISC_CLI || path.join(ROOT, 'build/msvc/cli/Release/insiliscope_cli.exe');
if (!fs.existsSync(cli)) console.log(`skip  the cli's --gt-out (no ${cli})`);
else {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'isc-gt-'));
  for (const [name, t, files] of [['fresh', fresh, a], ['history', hist, b], ['widefield', wf, null], ['lamp', lamp, d], ['brightfield', bfOnly, e]]) {
    const d = path.join(dir, name);
    fs.mkdirSync(d);
    fs.writeFileSync(path.join(d, 'f.isc.json'), t);
    const vol = name === 'widefield' ? ['--gt-volume', '100,50'] : name === 'lamp' ? ['--gt-min', '0'] : name === 'fresh' ? ['--gt-min', '5'] : [];
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
