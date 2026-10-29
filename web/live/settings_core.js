// SPDX-License-Identifier: BSD-3-Clause
// The part of the settings files (web/live/settings_file.js, docs/dev/SAVE_SETTINGS.md) that needs no page: keys and
// their parts, values by name, the parts' ids, the light history as saved and as replayed, reading a file. `eng` is the
// engine's option table (self.ISC_ENGINE_OPTIONS: name -> {def, part, names}, from isc_scope_options). Node loads it
// too (tests/web/settings_unit.mjs).
'use strict';
(function () {
const FORMAT = 'insiliscope-settings', VERSION = 1;
// "k=v k=v" -> Map (the viewer writes numbers)
const parseSpec = text => {
  const m = new Map();
  for (const t of text.split(/[\s,;]+/)) { const i = t.indexOf('='); if (i > 0) m.set(t.slice(0, i), +t.slice(i + 1)); }
  return m;
};
// The part of a key (= ScopeOptionPart, ScopeMovie.cpp): the engine's table for its named options, p.* sample, zern.*
// instrument, the dye field overrides (<prefix>-dye.<field>, dye<N>.<field>) sample; null: none.
function partOf(eng, k) {
  const o = eng.get(k);
  if (o) return o.part;
  if (/^p\./.test(k)) return 'sample';
  if (/^zern\.\d+$/.test(k)) return 'instrument';
  if (/^([a-z]+-dye|dye\d)\./.test(k)) return 'sample';
  return null;
}
// An option's value by name where it has names (isc_scope_options), else the number.
function toName(eng, k, v) {
  const o = eng.get(k);
  return o && o.names && Number.isInteger(v) && v >= 0 && v < o.names.length ? o.names[v] : v;
}
// A file's value as the option's number (= ScopeOptionValue: a number, a name, Typical / DyeDefault / Global); NaN.
function fromName(eng, k, v) {
  if (typeof v === 'number') return v;
  if (typeof v === 'boolean') return v ? 1 : 0;
  if (typeof v !== 'string') return NaN;
  if (/-dye$/.test(k) && v === 'Typical') return -1;
  if (/-mode$/.test(k) && v === 'DyeDefault') return -1;
  if (/-mode$/.test(k) && v === 'Global') return -2;
  if (k === 'mode' && v === 'None') return -1;
  const o = eng.get(k), i = o && o.names ? o.names.indexOf(v) : -1;
  if (i >= 0) return i;
  return v.trim() !== '' && Number.isFinite(+v) ? +v : NaN;
}
// A short id of a part's values (53-bit cyrb53 of the canonical JSON, base 32): the same values, the same id.
function idOf(values) {
  const text = JSON.stringify(Object.keys(values).sort().map(k => [k, values[k]]));
  let h1 = 0xdeadbeef, h2 = 0x41c6ce57;
  for (let i = 0; i < text.length; i++) {
    const c = text.charCodeAt(i);
    h1 = Math.imul(h1 ^ c, 2654435761); h2 = Math.imul(h2 ^ c, 1597334677);
  }
  h1 = Math.imul(h1 ^ (h1 >>> 16), 2246822507) ^ Math.imul(h2 ^ (h2 >>> 13), 3266489909);
  h2 = Math.imul(h2 ^ (h2 >>> 16), 2246822507) ^ Math.imul(h1 ^ (h1 >>> 13), 3266489909);
  return (4294967296 * (2097151 & h2) + (h1 >>> 0)).toString(32).padStart(11, '0').slice(-6);
}
// The light history as saved, from the live log (runs {spec, n, lit}): each run's changes from the run before (null: a
// key it no longer has), without the sample's keys (the loaded sample's go in on replay), enumerations by name.
function historyOut(eng, log, tSec) {
  const records = [];
  let prev = new Map();
  const keep = k => partOf(eng, k) !== 'sample' && partOf(eng, k) !== 'host';
  for (const r of log) {
    const m = new Map([...parseSpec(r.spec)].filter(([k]) => keep(k))), set = {};
    for (const [k, v] of m) if (prev.get(k) !== v) set[k] = toName(eng, k, v);
    for (const k of prev.keys()) if (!m.has(k)) set[k] = null;
    records.push({ n: r.n, lit: r.lit !== false, set });
    prev = m;
  }
  return { tSec: tSec || 0, records };
}
// The runs as the live worker replays them: full specs, the sample's keys of `sampleSpec` (the spec now) in each
// (settings_file.cpp Replay).
function replaySpecs(eng, records, sampleSpec) {
  const sample = [...parseSpec(sampleSpec)].filter(([k]) => partOf(eng, k) === 'sample');
  const out = [], m = new Map();
  for (const r of records) {
    for (const [k, raw] of Object.entries(r.set || {})) { if (raw === null) m.delete(k); else m.set(k, fromName(eng, k, raw)); }
    const s = new Map(m);
    for (const [k, v] of sample) s.set(k, v);
    out.push({ spec: [...s].map(([k, v]) => k + '=' + v).join(' '), n: Math.max(1, r.n | 0), lit: r.lit !== false });
  }
  return out;
}
// A file read: checked, migrated to this version (no renames yet), and the warnings for here ({release, worldVersion}).
const RENAMED = {};   // old key -> new key, when an option is renamed
function readFile(text, here) {
  let file;
  try { file = JSON.parse(text); } catch (e) { throw new Error('not a settings file (not JSON)'); }
  if (!file || file.format !== FORMAT || typeof file.parts !== 'object') throw new Error('not an inSiliScope settings file');
  const warnings = [];
  if (+file.version > VERSION) warnings.push(`a newer file format (version ${file.version}): what this viewer knows of it is read`);
  const made = file.made || {};
  if (made.release && made.release !== here.release)
    warnings.push(`made with inSiliScope ${made.release}, this viewer is ${here.release}: the same settings, the image may differ`);
  else if (made.worldVersion && made.worldVersion !== here.worldVersion)
    warnings.push(`made with world version ${made.worldVersion}, this viewer has ${here.worldVersion}: the cells may differ`);
  for (const p of Object.values(file.parts)) if (p && p.values) for (const [o, n] of Object.entries(RENAMED)) if (o in p.values) { p.values[n] = p.values[o]; delete p.values[o]; }
  return { file, warnings };
}
self.iscSettingsCore = { FORMAT, VERSION, parseSpec, partOf, toName, fromName, idOf, historyOut, replaySpecs, readFile };
})();
