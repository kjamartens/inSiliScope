#!/usr/bin/env node
// Builds the dye library the imaging code reads, from data/ (no network; refresh FPbase with tools/fetch_fpbase.mjs):
//   node tools/gen_dye_library.mjs [--check]
// Inputs: data/dyes/library.json, light_path.json, cameras.json (BSD-3-Clause), fpbase_spectra.json (CC BY-SA 4.0),
// data/references.json. Outputs (never edit by hand; --check fails when they are stale):
//   web/prototype/scope/dye_library_data.js  the resolved library for the JS reference (and, later, the C++ twin)
//   docs/references.md                        the project reference list
// Resolution: per dye and state the FPbase scalars with library.json 'override' on top, the FPbase spectra (or
// parametric shapes for Custom), and for every dSTORM block the ON time, dark time and bleach probability that
// reproduce Dempsey et al. 2011's detected photons per cycle, duty cycle and switching cycles at the reference light
// path (library.json dstormReference), with the ON time then fixed. BSD-3-Clause (this script).
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { GRID_MIN_NM, GRID_MAX_NM, GRID_STEP_NM, GRID_N, idealTransmission, parametricExcitation, parametricEmission,
  crossSectionUm2, photonFluxPerUm2, collectionEfficiency, detection } from '../web/prototype/scope/spectra.js';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const CHECK = process.argv.includes('--check');
const read = f => JSON.parse(fs.readFileSync(path.join(ROOT, f), 'utf8'));
const library = read('data/dyes/library.json'), lightPath = read('data/dyes/light_path.json');
const cameras = read('data/dyes/cameras.json'), fp = read('data/dyes/fpbase_spectra.json');
const references = read('data/references.json').references;
if (fp.grid.minNm !== GRID_MIN_NM || fp.grid.maxNm !== GRID_MAX_NM || fp.grid.stepNm !== GRID_STEP_NM)
  throw new Error('fpbase_spectra.json grid != spectra.js grid');

const usedSpectra = new Set();
const spectrumKey = id => { if (fp.spectra[id] == null) throw new Error(`spectrum ${id} not fetched (run tools/fetch_fpbase.mjs)`); usedSpectra.add(String(id)); return `fp:${id}`; };
const values = key => key.startsWith('fp:') ? fp.spectra[key.slice(3)].values : null;
const checkRefs = (where, refs) => { for (const r of refs ?? []) if (!references[r]) throw new Error(`${where}: unknown reference '${r}'`); };

// ---- dyes ----
const dyes = library.dyes.map(d => {
  checkRefs(d.id, d.refs);
  const out = { id: d.id, name: d.name, category: d.category, defaultMode: d.defaultMode,
    fluorescentFraction: d.fluorescentFraction, states: {}, modes: {}, notes: d.notes ?? null, source: null };
  if (d.parametric) {
    const p = d.parametric;
    out.states.main = { exMaxNm: p.exPeakNm, emMaxNm: p.emPeakNm, extCoeff: p.extCoeff, qy: p.qy, lifetimeNs: null,
      emHex: null, isDark: false, parametric: { exPeakNm: p.exPeakNm, emPeakNm: p.emPeakNm, exWidthNm: p.exWidthNm, emWidthNm: p.emWidthNm } };
  } else {
    const e = fp.entities[d.id];
    if (!e) throw new Error(`${d.id}: not in fpbase_spectra.json (run tools/fetch_fpbase.mjs)`);
    out.source = { fpbase: e.url, ref: 'lambert2019' };
    for (const [role, s] of Object.entries(e.states)) {
      const st = { exMaxNm: s.exMaxNm, emMaxNm: s.emMaxNm, extCoeff: s.extCoeff, qy: s.qy, lifetimeNs: s.lifetimeNs,
        emHex: s.emHex, isDark: s.isDark, fpbaseState: s.state ?? null, spectraFrom: s.spectraFrom ?? null,
        ex: s.ex ? spectrumKey(s.ex) : null, em: s.em ? spectrumKey(s.em) : null };
      if (role === 'main' && d.override) Object.assign(st, d.override, { overridden: Object.keys(d.override) });
      out.states[role] = st;
    }
  }
  for (const [mode, m] of Object.entries(d.modes)) {
    checkRefs(`${d.id} ${mode}`, m.refs);
    out.modes[mode] = mode === 'dSTORM' ? deriveDstorm(d, out, m) : { ...m };
  }
  return out;
});

function stateSpectra(st) {
  if (st.parametric) return { ex: parametricExcitation(st.parametric.exPeakNm, st.parametric.exWidthNm),
                              em: parametricEmission(st.parametric.emPeakNm, st.parametric.emWidthNm) };
  return { ex: values(st.ex), em: values(st.em) };
}

// onSec from the detected photons per cycle at the reference light path; dark time from the duty cycle
// (dc = on / (on + off)); bleach probability per blink = 1 / mean switching cycles.
function deriveDstorm(d, out, m) {
  const ref = library.dstormReference, band = ref.byLaser[m.laser];
  if (!band) throw new Error(`${d.id} dSTORM: no reference band for laser ${m.laser}`);
  const st = out.states.main, sp = stateSpectra(st);
  const kEm = st.qy * crossSectionUm2(st.extCoeff, sp.ex, m.laser) * photonFluxPerUm2(ref.intensityKWcm2, m.laser);
  const filt = idealTransmission({ type: 'bandpass', loNm: band.emission[0], hiNm: band.emission[1] });
  const detected = kEm * collectionEfficiency(ref.na, ref.immersionIndex) * detection(sp.em, [filt]).fraction * ref.qe;
  const onSec = m.detectedPhotonsPerCycle / detected;
  return { ...m, onSec, offSec: onSec * (1 - m.dutyCycle) / m.dutyCycle, bleachProb: Math.min(1, 1 / m.switchingCycles),
    derived: `onSec, offSec, bleachProb from detectedPhotonsPerCycle/dutyCycle/switchingCycles at ${ref.intensityKWcm2} kW/cm^2, ${m.laser} nm, `
      + `band ${band.emission[0]}-${band.emission[1]} nm, NA ${ref.na}, QE ${ref.qe} (detected ${Math.round(detected)} photons/s while ON)` };
}

// ---- light path, cameras ----
const filterEntry = f => {
  checkRefs(f.id, f.ref ? [f.ref] : []);
  return f.fpbase ? { id: f.id, name: f.name, curve: spectrumKey(f.fpbase), ref: f.ref } : { id: f.id, name: f.name, ideal: f.ideal };
};
const cameraEntry = c => {
  checkRefs(c.id, c.refs);
  return { ...c, qeCurve: typeof c.qeCurve === 'number' ? spectrumKey(c.qeCurve) : c.qeCurve };
};
const data = {
  grid: fp.grid,
  fetched: fp.fetched,
  lasers: lightPath.lasers,
  dichroics: lightPath.dichroics.map(filterEntry),
  emissionFilters: lightPath.emissionFilters.map(filterEntry),
  lightPathDefaults: lightPath.defaults,
  cameras: cameras.presets.map(cameraEntry),
  cameraDefault: cameras.default,
  dstormReference: library.dstormReference,
  dyes,
  dyeDefault: library.default,
  spectra: Object.fromEntries([...usedSpectra].sort((a, b) => a - b).map(id => [`fp:${id}`, fp.spectra[id].values])),
  spectrumInfo: Object.fromEntries([...usedSpectra].sort((a, b) => a - b).map(id => {
    const s = fp.spectra[id];
    return [`fp:${id}`, { owner: s.owner, subtype: s.subtype, url: s.url }];
  })),
};
if (Object.values(data.spectra).some(v => v.length !== GRID_N)) throw new Error('spectrum length != grid');

const js = `// GENERATED by tools/gen_dye_library.mjs from data/dyes/*.json -- do not edit; run the generator.
// Code: BSD-3-Clause. Spectra (DYE_DATA.spectra) and FPbase scalars: CC BY-SA 4.0, from FPbase
// (https://www.fpbase.org), Lambert TJ (2019) Nat Methods 16:277-278, doi:10.1038/s41592-019-0352-8.
// Other values: see each entry's refs/notes and data/references.json (docs/references.md).
export const DYE_DATA = ${JSON.stringify(data, null, 1).replace(/\[[\d\s.,eE+-]+\]/g, m => m.replace(/\s+/g, ''))};
`;

// ---- docs/references.md ----
const fmt = (k, r) => {
  const where = [r.journal, r.volume && `**${r.volume}**`, r.pages].filter(Boolean).join(' ');
  const link = r.doi ? `[doi:${r.doi}](https://doi.org/${r.doi})` : r.url ? `<${r.url}>` : '';
  return `- <a id="${k}"></a>**${k}** -- ${r.authors} (${r.year}). ${r.title}.${where ? ` *${where}*.` : ''} ${link}`
    + `${r.pmcid ? ` (${r.pmcid})` : ''}  \n  Used for: ${r.use}. *Checked: ${r.checked}.*`;
};
const md = `# References

Every literature value, default or model in the simulator names one of these keys where it is used (data files under
\`data/\`, code comments, these pages). *Checked* says how the citation, and where stated the values, were verified.
Generated from \`data/references.json\` by \`tools/gen_dye_library.mjs\`; edit that file, not this page.

${Object.entries(references).sort((a, b) => a[0].localeCompare(b[0])).map(([k, r]) => fmt(k, r)).join('\n')}
`;

let stale = false;
for (const [rel, text] of [['web/prototype/scope/dye_library_data.js', js], ['docs/references.md', md]]) {
  const p = path.join(ROOT, rel), old = fs.existsSync(p) ? fs.readFileSync(p, 'utf8').replace(/\r\n/g, '\n') : null;
  if (old === text) continue;
  if (CHECK) { console.error(`${rel} is stale: run node tools/gen_dye_library.mjs`); stale = true; }
  else { fs.writeFileSync(p, text); console.log(`wrote ${rel}`); }
}
if (stale) process.exit(1);
for (const d of dyes) for (const [mode, m] of Object.entries(d.modes))
  if (m.derived) console.log(`${d.id} ${mode}: onSec ${m.onSec.toPrecision(3)} s, offSec ${m.offSec.toPrecision(3)} s, bleachProb ${m.bleachProb.toFixed(3)}`);
