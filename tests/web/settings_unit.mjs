#!/usr/bin/env node
// SPDX-License-Identifier: BSD-3-Clause
// Settings files (web/live/settings_core.js, docs/dev/SAVE_SETTINGS.md) under Node, on the viewer's WASM module
// (web/insiliscope_module.js), in under a second:
//   - every engine option has a part (isc_scope_options), names read back as their numbers, p.* / zern.* / dye fields;
//   - a part's id: the same values the same id, in any key order;
//   - a live session's light history written as the viewer saves it (runs of spec changes, without the sample's keys),
//     through JSON, rebuilt with the sample's keys and replayed (isc_live_advance): the next frame is the session's,
//     bit for bit;
//   - reading a file: another release / world version / newer format warned about, other files refused;
//   - the setup readout's Zernike modes: the preset's, zern.<j> replacing its own.
//   node tests/web/settings_unit.mjs
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { loadWasmScope } from '../parity/wasm_scope.mjs';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const t0 = performance.now();
let fail = 0;
const check = (ok, what) => { if (!ok) fail++; console.log(`${ok ? 'ok  ' : 'FAIL'}  ${what}`); };
const self = globalThis.self || (globalThis.self = globalThis);
new Function('self', fs.readFileSync(path.join(ROOT, 'web/live/settings_core.js'), 'utf8'))(self);
const C = self.iscSettingsCore;
const { M } = await loadWasmScope(fs.readFileSync(path.join(ROOT, 'web/insiliscope_module.js'), 'utf8'));
const str = s => { const n = M.lengthBytesUTF8(s) + 1, p = M._malloc(n); M.stringToUTF8(s, p, n); return p; };

// The engine's options as the viewer reads them (web/index.html, the options job).
const n = M._isc_scope_options(0, 0) + 1, b = M._malloc(n);
M._isc_scope_options(b, n);
const eng = new Map(M.UTF8ToString(b).split('\n').filter(Boolean).map(l => l.split('\t'))
  .map(([name, def, , part, names]) => [name, { def: +def, part, names: names ? names.split('|') : null }]));
M._free(b);

// ---- parts and names ----
const PARTS = ['instrument', 'settings', 'sample', 'measurement', 'host'];
const count = Object.fromEntries(PARTS.map(p => [p, 0]));
let named = true;
for (const [k, o] of eng) {
  count[o.part] = (count[o.part] || 0) + 1;
  if (o.names) o.names.forEach((nm, i) => { named = named && C.toName(eng, k, i) === nm && C.fromName(eng, k, nm) === i; });
}
check(Object.keys(count).length === PARTS.length && eng.size > 150, `every engine option has a part: ${JSON.stringify(count)}`);
check(named, 'every value name reads back as its number, and the number as the name');
check(C.partOf(eng, 'p.cellDiamMin') === 'sample' && C.partOf(eng, 'zern.5') === 'instrument' && C.partOf(eng, 'mt-dye.qy') === 'sample' &&
  C.partOf(eng, 'dye2.on-sec') === 'sample' && C.partOf(eng, 'na') === 'instrument' && C.partOf(eng, 'laser-640') === 'settings' &&
  C.partOf(eng, 'x') === 'measurement' && C.partOf(eng, 'nope') === null, 'p.* sample, zern.* instrument, dye fields sample, na, laser-640, x');
check(C.fromName(eng, 'mt-dye', 'Typical') === -1 && C.fromName(eng, 'mt-mode', 'DyeDefault') === -1 && C.fromName(eng, 'mode', 'None') === -1 &&
  C.fromName(eng, 'na', '1.3') === 1.3 && C.fromName(eng, 'na', true) === 1 && Number.isNaN(C.fromName(eng, 'camera-preset', 'NoSuchCamera')),
  'the specials (Typical, DyeDefault, None), numbers as text, booleans; an unknown name is NaN');
check(C.idOf({ a: 1, b: 'x' }) === C.idOf({ b: 'x', a: 1 }) && C.idOf({ a: 1, b: 'x' }) !== C.idOf({ a: 2, b: 'x' }) &&
  /^[0-9a-v]{6}$/.test(C.idOf({ a: 1 })), 'a part\'s id: the same values the same id in any order, another value another');

// ---- the light history: a live session, saved, replayed ----
// Nanorulers (no cell packing) and the Gaussian PSF (no kernel to compute) keep the frames cheap.
const base = 'specimen=1 psf-model=0 size=16 seed=7 world-seed=7 ruler-mode=2 ';
const runs = [['x=0 y=0 z=0.0135 exposure-ms=100', 3], ['x=0 y=0 z=0.0135 exposure-ms=100 laser-561=3', 2],
  ['x=1.5 y=-1 z=0.0135 exposure-ms=100 laser-561=3', 2], ['x=1.5 y=-1 z=0.0135 exposure-ms=50', 1]];
const info = M._malloc(13 * 8), err = M._malloc(512);
function frame(spec) {
  const s = str(spec), px = M._isc_live_frame(s, 1, info, err, 512);
  M._free(s);
  if (px < 0) throw new Error(M.UTF8ToString(err));
  const p = M._isc_live_image();
  return M.HEAPU16.slice(p / 2, p / 2 + px);
}
M._isc_live_reset();
const log = [];
for (const [r, k] of runs) for (let i = 0; i < k; i++) {
  const spec = base + r;
  frame(spec);
  const last = log[log.length - 1];
  if (last && last.spec === spec) last.n++; else log.push({ spec, n: 1, lit: true });   // = live.js logFrames
}
const tSession = M._isc_live_time(), next = base + 'x=1.5 y=-1 z=0.0135 exposure-ms=50';
const want = frame(next);
const saved = JSON.parse(JSON.stringify(C.historyOut(eng, log, tSession)));
const noSample = saved.records.every(r => !('specimen' in r.set) && !('ruler-mode' in r.set) && !('world-seed' in r.set));
const replay = C.replaySpecs(eng, saved.records, base);
const tr = performance.now();
M._isc_live_reset();
let ok = true;
for (const r of replay) {
  const s = str(r.spec);
  ok = ok && M._isc_live_advance(s, r.n, r.lit ? 1 : 0, err, 512) >= 0;
  M._free(s);
}
const replayMs = performance.now() - tr, tReplay = M._isc_live_time(), got = frame(next);
M._isc_live_reset();
const fresh = frame(next);   // (the control: without the history the frame is another)
check(fresh.some((v, i) => v !== want[i]), 'the control: the same frame on a fresh sample differs from the session\'s');
check(ok && noSample && saved.records.length === 4 && saved.records[1].set['laser-561'] === 3 && saved.records[3].set['laser-561'] === null &&
  tReplay === tSession && got.length === want.length && got.every((v, i) => v === want[i]),
  `a session's light history (${saved.records.length} runs, ${log.reduce((a, r) => a + r.n, 0)} frames, t ${tSession.toFixed(2)} s) saved without ` +
  `the sample's keys, replayed in ${replayMs.toFixed(0)} ms: the next frame is the session's, bit for bit (${want.length} px)`);

// ---- reading a file ----
const here = { release: '0.1.0', worldVersion: '2026-10-05' };
const file = (made, version = 1) => JSON.stringify({ format: 'insiliscope-settings', version, made, parts: { sample: { values: { seed: 1 } } } });
const w = (made, version) => C.readFile(file(made, version), here).warnings.join(' | ');
const refused = t => { try { C.readFile(t, here); return false; } catch (e) { return true; } };
check(w({ release: '0.1.0', worldVersion: '2026-10-05' }) === '' && /0\.0\.1-old/.test(w({ release: '0.0.1-old' })) &&
  /world version 2020-01-01/.test(w({ release: '0.1.0', worldVersion: '2020-01-01' })) && /version 2/.test(w({}, 2)),
  'warned: another release, another world version, a newer format; none for the same');
check(refused('{not json') && refused('{"format":"other","parts":{}}') && refused('{"format":"insiliscope-settings"}'),
  'refused: broken JSON, another format, no parts');

// ---- the setup readout's Zernike modes ----
function zernike(spec) {
  const s = str(spec), need = M._isc_scope_setup(s, 0, 0, err, 512), out = M._malloc(need + 1);
  M._isc_scope_setup(s, out, need + 1, err, 512);
  const j = JSON.parse(M.UTF8ToString(out));
  M._free(s); M._free(out);
  return j.zernike;
}
const zp = zernike(base + 'psf-zernike-preset=2'), zo = zernike(base + 'psf-zernike-preset=2 zern.12=0.3');
check(zp.length === 28 && zp[5] !== 0 && zo[12] === 0.3 && zo.every((v, j) => j === 12 || v === zp[j]),
  `the setup's Zernike modes: the preset's (AstigmatismModerate: mode 5 = ${zp[5]}), zern.12 replacing its own`);
M._free(info); M._free(err);

console.log(`settings unit: ${fail ? fail + ' failure(s)' : 'all ok'}, ${((performance.now() - t0) / 1000).toFixed(2)} s`);
process.exit(fail ? 1 : 0);
