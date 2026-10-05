#!/usr/bin/env node
// Builds the dye library the imaging code reads, from data/ (no network; refresh FPbase with tools/fetch_fpbase.mjs):
//   node tools/gen_dye_library.mjs [--check]
// Inputs: data/dyes/library.json, light_path.json, cameras.json (BSD-3-Clause), fpbase_spectra.json (CC BY-SA 4.0),
// data/references.json. Outputs (never edit by hand; --check fails when they are stale):
//   web/prototype/scope/dye_library_data.js  the resolved library for the JS reference (and, later, the C++ twin)
//   web/dye_library.js                        the same for the viewer's option panel (a classic script)
//   docs/references.md                        the project reference list
//   adapter/inSiliScope/Simulation/DyeLibraryData.inc  the C++ twin (DyeLibrary.cpp)
// Resolution: per dye and state the FPbase scalars with library.json 'override' on top, the FPbase spectra (or
// parametric shapes for Custom), and for every dSTORM block the ON time, dark time and bleach probability that
// reproduce Dempsey et al. 2011's detected photons per cycle, duty cycle and switching cycles at the reference light
// path (library.json dstormReference), with the ON time then fixed. BSD-3-Clause (this script).
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { GRID_MIN_NM, GRID_MAX_NM, GRID_STEP_NM, GRID_N, idealTransmission, sampleAt, parametricExcitation, parametricEmission,
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
const GENERIC_MODES = {
  dSTORM: { laser: 640, onSec: 0.02, offSec: 20, bleachProb: 0.1, photonCV: 0.5, initialOnSec: 2, activation405PerKWcm2PerSec: 20 },
  PALM: { onSec: 0.05, offSecBetweenBlinks: 0.5, bleachProb: 0.5, photonCV: 0.5, spontaneousActivationPerSec: 1e-5,
    activation405PerKWcm2PerSec: 1 },
  'DNA-PAINT': { konPerMPerSec: 1e6, onSec: 0.05, photonCV: 0.5 },
  WideField: { photonBudget: 100000 },
};
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
  // Every dye can be used in every mode: a mode without data gets generic values, marked as such.
  for (const [mode, g] of Object.entries(GENERIC_MODES))
    if (!out.modes[mode]) out.modes[mode] = { ...g, generic: true, notes: `generic ${mode} values (estimate): no ${mode} data for ${d.name} in data/dyes/library.json` };
  // Each mode's illumination preset (light_path.json presets): given, or by the laser line that excites the dye best.
  const sp = stateSpectra(out.states.main), best = lines => lines.reduce((a, b) => (sampleAt(sp.ex, b) > sampleAt(sp.ex, a) ? b : a));
  for (const [mode, m] of Object.entries(out.modes)) {
    if (!m.lightPreset) m.lightPreset = mode === 'PALM' ? 'PALM-561'
      : `${{ dSTORM: 'dSTORM', 'DNA-PAINT': 'PAINT', WideField: 'WF' }[mode]}-${best(mode === 'WideField' ? [405, 488, 561, 640] : [488, 561, 640])}`;
    if (!lightPath.presets.some(q => q.id === m.lightPreset)) throw new Error(`${d.id} ${mode}: unknown light preset '${m.lightPreset}'`);
  }
  // dSTORM times scale with the excitation rate: its value at the reference light path (generic blocks too).
  const ds = out.modes.dSTORM;
  if (ds && ds.kExcRef === undefined) ds.kExcRef = referenceExcitation(out, ds.laser).kExc;
  return out;
});

function stateSpectra(st) {
  if (st.parametric) return { ex: parametricExcitation(st.parametric.exPeakNm, st.parametric.exWidthNm),
                              em: parametricEmission(st.parametric.emPeakNm, st.parametric.emWidthNm) };
  return { ex: values(st.ex), em: values(st.em) };
}

// onSec from the detected photons per cycle at the reference light path; dark time from the duty cycle
// (dc = on / (on + off)); bleach probability per blink = 1 / mean switching cycles.
// The dye's excitation rate (per dye, /s) and detected rate at the dSTORM reference light path of a laser line.
function referenceExcitation(out, laser) {
  const ref = library.dstormReference, band = ref.byLaser[laser];
  if (!band) throw new Error(`dSTORM: no reference band for laser ${laser}`);
  const st = out.states.main, sp = stateSpectra(st);
  const kExc = crossSectionUm2(st.extCoeff, sp.ex, laser) * photonFluxPerUm2(band.intensityKWcm2, laser);
  const filt = idealTransmission({ type: 'bandpass', loNm: band.emission[0], hiNm: band.emission[1] });
  const detected = st.qy * kExc * collectionEfficiency(ref.na, ref.immersionIndex) * detection(sp.em, [filt]).fraction * ref.qe;
  return { kExc, detected, band };
}
// onSec from the detected photons per cycle at the reference light path; dark time from the duty cycle
// (dc = on / (on + off)); bleach probability per blink = 1 / mean switching cycles. All three at the reference; the
// ON and dark times then scale with 1 / (excitation rate / kExcRef) (dstormReference notes).
function deriveDstorm(d, out, m) {
  const { kExc, detected, band } = referenceExcitation(out, m.laser);
  const onSec = m.detectedPhotonsPerCycle / detected;
  return { ...m, onSec, offSec: onSec * (1 - m.dutyCycle) / m.dutyCycle, bleachProb: Math.min(1, 1 / m.switchingCycles),
    kExcRef: kExc,
    derived: `onSec, offSec, bleachProb from detectedPhotonsPerCycle/dutyCycle/switchingCycles at ${band.intensityKWcm2} kW/cm^2, ${m.laser} nm, `
      + `band ${band.emission[0]}-${band.emission[1]} nm, NA ${library.dstormReference.na}, QE ${library.dstormReference.qe} `
      + `(detected ${Math.round(detected)} photons/s while ON, excitation ${Math.round(kExc)} /s = kExcRef)` };
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
  lightPresets: lightPath.presets.map(q => {
    checkRefs(q.id, q.refs);
    for (const [k, list] of [['dichroic', lightPath.dichroics], ['emissionFilter', lightPath.emissionFilters]])
      if (!list.some(f => f.id === q[k])) throw new Error(`light preset ${q.id}: unknown ${k} '${q[k]}'`);
    for (const nm of Object.keys(q.lasers)) if (!lightPath.lasers.includes(+nm)) throw new Error(`light preset ${q.id}: no ${nm} nm laser`);
    return q;
  }),
  cameras: cameras.presets.map(cameraEntry),
  cameraDefault: cameras.default,
  dstormReference: library.dstormReference,
  dyes,
  dyeDefault: library.default,
  suggestedLabelingPct: library.suggestedLabelingPct,
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

// The viewer's copy (a classic script: the option panel is built synchronously from it).
const viewerJs = `// GENERATED by tools/gen_dye_library.mjs from data/dyes/*.json -- do not edit; run the generator.
// The dye library for the viewer's option panel (web/index.html): the same data as
// web/prototype/scope/dye_library_data.js. Code: BSD-3-Clause; spectra and FPbase scalars: CC BY-SA 4.0, from FPbase
// (https://www.fpbase.org), Lambert TJ (2019) Nat Methods 16:277-278, doi:10.1038/s41592-019-0352-8.
self.ISC_DYE_DATA = ${JSON.stringify(data).replace(/\[[\d\s.,eE+-]+\]/g, m => m.replace(/\s+/g, ''))};
`;

// ---- the C++ twin (adapter/inSiliScope/Simulation/DyeLibrary.cpp includes it) ----
// The same values as dye_library_data.js: numbers as JS prints them (shortest round trip, so the C++ literal is the
// same double), JSON null / missing as kNaN (DyeLibrary.h: "not given"). Only what the imaging code reads.
const num = v => (v === null || v === undefined ? 'kNaN' : (() => {
  if (typeof v !== 'number' || !Number.isFinite(v)) throw new Error(`not a finite number: ${v}`);
  const s = String(v);
  return /[.eE]/.test(s) ? s : s + '.0';
})());
// A C++ narrow string literal: non-ASCII as UTF-8 byte escapes (independent of the compiler's source charset); a hex
// digit after an escape starts a new literal ("\xB2" "4").
const str = v => {
  if (v === null || v === undefined) return 'nullptr';
  let out = '', esc = false;
  for (const b of Buffer.from(String(v), 'utf8')) {
    const ch = String.fromCharCode(b);
    if (b >= 0x80) { out += '\\x' + b.toString(16).toUpperCase(); esc = true; continue; }
    if (esc && /[0-9a-fA-F]/.test(ch)) out += '" "';
    esc = false;
    out += ch === '"' || ch === '\\' ? '\\' + ch : ch;
  }
  return '"' + out + '"';
};
const MODE_NAMES = ['dSTORM', 'PALM', 'DNA-PAINT', 'WideField'];
const FILTER_TYPES = { none: 'FilterType::None', longpass: 'FilterType::LongPass', shortpass: 'FilterType::ShortPass',
  bandpass: 'FilterType::BandPass', notch: 'FilterType::Notch' };
const cFilter = f => {
  if (f.curve) return `   { ${str(f.id)}, ${str(f.name)}, ${str(f.curve)}, { FilterType::None, kNaN, kNaN, kNaN, 0, {} } },`;
  const i = f.ideal, bands = i.reflectNm ?? [];
  if (bands.length > 8) throw new Error(`${f.id}: more than 8 notch bands`);
  return `   { ${str(f.id)}, ${str(f.name)}, nullptr, { ${FILTER_TYPES[i.type]}, ${num(i.edgeNm)}, ${num(i.loNm)}, ${num(i.hiNm)}, `
    + `${bands.length}, { ${bands.map(([a, b]) => `{ ${num(a)}, ${num(b)} }`).join(', ')} } } },`;
};
const cState = st => !st ? '{ false, kNaN, kNaN, kNaN, kNaN, false, nullptr, nullptr, nullptr, false, kNaN, kNaN, kNaN, kNaN }'
  : `{ true, ${num(st.exMaxNm)}, ${num(st.emMaxNm)}, ${num(st.extCoeff)}, ${num(st.qy)}, ${st.isDark ? 'true' : 'false'}, `
    + `${str(st.ex)}, ${str(st.em)}, ${str(st.emHex)}, ${st.parametric ? 'true' : 'false'}, ${num(st.parametric?.exPeakNm)}, `
    + `${num(st.parametric?.emPeakNm)}, ${num(st.parametric?.exWidthNm)}, ${num(st.parametric?.emWidthNm)} }`;
const cMode = m => {
  if (!m) throw new Error('every dye has every mode (GENERIC_MODES)');
  const p = m.primed;
  const primed = p ? `{ true, ${num(p.primeNm[0])}, ${num(p.primeNm[1])}, ${num(p.convertNm[0])}, ${num(p.convertNm[1])}, ${num(p.perKWcm2SqPerSec)} }`
    : '{ false, kNaN, kNaN, kNaN, kNaN, kNaN }';
  return `{ true, ${num(m.laser)}, ${num(m.onSec)}, ${num(m.offSec)}, ${num(m.offSecBetweenBlinks)}, ${num(m.bleachProb)}, `
    + `${num(m.photonCV)}, ${num(m.initialOnSec)}, ${num(m.activation405PerKWcm2PerSec)}, ${num(m.spontaneousActivationPerSec)}, `
    + `${num(m.konPerMPerSec)}, ${num(m.photonBudget)}, ${num(m.prePhotonBudget)}, ${num(m.kExcRef)}, ${primed}, ${str(m.lightPreset)}, `
    + `${m.generic ? 'true' : 'false'} }`;
};
const CAMERA_FIELDS = ['quantumEfficiency', 'readNoiseElectrons', 'gainElectronsPerAdu', 'emGain', 'cicElectrons', 'offsetAdu',
  'offsetStdAdu', 'darkCurrentElectronsPerSec', 'gainStdPct', 'readNoiseStdPct', 'bitDepth'];
const spectrumIds = Object.keys(data.spectra);
const cpp = `// GENERATED by tools/gen_dye_library.mjs from data/dyes/*.json -- do not edit; run the generator.
// The C++ twin of web/prototype/scope/dye_library_data.js, included by DyeLibrary.cpp (types in DyeLibrary.h).
// Code: BSD-3-Clause. Spectra (kSpectra) and FPbase scalars: CC BY-SA 4.0, from FPbase (https://www.fpbase.org),
// Lambert TJ (2019) Nat Methods 16:277-278, doi:10.1038/s41592-019-0352-8. Other values: see data/dyes/*.json refs and
// notes, data/references.json (docs/references.md).

${spectrumIds.map((k, i) => `static const double kSpectrum${i}[${GRID_N}] = {${data.spectra[k].map(num).join(',')}};`).join('\n')}

static const SpectrumData kSpectra[] = {
${spectrumIds.map((k, i) => `   { ${str(k)}, kSpectrum${i} },`).join('\n')}
};

static const int kLaserLines[] = { ${data.lasers.join(', ')} };

static const FilterData kDichroics[] = {
${data.dichroics.map(cFilter).join('\n')}
};

static const FilterData kEmissionFilters[] = {
${data.emissionFilters.map(cFilter).join('\n')}
};

// Per laser line of kLaserLines (kW/cm^2; 0 = off).
static const LightPresetData kLightPresets[] = {
${data.lightPresets.map(q => `   { ${str(q.id)}, ${str(q.name)}, { ${data.lasers.map(nm => num(q.lasers[nm] ?? 0)).join(', ')} }, ${str(q.dichroic)}, ${str(q.emissionFilter)} },`).join('\n')}
};

// ${CAMERA_FIELDS.join(', ')}
static const CameraData kCameras[] = {
${data.cameras.map(c => `   { ${str(c.id)}, ${str(c.name)}, ${str(c.type)}, ${str(c.qeCurve)}, { ${CAMERA_FIELDS.map(f => num(c[f])).join(', ')} } },`).join('\n')}
};

// States main, pre; modes ${MODE_NAMES.join(', ')}.
static const DyeData kDyes[] = {
${data.dyes.map(d => `   { ${str(d.id)}, ${str(d.name)}, ${str(d.defaultMode)}, ${num(d.fluorescentFraction)},\n     ${cState(d.states.main)},\n     ${cState(d.states.pre)},\n     { ${MODE_NAMES.map(m => cMode(d.modes[m])).join(',\n       ')} } },`).join('\n')}
};

// ${MODE_NAMES.join(', ')}
static const double kSuggestedLabelingPct[] = { ${MODE_NAMES.map(m => num(data.suggestedLabelingPct[m])).join(', ')} };
static const char* const kDefaultDichroic = ${str(data.lightPathDefaults.dichroic)};
static const char* const kDefaultEmissionFilter = ${str(data.lightPathDefaults.emissionFilter)};
static const char* const kDefaultLightPreset = ${str(data.lightPathDefaults.preset)};
static const char* const kDefaultCamera = ${str(data.cameraDefault)};
static const char* const kDefaultDye = ${str(data.dyeDefault.dye)};
static const char* const kDefaultDyeMode = ${str(data.dyeDefault.mode)};
`;

let stale = false;
for (const [rel, text] of [['web/prototype/scope/dye_library_data.js', js], ['web/dye_library.js', viewerJs], ['docs/references.md', md],
  ['adapter/inSiliScope/Simulation/DyeLibraryData.inc', cpp]]) {
  const p = path.join(ROOT, rel), old = fs.existsSync(p) ? fs.readFileSync(p, 'utf8').replace(/\r\n/g, '\n') : null;
  if (old === text) continue;
  if (CHECK) { console.error(`${rel} is stale: run node tools/gen_dye_library.mjs`); stale = true; }
  else { fs.writeFileSync(p, text); console.log(`wrote ${rel}`); }
}
if (stale) process.exit(1);
for (const d of dyes) for (const [mode, m] of Object.entries(d.modes))
  if (m.derived) console.log(`${d.id} ${mode}: onSec ${m.onSec.toPrecision(3)} s, offSec ${m.offSec.toPrecision(3)} s, bleachProb ${m.bleachProb.toFixed(3)}`);
