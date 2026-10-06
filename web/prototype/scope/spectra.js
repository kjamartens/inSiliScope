// Spectra and the light path's physics (issue 16): sampled spectra on one grid, ideal filters, absorption cross
// section, laser photon flux, detected fraction and effective emission wavelength. The JS reference for
// adapter/inSiliScope/Simulation/Spectra.* (port: issue 16 phase 3). Pure functions, no state.
//
// Grid: 300..900 nm in 1 nm steps (data/dyes/fpbase_spectra.json). Spectra are sampled values on it (dye
// excitation/emission normalised to peak 1 by FPbase; filter transmission and camera QE as fractions).

export const GRID_MIN_NM = 300, GRID_MAX_NM = 900, GRID_STEP_NM = 1;
export const GRID_N = Math.round((GRID_MAX_NM - GRID_MIN_NM) / GRID_STEP_NM) + 1;
export const gridNm = i => GRID_MIN_NM + i * GRID_STEP_NM;

export const AVOGADRO = 6.02214076e23;
const PLANCK = 6.62607015e-34, LIGHT_SPEED = 299792458;

// Value of a sampled spectrum at nm (linear between grid points, 0 outside).
export function sampleAt(values, nm) {
  const x = (nm - GRID_MIN_NM) / GRID_STEP_NM;
  if (!(x >= 0) || x > GRID_N - 1) return 0;
  const i = Math.min(GRID_N - 2, Math.floor(x)), f = x - i;
  return values[i] * (1 - f) + values[i + 1] * f;
}

// Skewed-Gaussian shape with peak 1 at peakNm: widths are the FWHM-like spreads (nm) on the short- and long-wavelength
// sides. Excitation spectra have a long blue tail, emission spectra a long red one (Custom dyes; no FPbase data).
export function skewedGaussian(peakNm, leftWidthNm, rightWidthNm) {
  const out = new Float64Array(GRID_N), k = 4 * Math.log(2);
  for (let i = 0; i < GRID_N; ++i) {
    const d = gridNm(i) - peakNm, w = d < 0 ? leftWidthNm : rightWidthNm;
    out[i] = Math.exp(-k * (d / Math.max(1e-6, w)) ** 2);
  }
  return out;
}
export const parametricExcitation = (peakNm, widthNm) => skewedGaussian(peakNm, 1.6 * widthNm, 0.6 * widthNm);
export const parametricEmission = (peakNm, widthNm) => skewedGaussian(peakNm, 0.6 * widthNm, 1.6 * widthNm);

// Transmission of an ideal filter: { type: 'none' | 'longpass' (edgeNm) | 'bandpass' (loNm, hiNm) | 'notch'
// (reflectNm: [[lo, hi], ...] reflected, the rest transmitted) }. Hard edges on the grid.
export function idealTransmission(spec) {
  const out = new Float64Array(GRID_N);
  for (let i = 0; i < GRID_N; ++i) {
    const nm = gridNm(i);
    let t = 1;
    if (spec.type === 'longpass') t = nm >= spec.edgeNm ? 1 : 0;
    else if (spec.type === 'shortpass') t = nm <= spec.edgeNm ? 1 : 0;
    else if (spec.type === 'bandpass') t = nm >= spec.loNm && nm <= spec.hiNm ? 1 : 0;
    else if (spec.type === 'notch') t = spec.reflectNm.some(([lo, hi]) => nm >= lo && nm <= hi) ? 0 : 1;
    else if (spec.type !== 'none') throw new Error(`unknown ideal filter type '${spec.type}'`);
    out[i] = t;
  }
  return out;
}

// Absorption cross section, um^2, at a wavelength: ln(10) 1000 eps(lambda) / N_A (cm^2) x 1e8, eps(lambda) = eps at
// the peak x the normalised excitation spectrum there.
export const crossSectionUm2 = (epsPeak, exValues, nm) => Math.log(10.0) * 1000.0 * epsPeak * sampleAt(exValues, nm) / AVOGADRO * 1e8;

// Photon flux of a laser line, photons / um^2 / s, for an intensity in kW/cm^2 at the sample.
export const photonFluxPerUm2 = (kWPerCm2, nm) => kWPerCm2 * 1e3 / (PLANCK * LIGHT_SPEED / (nm * 1e-9)) * 1e-8;

// Fraction of the solid angle an objective of numerical aperture na collects in a medium of index n (isotropic emitter).
export const collectionEfficiency = (na, n) => {
  const r = Math.min(1.0, Math.max(0.0, na / Math.max(1e-6, n)));
  return 0.5 * (1.0 - Math.sqrt(1.0 - r * r));
};

// Detection of an emission spectrum through a list of transmission curves (dichroic, emission filter, camera QE...):
// fraction = sum(E T1 T2 ...) / sum(E), and the photon-weighted mean wavelength of what is detected (nm; NaN if
// nothing is).
export function detection(emValues, curves) {
  let total = 0, kept = 0, moment = 0;
  for (let i = 0; i < GRID_N; ++i) {
    const e = emValues[i];
    if (!(e > 0)) continue;
    let t = 1;
    for (const c of curves) t *= c[i];
    total += e;
    kept += e * t;
    moment += e * t * gridNm(i);
  }
  return { fraction: total > 0 ? kept / total : 0, lambdaNm: kept > 0 ? moment / kept : NaN };
}
