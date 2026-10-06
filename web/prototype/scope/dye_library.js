// Dyes, the light path and their photophysics (issue 16): the JS reference for adapter/inSiliScope/Simulation/
// DyeLibrary.* + LightPath.* (port: issue 16 phase 3). From a dye (library entry or a slot, with overrides), a mode
// and the microscope's light path it gives, per emitting state (main, pre):
//   excitation rate per dye at illumination 1 (sum over laser lines of cross section x photon flux x the dichroic's
//   reflectance at the line), emission rate (x QY), detected fraction (emission x dichroic T x emission filter T x
//   camera QE), effective wavelength (the PSF's), detected photons per second, and the world label's kinetics.
// Data: DYE_DATA (dye_library_data.js, generated from data/dyes by tools/gen_dye_library.mjs).
import { DYE_DATA } from './dye_library_data.js';
import { GRID_N, idealTransmission, parametricExcitation, parametricEmission, crossSectionUm2, photonFluxPerUm2,
  collectionEfficiency, detection, sampleAt, gridNm } from './spectra.js';
import { makeLabel } from './dyes.js';

export { DYE_DATA };
export const DYE_IDS = DYE_DATA.dyes.map(d => d.id);
export const DYE_SLOTS = ['Dye1', 'Dye2', 'Dye3'];
// A structure's dye choice: a library dye (index into DYE_IDS) or a slot (DYE_IDS.length + slot index).
export const DYE_CHOICES = [...DYE_IDS, ...DYE_SLOTS];
export const MODES = ['dSTORM', 'PALM', 'DNA-PAINT', 'WideField'];
export const EXCITATION_FILTER_IDS = DYE_DATA.excitationFilters.map(f => f.id);
export const DICHROIC_IDS = DYE_DATA.dichroics.map(f => f.id);
export const EMISSION_FILTER_IDS = DYE_DATA.emissionFilters.map(f => f.id);
export const CAMERA_IDS = DYE_DATA.cameras.map(c => c.id);
export const LASER_LINES = DYE_DATA.lasers;
const AVOGADRO_PER_NM_UM3 = 6.02214076e23 * 1e-9 * 1e-15;   // molecules per um^3 at 1 nM

const specCache = new Map();
export function spectrum(key) {
  let s = specCache.get(key);
  if (!s) { s = Float64Array.from(DYE_DATA.spectra[key]); specCache.set(key, s); }
  return s;
}
export const dyeById = id => { const d = DYE_DATA.dyes.find(x => x.id === id); if (!d) throw new Error(`unknown dye '${id}'`); return d; };

// ---- effective dye: a library entry + overrides (a slot's, then the structure's) ----
// Override keys (option suffixes) -> where they go. Mode fields act on the dye's block of the mode in use.
export const DYE_FIELDS = {
  'fluorescent-pct': { get: d => d.fluorescentFraction * 100, set: (d, v) => { d.fluorescentFraction = v / 100; } },
  'qy': { state: 'main', key: 'qy' }, 'ext-coeff': { state: 'main', key: 'extCoeff' },
  'pre-qy': { state: 'pre', key: 'qy' }, 'pre-ext-coeff': { state: 'pre', key: 'extCoeff' },
  'ex-peak-nm': { param: 'exPeakNm' }, 'em-peak-nm': { param: 'emPeakNm' }, 'ex-width-nm': { param: 'exWidthNm' },
  'em-width-nm': { param: 'emWidthNm' },
  'on-sec': { mode: 'onSec' }, 'off-sec': { mode: m => m.offSecBetweenBlinks !== undefined ? 'offSecBetweenBlinks' : 'offSec' },
  'bleach-prob': { mode: 'bleachProb' }, 'photon-cv': { mode: 'photonCV' }, 'initial-on-sec': { mode: 'initialOnSec' },
  'activation-405': { mode: 'activation405PerKWcm2PerSec' }, 'spont-activation': { mode: 'spontaneousActivationPerSec' },
  'primed': { primed: true }, 'kon': { mode: 'konPerMPerSec' }, 'photon-budget': { mode: 'photonBudget' },
  'pre-photon-budget': { mode: 'prePhotonBudget' },
};

// The dye a structure uses: choice = index into DYE_CHOICES; slots = [{source, overrides}] x 3; overrides = {field: v}.
// mode: index into MODES or -1 (the dye's default). Returns { dye (resolved copy), mode (name) }.
export function effectiveDye(choice, slots, overrides, modeIndex) {
  let base, chain = [];
  if (choice >= DYE_IDS.length) {
    const slot = slots[choice - DYE_IDS.length];
    if (!slot) throw new Error(`dye slot ${choice - DYE_IDS.length + 1} is not set`);
    base = DYE_DATA.dyes[slot.source];
    chain.push(slot.overrides || {});
  } else base = DYE_DATA.dyes[choice];
  if (!base) throw new Error(`dye choice ${choice} out of range`);
  chain.push(overrides || {});
  const d = structuredClone(base);
  const mode = modeIndex >= 0 ? MODES[modeIndex] : d.defaultMode;
  if (!d.modes[mode]) throw new Error(`${d.name} has no ${mode} block (tools/gen_dye_library.mjs fills generic ones)`);
  for (const ov of chain) for (const [k, v] of Object.entries(ov)) applyField(d, mode, k, v);
  return { dye: d, mode };
}
function applyField(d, mode, k, v) {
  const f = DYE_FIELDS[k];
  if (!f) throw new Error(`unknown dye field '${k}' (one of ${Object.keys(DYE_FIELDS).join(', ')})`);
  if (f.set) return f.set(d, v);
  if (f.state) {
    if (!d.states[f.state]) throw new Error(`${d.name} has no ${f.state} state ('${k}')`);
    d.states[f.state][f.key] = v;
    return;
  }
  if (f.param) {
    const p = d.states.main.parametric;
    if (!p) throw new Error(`'${k}' applies to parametric (Custom) dyes only`);
    p[f.param] = v;
    if (f.param === 'exPeakNm') d.states.main.exMaxNm = v;
    if (f.param === 'emPeakNm') d.states.main.emMaxNm = v;
    return;
  }
  const m = d.modes[mode];
  if (f.primed) { m.primed = { primeNm: [470, 510], convertNm: [690, 780], ...(m.primed || {}), perKWcm2SqPerSec: v }; return; }
  m[typeof f.mode === 'function' ? f.mode(m) : f.mode] = v;
}

export function stateSpectra(st) {
  if (st.parametric) return { ex: parametricExcitation(st.parametric.exPeakNm, st.parametric.exWidthNm),
                              em: parametricEmission(st.parametric.emPeakNm, st.parametric.emWidthNm) };
  return { ex: st.ex ? spectrum(st.ex) : null, em: st.em ? spectrum(st.em) : null };
}

// ---- light path ----
// lasers: [{nm, kWPerCm2}] (intensity at the sample with a perfect mirror and no excitation filter; the excitation
// filter's transmission and the dichroic's reflectance scale it), excitationFilter / dichroic / emissionFilter: an
// EXCITATION_FILTER_IDS / DICHROIC_IDS / EMISSION_FILTER_IDS index with the Custom parameters, qe: a camera
// QE curve (CAMERA_IDS index; 'Flat' presets use qeFlat), na, immersionIndex, chamberHeightUm.
export function makeLightPath(o) {
  const filt = (list, i, custom) => {
    const f = list[i];
    if (!f) throw new Error(`filter index ${i} out of range`);
    if (f.curve) return { name: f.name, T: spectrum(f.curve) };
    const spec = { ...f.ideal };
    if (f.id === 'Custom') Object.assign(spec, custom);
    return { name: f.name, T: idealTransmission(spec) };
  };
  const ex = filt(DYE_DATA.excitationFilters, o.excitationFilter ?? 0, { loNm: o.exLoNm, hiNm: o.exHiNm });
  const dich = filt(DYE_DATA.dichroics, o.dichroic, { edgeNm: o.dichroicEdgeNm });
  const em = filt(DYE_DATA.emissionFilters, o.emissionFilter, { loNm: o.emLoNm, hiNm: o.emHiNm });
  const cam = DYE_DATA.cameras[o.qeCurve];
  const qe = cam && typeof cam.qeCurve === 'string' && cam.qeCurve.startsWith('fp:') ? spectrum(cam.qeCurve)
    : new Float64Array(GRID_N).fill(o.qeFlat);
  return { lasers: o.lasers.filter(l => l.kWPerCm2 > 0).map(l => ({ nm: l.nm, kWPerCm2: l.kWPerCm2 * sampleAt(ex.T, l.nm) })),
    excitationFilter: ex, dichroic: dich, emissionFilter: em, qe, qeName: cam ? cam.id : 'Flat',
    na: o.na, immersionIndex: o.immersionIndex, chamberHeightUm: o.chamberHeightUm,
    eta: collectionEfficiency(o.na, o.immersionIndex) };
}
// Intensity of the lasers whose line falls in [lo, hi] nm (primed conversion windows), kW/cm^2 at the sample.
const intensityIn = (lp, [lo, hi]) => lp.lasers.reduce((s, l) => s + (l.nm >= lo && l.nm <= hi ? l.kWPerCm2 * (1 - sampleAt(lp.dichroic.T, l.nm)) : 0), 0);
const intensityAt = (lp, nm) => lp.lasers.reduce((s, l) => s + (Math.abs(l.nm - nm) < 0.5 ? l.kWPerCm2 * (1 - sampleAt(lp.dichroic.T, l.nm)) : 0), 0);

// Emission and detection of one state at illumination 1. null for a dark state.
export function statePhotophysics(st, lp) {
  if (st.isDark) return null;
  const sp = stateSpectra(st);
  if (!sp.ex || !sp.em) throw new Error('state without spectra');
  let kExc = 0;
  for (const l of lp.lasers)
    kExc += crossSectionUm2(st.extCoeff, sp.ex, l.nm) * photonFluxPerUm2(l.kWPerCm2 * (1 - sampleAt(lp.dichroic.T, l.nm)), l.nm);
  const kEm = st.qy * kExc;
  const det = detection(sp.em, [lp.dichroic.T, lp.emissionFilter.T, lp.qe]);
  const lambdaNm = Number.isFinite(det.lambdaNm) ? det.lambdaNm : st.emMaxNm;
  return { excitationPerSec: kExc, emissionPerSec: kEm, detectedFraction: det.fraction, lambdaNm,
    detectedPerSec: kEm * lp.eta * det.fraction, color: st.emHex };
}

// The world label (dyes.js makeLabel) and the per-state photophysics of a structure's dye in its mode.
//   density: site fraction; imagerNm: DNA-PAINT imager concentration; orientation/motion: dyes.js label fields.
// Returns { label, mode, dye, states: {main, pre?}, kActPerSec, imagerBackgroundPerPxPerSec(pixelUm), notes }.
export function labelPhotophysics(eff, lp, o) {
  const { dye, mode } = eff, m = dye.modes[mode];
  const main = statePhotophysics(dye.states.main, lp);
  const pre = mode === 'PALM' && dye.states.pre ? statePhotophysics(dye.states.pre, lp) : null;
  const kin = { activationRatePerSec: 0, onSec: Math.max(1e-6, m.onSec ?? 0.05), offSec: 1.0, bleachProb: 1.0,
    photonCV: Math.max(0, m.photonCV ?? 0.5), initialOnSec: 0 };
  let kAct = 0;
  let excitationScale = 1;
  if (mode === 'dSTORM') {
    // ON time, spontaneous dark time and initial ON time are given at the reference light path (Dempsey et al. 2011's
    // measurement intensity) and scale as 1 / (excitation rate / kExcRef): off- and on-switching are linear in the
    // excitation, so photons per blink and the duty cycle stay as measured (dstormReference notes). The 405 nm
    // activation adds to the return rate.
    excitationScale = main && m.kExcRef > 0 ? Math.max(1e-6, main.excitationPerSec / m.kExcRef) : 1;
    kin.onSec = Math.max(1e-6, (m.onSec ?? 0.02) / excitationScale);
    kAct = excitationScale / Math.max(1e-6, m.offSec) + (m.activation405PerKWcm2PerSec ?? 0) * intensityAt(lp, 405);
    Object.assign(kin, { activationRatePerSec: kAct, offSec: 1 / kAct, bleachProb: m.bleachProb,
      initialOnSec: (m.initialOnSec ?? 0) / excitationScale });
  } else if (mode === 'PALM') {
    kAct = (m.spontaneousActivationPerSec ?? 0) + (m.activation405PerKWcm2PerSec ?? 0) * intensityAt(lp, 405);
    if (m.primed) kAct += m.primed.perKWcm2SqPerSec * intensityIn(lp, m.primed.primeNm) * intensityIn(lp, m.primed.convertNm);
    Object.assign(kin, { activationRatePerSec: kAct, offSec: m.offSecBetweenBlinks ?? 0.5, bleachProb: m.bleachProb ?? 1 });
  } else if (mode === 'DNA-PAINT') {
    kAct = (m.konPerMPerSec ?? 1e6) * Math.max(0, o.imagerNm) * 1e-9;
    kin.activationRatePerSec = kAct;
  }
  const label = makeLabel({ density: o.density, fluorescentFraction: dye.fluorescentFraction, mode, kinetics: kin,
    preState: !!pre || (mode === 'PALM' && !!dye.states.pre), orientation: o.orientation, motion: o.motion, offTarget: o.offTarget });
  const states = { main };
  if (pre) states.pre = pre;
  return {
    label, mode, dye, states, kActPerSec: kAct, excitationScale, onSecNow: kin.onSec,
    photonBudget: m.photonBudget ?? (mode === 'WideField' ? 100000 : 0), prePhotonBudget: m.prePhotonBudget ?? 0,
    // Free imager in solution: uniform, c x N_A x chamber height x pixel area x its detected rate (no depletion, no
    // exclusion from cells; schnitzbauer2017).
    imagerBackgroundPerPxPerSec: pixelUm => mode === 'DNA-PAINT' && main
      ? Math.max(0, o.imagerNm) * AVOGADRO_PER_NM_UM3 * lp.chamberHeightUm * pixelUm * pixelUm * main.detectedPerSec : 0,
    notes: m.notes ?? null,
  };
}

// Camera preset values (cameras.json) as camera options.
export function cameraPreset(i) {
  const c = DYE_DATA.cameras[i];
  if (!c) throw new Error(`camera preset ${i} out of range`);
  return c;
}

// A camera preset's gain (e-/ADU, per photoelectron) for the imaging at hand: gainElectronsPerAduWideField, when the
// preset has one, for WideField-only labels or BrightField, else gainElectronsPerAdu (C++ CameraPresetGain).
export const cameraPresetGain = (c, wideFieldOrBrightField) =>
  (wideFieldOrBrightField && c.gainElectronsPerAduWideField !== undefined ? c.gainElectronsPerAduWideField : c.gainElectronsPerAdu);
// An EMCCD's EM gain follows from its gain: the gain (e-/ADU) is per photoelectron, i.e. the pre-amplifier sensitivity
// (e-/ADU after the EM register; the preset's preampElectronsPerAdu, 1 when it has none) divided by the EM gain. At
// least 1 (no multiplication). C++ CameraPreamp / EmGainFromGain.
export const cameraPreamp = c => (c.preampElectronsPerAdu === undefined ? 1.0 : c.preampElectronsPerAdu);
export const emGainFromGain = (preamp, gain) => (gain > 0.0 ? Math.max(1.0, preamp / gain) : 1.0);

// QE of the light path at the emission filter's transmission-weighted centre: the flat background's QE.
export function backgroundQe(lp) {
  let s = 0, m = 0;
  for (let i = 0; i < GRID_N; i++) { const t = lp.emissionFilter.T[i] * lp.dichroic.T[i]; s += t; m += t * gridNm(i); }
  return sampleAt(lp.qe, s > 0 ? m / s : 600);
}
