#!/usr/bin/env node
// Fetches the FPbase data behind data/dyes/ (dev-only, needs the network):
//   node tools/fetch_fpbase.mjs            -> writes data/dyes/fpbase_spectra.json
// Reads data/dyes/library.json (dyes and fluorescent proteins by FPbase name), light_path.json (filter spectrum ids)
// and cameras.json (QE spectrum ids). Spectra are resampled linearly onto 300..900 nm in 1 nm steps (0 outside the
// FPbase range) and rounded to 1e-4; the scalars (peaks, eps, QY, lifetime, emission colour) are stored as FPbase has
// them, so library.json overrides stay visible as overrides. Output: CC BY-SA 4.0 (FPbase data), with attribution.
// Cite: Lambert TJ (2019) FPbase: a community-editable fluorescent protein database. Nat Methods 16:277-278.
// BSD-3-Clause (this script).
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const DIR = path.join(ROOT, 'data', 'dyes');
const API = 'https://www.fpbase.org/graphql/';
export const GRID = { minNm: 300, maxNm: 900, stepNm: 1 };

async function gql(query) {
  for (let attempt = 0; ; ++attempt) {
    try {
      const r = await fetch(API, { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ query }) });
      const text = await r.text();
      if (!r.ok || text.startsWith('<')) throw new Error(`HTTP ${r.status} (rate limited?)`);
      const j = JSON.parse(text);
      if (j.errors) throw new Error(JSON.stringify(j.errors));
      return j.data;
    } catch (e) {
      if (attempt >= 6) throw e;
      await new Promise(res => setTimeout(res, 2000 * 2 ** attempt));
    }
  }
}

function resample(data) {
  const pts = data.map(([w, v]) => [Number(w), Number(v)]).sort((a, b) => a[0] - b[0]);
  const n = Math.round((GRID.maxNm - GRID.minNm) / GRID.stepNm) + 1, out = new Array(n).fill(0);
  let k = 0;
  for (let i = 0; i < n; ++i) {
    const w = GRID.minNm + i * GRID.stepNm;
    if (w < pts[0][0] || w > pts[pts.length - 1][0]) continue;
    while (k < pts.length - 2 && pts[k + 1][0] < w) ++k;
    const [w0, v0] = pts[k], [w1, v1] = pts[Math.min(k + 1, pts.length - 1)];
    const v = w1 > w0 ? v0 + (v1 - v0) * (w - w0) / (w1 - w0) : v0;
    out[i] = Math.round(Math.max(0, v) * 1e4) / 1e4;
  }
  return out;
}

const read = f => JSON.parse(fs.readFileSync(path.join(DIR, f), 'utf8'));
const library = read('library.json'), lightPath = read('light_path.json'), cameras = read('cameras.json');

const stateFields = 'name exMax emMax extCoeff qy lifetime emhex isDark spectra { id subtype }';
const allDyes = (await gql(`{ dyes { id name slug exMax emMax extCoeff qy lifetime emhex spectra { id subtype } } }`)).dyes;
const proteinCache = new Map();
async function protein(name) {
  if (!proteinCache.has(name)) {
    const p = (await gql(`{ protein(name: ${JSON.stringify(name)}) { name slug switchType states { ${stateFields} } } }`)).protein;
    if (!p) throw new Error(`FPbase has no protein '${name}'`);
    proteinCache.set(name, p);
  }
  return proteinCache.get(name);
}
const pick = (spectra, kinds) => { for (const k of kinds) { const s = spectra.find(x => x.subtype === k); if (s) return Number(s.id); } return null; };
const scalars = s => ({ exMaxNm: s.exMax ?? null, emMaxNm: s.emMax ?? null, extCoeff: s.extCoeff ?? null, qy: s.qy ?? null,
  lifetimeNs: s.lifetime ?? null, emHex: s.emhex ?? null, isDark: s.isDark ?? false });

const entities = {}, wanted = new Map();  // spectrum id -> use
for (const d of library.dyes) {
  if (!d.fpbase) continue;
  const e = { fpbase: d.fpbase, states: {} };
  if (d.fpbase.dye) {
    const x = allDyes.find(y => y.name === d.fpbase.dye);
    if (!x) throw new Error(`FPbase has no dye '${d.fpbase.dye}'`);
    e.slug = x.slug;
    e.url = `https://www.fpbase.org/dye/${x.slug.replace(/-default$/, '')}/`;
    e.states.main = { ...scalars(x), ex: pick(x.spectra, ['EX', 'AB']), em: pick(x.spectra, ['EM']) };
  } else {
    const p = await protein(d.fpbase.protein);
    e.slug = p.slug;
    e.url = `https://www.fpbase.org/protein/${p.slug}/`;
    for (const role of ['main', 'pre']) {
      const stateName = role === 'main' ? (d.fpbase.main ?? p.states[0].name) : d.fpbase.pre;
      if (!stateName) continue;
      const st = p.states.find(s => s.name === stateName);
      if (!st) throw new Error(`FPbase protein '${p.name}' has no state '${stateName}'`);
      const v = { state: stateName, ...scalars(st), ex: pick(st.spectra, ['EX', 'AB']), em: pick(st.spectra, ['EM']) };
      const from = d.fpbase.spectraFrom?.[role];
      if (from) {
        const q = await protein(from.protein), fs2 = q.states.find(s => s.name === from.state);
        if (!fs2) throw new Error(`FPbase protein '${from.protein}' has no state '${from.state}'`);
        v.ex = pick(fs2.spectra, ['EX', 'AB']); v.em = pick(fs2.spectra, ['EM']);
        v.spectraFrom = `${from.protein}: ${from.state}`;
      }
      e.states[role] = v;
    }
  }
  for (const v of Object.values(e.states)) for (const k of ['ex', 'em']) if (v[k]) wanted.set(v[k], `${d.id} ${k}`);
  entities[d.id] = e;
}
for (const f of [...lightPath.dichroics, ...lightPath.emissionFilters]) if (f.fpbase) wanted.set(f.fpbase, `filter ${f.id}`);
for (const c of cameras.presets) if (typeof c.qeCurve === 'number') wanted.set(c.qeCurve, `camera ${c.id}`);

const spectra = {};
for (const [id, use] of [...wanted].sort((a, b) => a[0] - b[0])) {
  await new Promise(res => setTimeout(res, 400));  // be polite to the FPbase API
  const s = (await gql(`{ spectrum(id: ${id}) { id subtype category owner { name } data } }`)).spectrum;
  spectra[id] = { owner: s.owner?.name ?? null, subtype: s.subtype, category: s.category, use,
    url: `https://www.fpbase.org/spectra/?s=${id}`, values: resample(s.data) };
  process.stdout.write(`spectrum ${id} ${s.owner?.name} ${s.subtype}\n`);
}

const out = {
  license: 'CC BY-SA 4.0 (https://creativecommons.org/licenses/by-sa/4.0/). Data from FPbase (https://www.fpbase.org), '
    + 'resampled to the grid below. Cite: Lambert TJ (2019) FPbase: a community-editable fluorescent protein database. '
    + 'Nature Methods 16:277-278, doi:10.1038/s41592-019-0352-8. Generated by tools/fetch_fpbase.mjs; do not edit by hand.',
  fetched: new Date().toISOString().slice(0, 10),
  grid: GRID,
  entities,
  spectra,
};
// Number arrays on one line (one spectrum per line), so a refresh diffs line by line.
const text = JSON.stringify(out, null, 1).replace(/\[[\d\s.,eE+-]+\]/g, m => m.replace(/\s+/g, ''));
fs.writeFileSync(path.join(DIR, 'fpbase_spectra.json'), text + '\n');
console.log(`wrote data/dyes/fpbase_spectra.json: ${Object.keys(entities).length} dyes, ${Object.keys(spectra).length} spectra`);
