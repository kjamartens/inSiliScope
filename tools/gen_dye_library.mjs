#!/usr/bin/env node
// Builds the dye library the imaging code reads, from data/ (no network; refresh FPbase with tools/fetch_fpbase.mjs):
//   node tools/gen_dye_library.mjs [--check]
// Inputs: data/dyes/library.json, light_path.json, cameras.json (BSD-3-Clause), fpbase_spectra.json (CC BY-SA 4.0),
// data/specimens.json (the specimens and their targets), data/references.json, data/fiducials/ (the fiducial-marker
// types, the spectra read from balinovic2019 (CC BY 3.0) and the gold Mie table). Outputs (never edit by hand; --check fails when they are stale):
//   web/dye_library.js                        the resolved library for the viewer's option panel (a classic script)
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
  crossSectionUm2, photonFluxPerUm2, collectionEfficiency, detection } from './spectra.mjs';
import { curveValues } from './fiducial_spectra/curves.mjs';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const CHECK = process.argv.includes('--check');
const read = f => JSON.parse(fs.readFileSync(path.join(ROOT, f), 'utf8'));
const library = read('data/dyes/library.json'), lightPath = read('data/dyes/light_path.json');
const cameras = read('data/dyes/cameras.json'), fp = read('data/dyes/fpbase_spectra.json');
const references = read('data/references.json').references;
const specimens = read('data/specimens.json');
const fiducialTypes = read('data/fiducials/types.json'), paperSpectra = read('data/fiducials/paper_spectra.json');
const goldMie = read('data/fiducials/gold_mie.json');
if (fp.grid.minNm !== GRID_MIN_NM || fp.grid.maxNm !== GRID_MAX_NM || fp.grid.stepNm !== GRID_STEP_NM)
  throw new Error('fpbase_spectra.json grid != spectra.js grid');

const usedSpectra = new Set(), usedPaper = new Set();
const spectrumKey = id => { if (fp.spectra[id] == null) throw new Error(`spectrum ${id} not fetched (run tools/fetch_fpbase.mjs)`); usedSpectra.add(String(id)); return `fp:${id}`; };
// A fiducial spectrum: 'fp:<id>' (FPbase) or 'paper:<curve>' (data/fiducials/paper_spectra.json) -> 'cs:<curve>'.
const fiducialSpectrumKey = k => {
  if (k.startsWith('fp:')) return spectrumKey(Number(k.slice(3)));
  if (!k.startsWith('paper:') || !paperSpectra.curves[k.slice(6)]) throw new Error(`fiducial spectrum '${k}': not fp:<id> or paper:<curve>`);
  usedPaper.add(k.slice(6));
  return `cs:${k.slice(6)}`;
};
const values = key => key.startsWith('fp:') ? fp.spectra[key.slice(3)].values
  : key.startsWith('cs:') ? curveValues(paperSpectra.curves[key.slice(3)]) : null;
const checkRefs = (where, refs) => { for (const r of refs ?? []) if (!references[r]) throw new Error(`${where}: unknown reference '${r}'`); };

// ---- dyes ----
const GENERIC_MODES = {
  dSTORM: { laser: 640, onSec: 0.02, offSec: 20, bleachProb: 0.1, photonCV: 0.5, initialOnSec: 2, activation405PerKWcm2PerSec: 20 },
  PALM: { onSec: 0.05, offSecBetweenBlinks: 0.5, bleachProb: 0.5, photonCV: 0.5, spontaneousActivationPerSec: 1e-5,
    activation405PerKWcm2PerSec: 1 },
  'DNA-PAINT': { photonCV: 0.5 },   // the binding kinetics are the target's (data/specimens.json paint)
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
    if (mode === 'DNA-PAINT' && (m.konPerMPerSec !== undefined || m.onSec !== undefined))
      throw new Error(`${d.id} DNA-PAINT: konPerMPerSec / onSec are the target's binding kinetics (data/specimens.json paint), not the dye's`);
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
// ---- specimens, targets, typical labels ----
const MODES = ['dSTORM', 'PALM', 'DNA-PAINT', 'WideField'];
checkRefs('typicalLabels', library.typicalLabels.refs);
const targets = specimens.specimens.flatMap(sp => sp.targets.map(t => ({ ...t, specimen: sp.id })));
targets.forEach((t, i) => { if (t.structure !== i) throw new Error(`target ${t.id}: structure ${t.structure}, expected ${i} (the core's ISC_STRUCT_* order)`); });
for (const sp of specimens.specimens)
  if (sp.lightPreset && !lightPath.presets.some(q => q.id === sp.lightPreset)) throw new Error(`specimen ${sp.id}: unknown light preset '${sp.lightPreset}'`);
targets.forEach(t => {
  const q = t.paint;
  if (!q || !(q.konPerMPerSec >= 0) || !(q.onSec > 0) || !(q.imagerNm >= 0))
    throw new Error(`target ${t.id}: paint { konPerMPerSec >= 0, onSec > 0, imagerNm >= 0 } (its DNA-PAINT binding kinetics, default imager)`);
  checkRefs(`target ${t.id} paint`, q.refs);
  // defaultDye: the <prefix>-dye default when it is not the typical dye; only 'None' (off until labelled) for now.
  if (t.defaultDye !== undefined && t.defaultDye !== 'None') throw new Error(`target ${t.id}: defaultDye '${t.defaultDye}' (only 'None')`);
});
const typicalLabels = Object.fromEntries(targets.map(t => {
  const byMode = library.typicalLabels[t.id];
  if (!byMode) throw new Error(`target ${t.id}: no typicalLabels in data/dyes/library.json`);
  return [t.id, Object.fromEntries(MODES.map(m => {
    const e = byMode[m], d = e && dyes.find(x => x.id === e.dye);
    if (!d) throw new Error(`typicalLabels ${t.id} ${m}: unknown dye '${e && e.dye}'`);
    if (d.modes[m].generic) throw new Error(`typicalLabels ${t.id} ${m}: ${d.id} has no ${m} data`);
    return [m, { dye: e.dye, labelingPct: e.labelingPct }];
  }))];
}));
// The label linkage presets (library.json linkagePresets): Legacy = each structure's built-in numbers.
const linkagePresets = library.linkagePresets.presets.map((q, i) => {
  checkRefs(q.id, q.refs);
  if (i === 0 !== !!q.structureDefault) throw new Error('linkagePresets: the first (and only the first) preset is the structure default');
  if (!q.structureDefault && !(q.binderNm >= 0 && q.binderFlexNm >= 0 && q.linkerMinNm >= 0 && q.linkerMaxNm >= q.linkerMinNm))
    throw new Error(`linkage preset ${q.id}: binderNm, binderFlexNm >= 0, 0 <= linkerMinNm <= linkerMaxNm`);
  return q.structureDefault ? { id: q.id, name: q.name, structureDefault: true }
    : { id: q.id, name: q.name, binderNm: q.binderNm, binderFlexNm: q.binderFlexNm, linkerMinNm: q.linkerMinNm, linkerMaxNm: q.linkerMaxNm };
});
// Only the targets listed in linkagePresets.defaults take the linkage model (the nanorulers' docking strands have
// their own geometry: origamiLinkerNm).
const linkageDefaults = Object.fromEntries(targets.filter(t => t.id in library.linkagePresets.defaults).map(t => {
  const id = library.linkagePresets.defaults[t.id];
  const i = linkagePresets.findIndex(q => q.id === id);
  if (i < 0) throw new Error(`linkagePresets.defaults: target ${t.id} has an unknown preset '${id}'`);
  return [t.id, id];
}));
for (const k of Object.keys(library.linkagePresets.defaults))
  if (k !== 'notes' && !targets.some(t => t.id === k)) throw new Error(`linkagePresets.defaults: unknown target ${k}`);
// ---- fiducial markers (data/fiducials/types.json) ----
// Every component resolved: spectra keys, 'emitters' equivalent dyes at the type's reference diameter (fitted by
// tools/fiducial_spectra/fit.mjs, or taken from another component / type: emittersFrom, emittersFromType), its
// excitation peak (the bleach half-time's reference wavelength).
checkRefs('fiducials', Object.values(fiducialTypes.references).flatMap(r => r.refs));
const fiducialEmitters = (t, c) => {
  if (c.emittersSet !== undefined) {   // set by hand: wins over the calibration (whose fit stays in 'emitters')
    if (!(c.emittersSet > 0)) throw new Error(`fiducial ${t.id} ${c.id}: emittersSet must be > 0`);
    return c.emittersSet;
  }
  if (c.calibration) {
    if (!(c.emitters > 0)) throw new Error(`fiducial ${t.id} ${c.id}: no emitters (run tools/fiducial_spectra/fit.mjs)`);
    return c.emitters;
  }
  if (c.emittersFrom) {
    const o = t.components.find(x => x.id === c.emittersFrom);
    if (!o) throw new Error(`fiducial ${t.id} ${c.id}: unknown emittersFrom '${c.emittersFrom}'`);
    return fiducialEmitters(t, o);
  }
  if (c.emittersFromType) {
    const u = fiducialTypes.types.find(x => x.id === c.emittersFromType);
    if (!u || u.components.length !== 1 || u.refDiameterNm !== t.refDiameterNm)
      throw new Error(`fiducial ${t.id} ${c.id}: emittersFromType needs a one-component type of the same diameter`);
    return fiducialEmitters(u, u.components[0]);
  }
  if (c.emitters > 0) return c.emitters;
  throw new Error(`fiducial ${t.id} ${c.id}: emitters, calibration, emittersFrom or emittersFromType`);
};
if (!(goldMie.lambdaNm.minNm >= GRID_MIN_NM && goldMie.lambdaNm.maxNm <= GRID_MAX_NM)) throw new Error('gold_mie.json: wavelengths outside the grid');
const fiducials = {
  equivalentDye: { extCoeff: fiducialTypes.equivalentDye.extCoeff, qy: fiducialTypes.equivalentDye.qy },
  defaults: Object.fromEntries(Object.entries(fiducialTypes.defaults).filter(([k]) => k !== 'notes')),
  types: fiducialTypes.types.map(t => {
    checkRefs(`fiducial ${t.id}`, t.refs);
    if (t.scatter && t.scatter !== 'gold') throw new Error(`fiducial ${t.id}: scatter '${t.scatter}' (only gold)`);
    return { id: t.id, name: t.name, refDiameterNm: t.refDiameterNm, scatter: t.scatter ?? null,
      components: t.components.map(c => {
        const ex = fiducialSpectrumKey(c.ex), em = fiducialSpectrumKey(c.em), exv = values(ex);
        let peak = 0;
        for (let i = 1; i < GRID_N; ++i) if (exv[i] > exv[peak]) peak = i;
        return { id: c.id, name: c.name, ex, em, exPeakNm: GRID_MIN_NM + peak * GRID_STEP_NM, emitters: fiducialEmitters(t, c),
          bleaches: !!c.bleaches };
      }) };
  }),
  // Gold spheres in water: Mie cross sections (um^2), rows per diameter, columns per wavelength (bilinear).
  goldMie: { lambdaNm: goldMie.lambdaNm, diameterNm: goldMie.diameterNm, scatteringUm2: goldMie.scatteringUm2,
    absorptionUm2: goldMie.absorptionUm2 },
};
if (!fiducials.types.some(t => t.id === fiducials.defaults.type)) throw new Error(`fiducial default type '${fiducials.defaults.type}'`);

const data = {
  grid: fp.grid,
  fetched: fp.fetched,
  lasers: lightPath.lasers,
  excitationFilters: lightPath.excitationFilters.map(filterEntry),
  dichroics: lightPath.dichroics.map(filterEntry),
  emissionFilters: lightPath.emissionFilters.map(filterEntry),
  lightPathDefaults: lightPath.defaults,
  // The filters of the 'auto' light preset when the targets' presets differ (light_path.json multiTarget).
  lightPathMultiTarget: (() => {
    const m = lightPath.multiTarget;
    if (!lightPath.dichroics.some(f => f.id === m.dichroic)) throw new Error(`multiTarget: unknown dichroic '${m.dichroic}'`);
    if (!lightPath.emissionFilters.some(f => f.id === m.emissionFilter)) throw new Error(`multiTarget: unknown emission filter '${m.emissionFilter}'`);
    return { dichroic: m.dichroic, emissionFilter: m.emissionFilter };
  })(),
  lightPresets: lightPath.presets.map(q0 => {
    const q = { ...q0, excitationFilter: q0.excitationFilter ?? 'None' };   // no clean-up filter unless the preset names one
    checkRefs(q.id, q.refs);
    for (const [k, list] of [['excitationFilter', lightPath.excitationFilters], ['dichroic', lightPath.dichroics],
                             ['emissionFilter', lightPath.emissionFilters]])
      if (!list.some(f => f.id === q[k])) throw new Error(`light preset ${q.id}: unknown ${k} '${q[k]}'`);
    for (const nm of Object.keys(q.lasers)) if (!lightPath.lasers.includes(+nm)) throw new Error(`light preset ${q.id}: no ${nm} nm laser`);
    return q;
  }),
  cameras: cameras.presets.map(cameraEntry),
  cameraDefault: cameras.default,
  dstormReference: library.dstormReference,
  dyes,
  dyeDefault: library.default,
  specimens: specimens.specimens.map(sp => ({ id: sp.id, name: sp.name, targets: sp.targets, ...(sp.illumination ? { illumination: sp.illumination } : {}),
    ...(sp.fiducials ? { fiducials: { densityPerUm2: sp.fiducials.densityPerUm2, aggregationPct: sp.fiducials.aggregationPct } } : {}),
    ...(sp.lightPreset ? { lightPreset: sp.lightPreset } : {}) })),
  specimenDefault: specimens.default,
  typicalLabels,
  linkagePresets,
  linkageDefaults,
  fiducials,
  spectra: Object.fromEntries([...[...usedSpectra].sort((a, b) => a - b).map(id => [`fp:${id}`, fp.spectra[id].values]),
    ...[...usedPaper].sort().map(c => [`cs:${c}`, values(`cs:${c}`)])]),
  spectrumInfo: Object.fromEntries([...[...usedSpectra].sort((a, b) => a - b).map(id => {
    const s = fp.spectra[id];
    return [`fp:${id}`, { owner: s.owner, subtype: s.subtype, url: s.url }];
  }), ...[...usedPaper].sort().map(c => [`cs:${c}`, { owner: `balinovic2019 fig. ${paperSpectra.curves[c].figure}`,
    subtype: paperSpectra.curves[c].kind === 'excitation' ? 'EX' : 'EM', url: 'https://doi.org/10.1088/1361-6463/ab0862' }])]),
};
if (Object.values(data.spectra).some(v => v.length !== GRID_N)) throw new Error('spectrum length != grid');

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
// The dye library for the viewer's option panel (web/index.html): the same data as the C++
// DyeLibraryData.inc. Code: BSD-3-Clause; spectra and FPbase scalars: CC BY-SA 4.0, from FPbase
// (https://www.fpbase.org), Lambert TJ (2019) Nat Methods 16:277-278, doi:10.1038/s41592-019-0352-8; the 'cs:' spectra
// CC BY 3.0, read from Balinovic A et al. (2019) J Phys D 52:204002, doi:10.1088/1361-6463/ab0862.
self.ISC_DYE_DATA = ${JSON.stringify(data).replace(/\[[\d\s.,eE+-]+\]/g, m => m.replace(/\s+/g, ''))};
`;

// ---- the C++ twin (adapter/inSiliScope/Simulation/DyeLibrary.cpp includes it) ----
// The same values as web/dye_library.js: numbers as JS prints them (shortest round trip, so the C++ literal is the
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
  bandpass: 'FilterType::BandPass', notch: 'FilterType::Notch', multiband: 'FilterType::MultiBand' };
const cFilter = f => {
  if (f.curve) return `   { ${str(f.id)}, ${str(f.name)}, ${str(f.curve)}, { FilterType::None, kNaN, kNaN, kNaN, 0, {} } },`;
  const i = f.ideal, bands = i.reflectNm ?? i.passNm ?? [];
  if (bands.length > 8) throw new Error(`${f.id}: more than 8 bands`);
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
    + `${num(m.photonBudget)}, ${num(m.prePhotonBudget)}, ${num(m.kExcRef)}, ${primed}, ${str(m.lightPreset)}, `
    + `${m.generic ? 'true' : 'false'} }`;
};
const CAMERA_FIELDS = ['quantumEfficiency', 'readNoiseElectrons', 'gainElectronsPerAdu', 'preampElectronsPerAdu', 'cicElectrons', 'offsetAdu',
  'offsetStdAdu', 'darkCurrentElectronsPerSec', 'gainStdPct', 'readNoiseStdPct', 'bitDepth',
  'gainElectronsPerAduWideField', 'physicalPixelUm'];
const spectrumIds = Object.keys(data.spectra);
const cpp = `// GENERATED by tools/gen_dye_library.mjs from data/dyes/*.json -- do not edit; run the generator.
// The C++ library (the same data as web/dye_library.js), included by DyeLibrary.cpp (types in DyeLibrary.h).
// Code: BSD-3-Clause. Spectra (kSpectra) and FPbase scalars: CC BY-SA 4.0, from FPbase (https://www.fpbase.org),
// Lambert TJ (2019) Nat Methods 16:277-278, doi:10.1038/s41592-019-0352-8; the 'cs:' spectra CC BY 3.0, read from
// Balinovic A et al. (2019) J Phys D 52:204002, doi:10.1088/1361-6463/ab0862. Other values: see data/dyes/*.json refs and
// notes, data/references.json (docs/references.md).

${spectrumIds.map((k, i) => `static const double kSpectrum${i}[${GRID_N}] = {${data.spectra[k].map(num).join(',')}};`).join('\n')}

static const SpectrumData kSpectra[] = {
${spectrumIds.map((k, i) => `   { ${str(k)}, kSpectrum${i} },`).join('\n')}
};

static const int kLaserLines[] = { ${data.lasers.join(', ')} };

static const FilterData kExcitationFilters[] = {
${data.excitationFilters.map(cFilter).join('\n')}
};

static const FilterData kDichroics[] = {
${data.dichroics.map(cFilter).join('\n')}
};

static const FilterData kEmissionFilters[] = {
${data.emissionFilters.map(cFilter).join('\n')}
};

// Per laser line of kLaserLines (kW/cm^2; 0 = off); the BFP position it sets (illum-bfp; NaN = none: epi).
static const LightPresetData kLightPresets[] = {
${data.lightPresets.map(q => `   { ${str(q.id)}, ${str(q.name)}, { ${data.lasers.map(nm => num(q.lasers[nm] ?? 0)).join(', ')} }, ${str(q.excitationFilter)}, ${str(q.dichroic)}, ${str(q.emissionFilter)}, ${num(q.bfpPosition)} },`).join('\n')}
};

// ${CAMERA_FIELDS.join(', ')}
static const CameraData kCameras[] = {
${data.cameras.map(c => `   { ${str(c.id)}, ${str(c.name)}, ${str(c.type)}, ${str(c.qeCurve)}, { ${CAMERA_FIELDS.map(f => num(c[f])).join(', ')} } },`).join('\n')}
};

// States main, pre; modes ${MODE_NAMES.join(', ')}.
static const DyeData kDyes[] = {
${data.dyes.map(d => `   { ${str(d.id)}, ${str(d.name)}, ${str(d.defaultMode)}, ${num(d.fluorescentFraction)},\n     ${cState(d.states.main)},\n     ${cState(d.states.pre)},\n     { ${MODE_NAMES.map(m => cMode(d.modes[m])).join(',\n       ')} } },`).join('\n')}
};

// The specimens and their targets (data/specimens.json), the targets' typical labels per mode (library.json
// typicalLabels: dye index, % of the sites) in the order ${MODE_NAMES.join(', ')}; the DNA-PAINT binding kinetics
// (paint: k_on /M/s, mean bound time s, default imager concentration nM); the default linkage preset (index into
// kLinkagePresets; -1 = no linkage model, library.json linkagePresets.defaults); defaultNone: the dye defaults to None
// (specimens.json defaultDye: off until labelled).
static const TargetData kTargets[] = {
${targets.map(t => `   { ${str(t.id)}, ${str(t.name)}, ${str(t.prefix)}, ${str(t.specimen)}, ${t.structure}, { ${MODE_NAMES.map(m => dyes.findIndex(d => d.id === typicalLabels[t.id][m].dye)).join(', ')} }, { ${MODE_NAMES.map(m => num(typicalLabels[t.id][m].labelingPct)).join(', ')} }, ${num(t.paint.konPerMPerSec)}, ${num(t.paint.onSec)}, ${num(t.paint.imagerNm)}, ${t.id in linkageDefaults ? linkagePresets.findIndex(q => q.id === linkageDefaults[t.id]) : -1}, ${t.defaultDye === 'None'} },`).join('\n')}
};
// The label linkage presets (library.json linkagePresets): binder, flexibility, linker min, max (nm); the first is
// each structure's built-in numbers (structureDefault).
static const LinkagePresetData kLinkagePresets[] = {
${linkagePresets.map(q => `   { ${str(q.id)}, ${str(q.name)}, ${q.structureDefault ? 'true' : 'false'}, { ${['binderNm', 'binderFlexNm', 'linkerMinNm', 'linkerMaxNm'].map(k => num(q[k])).join(', ')} } },`).join('\n')}
};
// The filters of the auto light preset when the targets' presets differ (light_path.json multiTarget).
static const char* const kMultiTargetDichroic = ${str(data.lightPathMultiTarget.dichroic)};
static const char* const kMultiTargetEmissionFilter = ${str(data.lightPathMultiTarget.emissionFilter)};
// id, name, its targets (first, count), the BFP position in place of the light preset's (NaN = none), the bead density and
// aggregation a mount sets when none is (NaN = none), the light preset when no target is labelled (nullptr = none).
static const SpecimenData kSpecimens[] = {
${data.specimens.map(sp => `   { ${str(sp.id)}, ${str(sp.name)}, ${targets.findIndex(t => t.specimen === sp.id)}, ${sp.targets.length}, ${num(sp.illumination && sp.illumination.bfpPosition)}, ${num(sp.fiducials && sp.fiducials.densityPerUm2)}, ${num(sp.fiducials && sp.fiducials.aggregationPct)}, ${str(sp.lightPreset ?? null)} },`).join('\n')}
};
// Fiducial beads (data/fiducials/types.json): per component id, name, excitation and emission spectra, the excitation
// peak (nm), the equivalent dyes at the type's reference diameter, whether it bleaches; per type id, name, reference
// diameter (nm), gold's laser scatter, its components (first, count); the equivalent dye (eps /M/cm, QY).
static const FiducialComponentData kFiducialComponents[] = {
${data.fiducials.types.flatMap(t => t.components.map(c => `   { ${str(c.id)}, ${str(c.name)}, ${str(c.ex)}, ${str(c.em)}, ${num(c.exPeakNm)}, ${num(c.emitters)}, ${c.bleaches ? 'true' : 'false'} },`)).join('\n')}
};
static const FiducialTypeData kFiducialTypes[] = {
${data.fiducials.types.map((t, i) => `   { ${str(t.id)}, ${str(t.name)}, ${num(t.refDiameterNm)}, ${t.scatter === 'gold' ? 'true' : 'false'}, ${data.fiducials.types.slice(0, i).reduce((n, u) => n + u.components.length, 0)}, ${t.components.length} },`).join('\n')}
};
static const double kFiducialEquivalentDye[2] = { ${num(data.fiducials.equivalentDye.extCoeff)}, ${num(data.fiducials.equivalentDye.qy)} };
// type, densityPerUm2, diameterNm, diameterCvPct, height, layerUm, bleachHalfSec, bleachRefKWcm2
static const FiducialDefaultsData kFiducialDefaults = { ${str(data.fiducials.defaults.type)}, ${num(data.fiducials.defaults.densityPerUm2)}, ${num(data.fiducials.defaults.diameterNm)}, ${num(data.fiducials.defaults.diameterCvPct)}, ${str(data.fiducials.defaults.height)}, ${num(data.fiducials.defaults.layerUm)}, ${num(data.fiducials.defaults.bleachHalfSec)}, ${num(data.fiducials.defaults.bleachRefKWcm2)} };
// Gold spheres' scattering cross section in water, um^2 (data/fiducials/gold_mie.json), [diameter][wavelength] on
// the grids (min, max, step nm): wavelength, diameter.
static const double kGoldMieScattering[] = {${data.fiducials.goldMie.scatteringUm2.flat().map(num).join(',')}};
static const GoldMieData kGoldMie = { ${['lambdaNm', 'diameterNm'].flatMap(k => ['minNm', 'maxNm', 'stepNm'].map(f => num(data.fiducials.goldMie[k][f]))).join(', ')}, kGoldMieScattering };
static const char* const kDefaultExcitationFilter = ${str(data.lightPathDefaults.excitationFilter)};
static const char* const kDefaultDichroic = ${str(data.lightPathDefaults.dichroic)};
static const char* const kDefaultEmissionFilter = ${str(data.lightPathDefaults.emissionFilter)};
static const char* const kDefaultLightPreset = ${str(data.lightPathDefaults.preset)};
static const char* const kDefaultCamera = ${str(data.cameraDefault)};
static const char* const kDefaultDye = ${str(data.dyeDefault.dye)};
static const char* const kDefaultDyeMode = ${str(data.dyeDefault.mode)};
`;

let stale = false;
for (const [rel, text] of [['web/dye_library.js', viewerJs], ['docs/references.md', md],
  ['adapter/inSiliScope/Simulation/DyeLibraryData.inc', cpp]]) {
  const p = path.join(ROOT, rel), old = fs.existsSync(p) ? fs.readFileSync(p, 'utf8').replace(/\r\n/g, '\n') : null;
  if (old === text) continue;
  if (CHECK) { console.error(`${rel} is stale: run node tools/gen_dye_library.mjs`); stale = true; }
  else { fs.writeFileSync(p, text); console.log(`wrote ${rel}`); }
}
if (stale) process.exit(1);
for (const d of dyes) for (const [mode, m] of Object.entries(d.modes))
  if (m.derived) console.log(`${d.id} ${mode}: onSec ${m.onSec.toPrecision(3)} s, offSec ${m.offSec.toPrecision(3)} s, bleachProb ${m.bleachProb.toFixed(3)}`);
