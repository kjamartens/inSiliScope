// A cell-field movie in JS: the mirror of adapter/inSiliScope/Simulation/ScopeMovie.cpp (+ CellFieldSource),
// i.e. what insiliscope_cli / the viewer's isc_scope_movie render, option for option:
//   world (seed, p.* geometry, one label per structure) -> modality 0 Fluorescence (fluorescence.js: every label in
//   its mode -- blinks splatted with their own PSF, continuous populations mean-field or per dye, the DNA-PAINT imager
//   background -- then ApplyNoiseChain) or modality 1 BrightField (brightfield.js).
// Issue 16: dyes (dye_library.js, data/dyes) on structures, a light path (lasers, dichroic, emission filter, camera QE
// curve) and camera presets replace the single dye and the SuperRes/WideField split.
// This is the JS REFERENCE for imaging. Iterate on photophysics/PSF/camera here (web/lab), then port to
// the C++ files named in each module's header when merging (web/lab/README.md, tests/parity/scope_parity.mjs).
import { World, STRUCTURES } from './world.js';
import { ZERNIKE_PRESETS, zernikePresetCoefficients, psfKernelHalfWidthPx, buildZernikeKernelCache, NUM_ZERNIKE } from './psf.js';
import { renderBrightfieldMovie } from './brightfield.js';
import { renderFluorescenceMovie } from './fluorescence.js';
import { DYE_DATA, DYE_IDS, DYE_CHOICES, DYE_FIELDS, MODES, DICHROIC_IDS, EMISSION_FILTER_IDS, CAMERA_IDS, LASER_LINES,
  effectiveDye, makeLightPath, labelPhotophysics, cameraPreset, backgroundQe } from './dye_library.js';
import { ORIENTATION_MODES, MOTION_MODES } from './dyes.js';
import { sampleAt } from './spectra.js';

const LIGHT_PRESET_CHOICES = ['auto', ...DYE_DATA.lightPresets.map(q => q.id)];
export { LIGHT_PRESET_CHOICES };
const idx = (list, id) => { const i = list.indexOf(id); if (i < 0) throw new Error(`no '${id}'`); return i; };
// The default 640 nm intensity: ATTO 655 through LP650 + 676/37 on the Kinetix detects 6375 photoelectrons/s while ON,
// what the single-dye default did (7500 photons/s x QE 0.85; scope/dye_library.js, issue 16).
export const DEFAULT_LASER_640_KW = 0.1607;
// k_on 1e6 /M/s x 1.43 nM = 1.43e-3 bindings per site per second, the former default activation rate.
export const DEFAULT_IMAGER_NM = 1.43;

// [name, default, help]: ScopeMovieOptions(), same order and defaults.
export const SCOPE_OPTIONS = [
  ['seed', 42, 'SimType_RandomSeed (cell field = seed ^ 0x43454C4C unless world-seed >= 0; noise as the adapter)'],
  ['world-seed', -1, 'cell-field world seed used as is (the viewer\'s seed); -1 = derive it from seed'],
  ['disk-cache', 1, 'per-user cache on disk (the C++ hosts; no file here): 0 = none, 1 = the packed cell positions, 2 = also the PSF kernel'],
  ['prepare', 0, '1 = build the world and the PSF kernel only (warms the caches), no frames'],
  ['x', 0, 'FOV centre x, world um (XY stage position)'],
  ['y', 0, 'FOV centre y, world um'],
  ['z', 0.5, 'Z stage: focal-plane height above the coverslip, um (as the ZStage device; it starts at 0.5)'],
  ['size', 128, 'FOV width = height, pixels'],
  ['frames', 1000, 'number of frames'],
  ['exposure-ms', 50, 'frame duration, ms (simulated time per frame)'],
  ['start-sec', 0, 'simulated time of the first frame, s'],
  ['pixel-nm', 100, 'pixel size, nm'],
  ['background-per-sec', 0, 'Background_BackgroundPhotonsPerSec (photons/pixel/s at the camera, x the QE at the emission filter centre)'],
  ['na', 1.4, 'PSFParam_PsfNa: numerical aperture'],
  ['focus-um', 0, 'SimType_CellFieldFocusHeightUm (focus offset added to z)'],
  ['z-range-um', 7.0, 'SimType_CellFieldZRangeUm: dyes within +/- z-range/2 of the focal plane are rendered (0 = all)'],
  // ---- the microtubules' label (SimType_CellFieldMicrotubule*, FluoParam_Microtubule_*) ----
  ['mt-dye', idx(DYE_CHOICES, 'ATTO655'), 'SimType_CellFieldMicrotubuleDye: a library dye or Dye1..Dye3 (names accepted; data/dyes/library.json)'],
  ['mt-mode', -1, 'SimType_CellFieldMicrotubuleLabelMode: -1 = the dye\'s default, 0 dSTORM, 1 PALM, 2 DNA-PAINT, 3 WideField (names accepted)'],
  ['mt-label-pct', -1, 'SimType_CellFieldMicrotubuleLabelingPct: % of the binding sites (13 x 8 nm lattice, 1625 /um) that carry a label; -1 = the suggestion of the mode (data/dyes suggestedLabelingPct: DNA-PAINT 70, dSTORM 3, PALM 5, WideField 70)'],
  ['mt-imager-nm', DEFAULT_IMAGER_NM, 'SimType_CellFieldMicrotubuleImagerNm: DNA-PAINT imager concentration, nM (binding rate k_on x c; the free imager adds a uniform background -- taken as constant: no depletion by binding or bleaching, no exclusion from cells)'],
  ['mt-orient', 0, 'SimType_CellFieldMicrotubuleOrientation: 0 Free (isotropic), 1 Fixed, 2 Random (no effect on the image yet)'],
  ['mt-orient-polar-deg', 90, 'SimType_CellFieldMicrotubuleOrientPolarDeg: Fixed dipole angle from the microtubule axis'],
  ['mt-orient-azimuth-deg', 0, 'SimType_CellFieldMicrotubuleOrientAzimuthDeg: Fixed dipole azimuth about the axis, from the radial direction'],
  ['mt-wobble-deg', 0, 'SimType_CellFieldMicrotubuleWobbleConeDeg: fast wobble cone half-angle (Fixed, Random)'],
  ['mt-motion', 0, 'SimType_CellFieldMicrotubuleMotion: 0 Static (single-particle tracking: future)'],
  // ---- dye slots (FluoParam_DyeN_*): a library dye + overrides, chosen by mt-dye = Dye1..Dye3 ----
  ['dye1.source', idx(DYE_IDS, 'AF647'), 'FluoParam_Dye1_Source: library dye of slot 1 (dye1.<field> overrides it)'],
  ['dye2.source', idx(DYE_IDS, 'mEos3.2'), 'FluoParam_Dye2_Source'],
  ['dye3.source', idx(DYE_IDS, 'mEGFP'), 'FluoParam_Dye3_Source'],
  // ---- light path (Optics_*) ----
  ...LASER_LINES.map(nm => [`laser-${nm}`, nm === 640 ? DEFAULT_LASER_640_KW : 0, `Optics_Laser${nm}KWcm2: ${nm} nm laser intensity at the sample, kW/cm^2 (0 = off)`]),
  ['laser-custom-nm', 0, 'Optics_LaserCustomNm: wavelength of an extra laser line, nm (0 = none)'],
  ['laser-custom', 0, 'Optics_LaserCustomKWcm2: its intensity, kW/cm^2'],
  ['light-preset', -1, 'Optics_Preset: -1 = none (the laser/dichroic/filter options as given); auto = the light preset of the first structure\'s dye in its mode; or a preset name/index (data/dyes/light_path.json presets). A preset sets every laser-*, dichroic and em-filter the spec does not give.'],
  ['illum-geometry', 0, 'Optics_IlluminationGeometry: 0 Epi (TIRF/HILO: future)'],
  ['chamber-height-um', 5, 'Optics_ChamberHeightUm: imager solution depth that adds to the DNA-PAINT background (Epi: the whole chamber; small by default, standing in for HILO/TIRF)'],
  ['dichroic', idx(DICHROIC_IDS, DYE_DATA.lightPathDefaults.dichroic), 'Optics_Dichroic: reflects the lasers (R = 1 - T), transmits the emission (names accepted)'],
  ['dichroic-edge-nm', 650, 'Optics_DichroicEdgeNm: the Custom dichroic\'s long-pass edge'],
  ['em-filter', idx(EMISSION_FILTER_IDS, DYE_DATA.lightPathDefaults.emissionFilter), 'Optics_EmissionFilter (names accepted)'],
  ['em-lo-nm', 657.5, 'Optics_EmissionLoNm: Custom band pass, low edge'],
  ['em-hi-nm', 694.5, 'Optics_EmissionHiNm: Custom band pass, high edge'],
  ['chunk-um', 26, 'SimType_CellFieldChunkSizeUm'],
  ['occupancy', 0.33, 'SimType_CellFieldOccupancy'],
  ['cell-diam-min-um', 25, 'SimType_CellFieldCellDiameterMinUm'],
  ['cell-diam-max-um', 35, 'SimType_CellFieldCellDiameterMaxUm'],
  ['mt-density', 0.9, 'SimType_CellFieldMicrotubuleDensityPerUm2'],
  ['packing', 1, 'SimType_CellFieldPacking (1 on, 0 off)'],
  // ---- camera (CamParam_*): a preset sets every value below that the spec does not give ----
  ['camera-preset', idx(CAMERA_IDS, DYE_DATA.cameraDefault), 'CamParam_CameraPreset: Kinetix22, iXonUltra897, Custom (names accepted; data/dyes/cameras.json)'],
  ['qe-curve', -1, 'CamParam_QeCurve: QE(lambda) of a camera (index/name), -1 = the preset\'s, Custom = flat at qe'],
  ['qe', 0.85, 'CamParam_QuantumEfficiency (the flat QE of the Custom curve)'],
  ['camera-type', -1, 'CamParam_CameraType: -1 = the preset\'s, 0 sCMOS, 1 EMCCD'],
  ['dark-per-sec', 1.03, 'CamParam_DarkCurrentElectronsPerSec'],
  ['gain', 0.25, 'CamParam_GainPhotonsPerADU (electrons per ADU)'],
  ['offset', 100, 'CamParam_OffsetADU'],
  ['offset-std', 0.5, 'CamParam_OffsetStdADU'],
  ['read-noise', 1.2, 'CamParam_ReadNoiseElectrons'],
  ['gain-std-pct', 0.5, 'CamParam_GainStdPctPerPixel (per-pixel gain spread, PRNU)'],
  ['read-noise-std-pct', 20, 'CamParam_ReadNoiseStdPctPerPixel'],
  ['em-gain', 300, 'CamParam_EmGain (EMCCD)'],
  ['cic', 0.002, 'CamParam_CicElectrons (EMCCD clock-induced charge, e-/pixel/frame)'],
  ['bit-depth', 16, 'CamParam_BitDepth (EMCCD)'],
  ['modality', 0, 'General_ImagingModality: 0 = Fluorescence (every label in its mode), 1 = BrightField (transmitted light; names accepted)'],
  ['wf-upscale', 1, 'General_WideFieldUpscaling: mean-field grid cells per pixel, per axis (1-4)'],
  ['wf-plane-nm', 25, 'General_WideFieldZPlaneNm: mean-field dye plane thickness, nm'],
  ['wf-kernel-um', 7, 'mean-field PSF kernel radius cap, um'],
  ['mean-field-density-per-um2', 20, 'General_MeanFieldDensityPerUm2: a continuous population (WideField dyes, pre states, dSTORM initial ON) renders mean-field above this many emitting dyes per um^2 of the focal slab, per dye below'],
  ['mean-field-slab-nm', 500, 'General_MeanFieldSlabNm: that slab\'s thickness around the focal plane'],
  ['mean-field-max-emitters', 5000, 'General_MeanFieldMaxEmitters: and mean-field above this many emitting dyes in the z range (cost cap of the per-dye path)'],
  ['bf-quality', 3, 'General_BrightFieldQuality: speed vs precision, 1 (fast) .. 4 (precise); sets the four below unless given'],
  ['bf-sources', 0, 'General_BrightFieldSources: condenser source points (0 = from bf-quality: 6/12/24/48)'],
  ['bf-upscale', 0, 'General_BrightFieldUpscaling: optical grid cells per pixel, per axis (a minimum, raised to keep the grid pitch <= lambda / 4n; 0 = from bf-quality: 1)'],
  ['bf-sub', 0, 'General_BrightFieldGeometrySamples: geometry samples per grid cell side (0 = from bf-quality: 1/1/2/2)'],
  ['bf-slice-um', -1, 'General_BrightFieldSliceUm: multislice step, um; 0 = one thin slice (-1 = from bf-quality: 0/0.5/0.5/0.25)'],
  ['bf-margin-um', 0, 'BrightField grid margin around the FOV, um (0 = from bf-quality: 3-5)'],
  ['bf-condenser-na', 0.4, 'General_BrightFieldCondenserNa: illumination NA (0 = coherent)'],
  ['bf-wavelength-nm', 550, 'General_BrightFieldWavelengthNm: illumination wavelength (the camera QE is read there)'],
  ['bf-photons-per-px-per-sec', 80000, 'General_BrightFieldPhotonsPerPxPerSec: empty-field photons per pixel per second'],
  ['bf-aberrations', 1, 'General_BrightFieldAberrations: 1 = the PSF\'s Zernike aberrations in the detection pupil, 0 = none'],
  ['bf-n-medium', 1.337, 'SimType_CellFieldIndexMedium: refractive index of the medium'],
  ['bf-n-cytoplasm', 1.35, 'SimType_CellFieldIndexCytoplasm'],
  ['bf-n-nucleus', 1.35, 'SimType_CellFieldIndexNucleus'],
  ['bf-n-microtubule', 1.48, 'SimType_CellFieldIndexMicrotubule (12.5 nm tubes)'],
  ['bf-absorption-per-um', 0, 'SimType_CellFieldAbsorptionPerUm: intensity absorption of cell material, 1/um (unstained: 0)'],
  ['immersion-index', 1.518, 'PSFParam_PsfImmersionIndex (PSF and collection efficiency)'],
  ['psf-model', 3, 'PSFParam_PsfModel: 0 = Gaussian, 3 = GibsonLanniZernike (names accepted; 1/2 need the adapter\'s JVM)'],
  ['psf-zernike-preset', 9, 'PSFParam_PsfZernikePreset: index or name (0 None ... 9 MixedRealisticObjective ... 12)'],
  ['psf-mask', 0, 'PSFParam_PsfMaskType: 0 = None, 1 = DoubleHelix (names accepted)'],
  ['psf-mask-modes', 5, 'PSFParam_PsfMaskModes: double-helix Gauss-Laguerre modes (2-8)'],
  ['psf-mask-waist', 1.0, 'PSFParam_PsfMaskWaist: double-helix waist, pupil radii'],
  ['psf-oversampling', 6, 'PSFParam_PsfOversampling: kernel samples per camera pixel, per axis (1-16)'],
  ['psf-kernel-half-width-nm', 7000, 'PSFParam_PsfKernelHalfWidthNm (a minimum: grown to 3x the Rayleigh radius)'],
  ['psf-z-range-um', 7.0, 'PSFParam_PsfZRangeUm: span of the PSF z stack'],
  ['psf-z-step-um', 0.1, 'PSFParam_PsfZStepUm: PSF z plane spacing'],
  ['psf-sample-index', 1.518, 'PSFParam_PsfSampleIndex: sample refractive index (Gibson-Lanni)'],
  ['psf-working-distance-um', 150, 'PSFParam_PsfWorkingDistanceUm (Gibson-Lanni ti0)'],
  ['psf-sample-depth-nm', 0, 'PSFParam_PsfSampleDepthNm: emitter depth below the coverslip (Gibson-Lanni)'],
  ['psf-interp', 2, 'PSFParam_PsfInterp: 0 Nearest, 1 Linear, 2 Cubic, 3 Fft (names accepted)'],
];
// Per-structure dye overrides `<prefix>-dye.<field>` and slot overrides `dye<N>.<field>` (DYE_FIELDS keys).
export const DYE_FIELD_NAMES = Object.keys(DYE_FIELDS);
const DEFAULTS = Object.fromEntries(SCOPE_OPTIONS.map(([k, v]) => [k, v]));
const NAMES = {
  modality: ['Fluorescence', 'BrightField'], 'psf-model': ['Gaussian', 'RichardsWolf', 'GibsonLanni', 'GibsonLanniZernike'],
  'psf-mask': ['None', 'DoubleHelix'], 'psf-interp': ['Nearest', 'Linear', 'Cubic', 'Fft'], 'psf-zernike-preset': ZERNIKE_PRESETS,
  'mt-dye': DYE_CHOICES, 'mt-mode': MODES, 'mt-orient': ORIENTATION_MODES, 'mt-motion': MOTION_MODES,
  'dye1.source': DYE_IDS, 'dye2.source': DYE_IDS, 'dye3.source': DYE_IDS,
  dichroic: DICHROIC_IDS, 'em-filter': EMISSION_FILTER_IDS, 'light-preset': LIGHT_PRESET_CHOICES, 'camera-preset': CAMERA_IDS, 'qe-curve': CAMERA_IDS,
  'camera-type': ['sCMOS', 'EMCCD'], 'illum-geometry': ['Epi'],
};
const DYE_OVERRIDE = /^(?:([a-z]+)-dye|dye([1-3]))\.([a-z0-9-]+)$/;

// "k=v k=v" (spaces, commas, semicolons) or an object -> spec object. Names accepted where the C++ does.
export function parseSpec(spec) {
  const out = {};
  const set = (k, v) => {
    const ov = DYE_OVERRIDE.exec(k);
    if (ov && !(k in DEFAULTS) && (!DYE_FIELDS[ov[3]] || (ov[1] && !STRUCTURES.some(s => s.prefix === ov[1]))))
      throw new Error(`bad option '${k}' (dye fields: ${DYE_FIELD_NAMES.join(', ')})`);
    if (!(k in DEFAULTS) && !ov && !/^p\.\w+$/.test(k) && !/^zern\.\d{1,2}$/.test(k)) throw new Error(`bad option '${k}'`);
    let x = typeof v === 'number' ? v : (String(v).trim() === '' ? NaN : Number(v));
    if (Number.isNaN(x) && NAMES[k]) { const i = NAMES[k].indexOf(String(v)); x = i >= 0 ? i : NaN; }
    if (Number.isNaN(x)) throw new Error(`bad option '${k}=${v}'`);
    out[k] = x;
  };
  if (typeof spec === 'string') {
    for (const tok of spec.split(/[\s,;]+/).filter(Boolean)) {
      const i = tok.indexOf('=');
      if (i < 0) throw new Error(`bad option '${tok}'`);
      set(tok.slice(0, i), tok.slice(i + 1));
    }
  } else for (const [k, v] of Object.entries(spec || {})) set(k, v);
  return out;
}
export const specToString = spec => Object.entries(spec).map(([k, v]) => `${k}=${v}`).join(' ');
const getter = spec => n => (n in spec ? spec[n] : (DEFAULTS[n] ?? 0));

export function scopeDims(spec) {
  const O = getter(spec);
  const w = Math.min(2048, Math.max(1, O('size'))) >>> 0;
  const frames = O('prepare') >= 1 ? 0 : Math.trunc(Math.min(100000, Math.max(1, O('frames'))));
  return { width: w, height: w, frames };
}

// The camera of a spec: the preset's values for every option the spec does not set (CameraPreset).
const CAMERA_KEYS = { qe: 'quantumEfficiency', 'dark-per-sec': 'darkCurrentElectronsPerSec', gain: 'gainElectronsPerAdu',
  offset: 'offsetAdu', 'offset-std': 'offsetStdAdu', 'read-noise': 'readNoiseElectrons', 'gain-std-pct': 'gainStdPct',
  'read-noise-std-pct': 'readNoiseStdPct', 'em-gain': 'emGain', cic: 'cicElectrons', 'bit-depth': 'bitDepth' };
export function scopeCamera(spec) {
  const O = getter(spec), preset = cameraPreset(Math.trunc(O('camera-preset')));
  const C = n => (n in spec || preset[CAMERA_KEYS[n]] === undefined ? O(n) : preset[CAMERA_KEYS[n]]);
  const type = O('camera-type') >= 0 ? O('camera-type') : (preset.type === 'EMCCD' ? 1 : 0);
  const qeCurve = O('qe-curve') >= 0 ? Math.trunc(O('qe-curve')) : CAMERA_IDS.indexOf(preset.id);
  return { preset: preset.id, qeCurve, qeFlat: C('qe'), emccd: type === 1, darkPerSec: C('dark-per-sec'),
    gainPhotonsPerAdu: C('gain'), offsetAdu: C('offset'), offsetStdAdu: C('offset-std'), readNoiseElectrons: C('read-noise'),
    gainStdFraction: C('gain-std-pct') / 100.0, readNoiseStdFraction: C('read-noise-std-pct') / 100.0,
    emGain: C('em-gain'), cicElectrons: C('cic'), bitDepth: C('bit-depth') };
}

// The light preset a spec asks for (LightPreset): null, or {lasers: {nm: kW}, dichroic, emissionFilter}.
export function scopeLightPreset(spec) {
  const O = getter(spec), i = Math.trunc(O('light-preset'));
  if (i < 0) return null;
  let id = LIGHT_PRESET_CHOICES[i];
  if (id === 'auto') {
    const { slots, byStructure } = dyeOverrides(spec), P = STRUCTURES[0].prefix;
    const eff = effectiveDye(Math.trunc(O(`${P}-dye`)), slots, byStructure[P], Math.trunc(O(`${P}-mode`)));
    id = eff.dye.modes[eff.mode].lightPreset;
  }
  const q = DYE_DATA.lightPresets.find(x => x.id === id);
  if (!q) throw new Error(`light preset ${i} out of range`);
  return q;
}
// The light path of a spec (LightPath): the light preset's values for the options the spec does not give.
export function scopeLightPath(spec, camera = scopeCamera(spec)) {
  const q = scopeLightPreset(spec);
  if (q) {
    spec = { ...spec };
    for (const nm of LASER_LINES) if (!(`laser-${nm}` in spec)) spec[`laser-${nm}`] = q.lasers[nm] ?? 0;
    if (!('dichroic' in spec)) spec.dichroic = DICHROIC_IDS.indexOf(q.dichroic);
    if (!('em-filter' in spec)) spec['em-filter'] = EMISSION_FILTER_IDS.indexOf(q.emissionFilter);
  }
  const O = getter(spec);
  const lasers = LASER_LINES.map(nm => ({ nm, kWPerCm2: Math.max(0, O(`laser-${nm}`)) }));
  if (O('laser-custom-nm') > 0) lasers.push({ nm: O('laser-custom-nm'), kWPerCm2: Math.max(0, O('laser-custom')) });
  return makeLightPath({ lasers, dichroic: Math.trunc(O('dichroic')), dichroicEdgeNm: O('dichroic-edge-nm'),
    emissionFilter: Math.trunc(O('em-filter')), emLoNm: O('em-lo-nm'), emHiNm: O('em-hi-nm'), qeCurve: camera.qeCurve,
    qeFlat: camera.qeFlat, na: O('na'), immersionIndex: O('immersion-index'), chamberHeightUm: Math.max(0, O('chamber-height-um')) });
}

// Dye overrides of a spec: { slots: [{source, overrides}] x 3, structure prefix -> overrides }.
function dyeOverrides(spec) {
  const O = getter(spec);
  const slots = [1, 2, 3].map(n => ({ source: Math.trunc(O(`dye${n}.source`)), overrides: {} }));
  const byStructure = Object.fromEntries(STRUCTURES.map(s => [s.prefix, {}]));
  for (const [k, v] of Object.entries(spec)) {
    const m = DYE_OVERRIDE.exec(k);
    if (!m || m[3] === 'source') continue;
    if (m[2]) slots[+m[2] - 1].overrides[m[3]] = v; else byStructure[m[1]][m[3]] = v;
  }
  return { slots, byStructure };
}

// Per structure: its effective dye, mode, world label and photophysics (labelPhotophysics).
export function scopeLabels(spec, lp) {
  const O = getter(spec), { slots, byStructure } = dyeOverrides(spec);
  return STRUCTURES.map(s => {
    const P = s.prefix;
    const eff = effectiveDye(Math.trunc(O(`${P}-dye`)), slots, byStructure[P], Math.trunc(O(`${P}-mode`)));
    const pct = O(`${P}-label-pct`) >= 0 ? O(`${P}-label-pct`) : DYE_DATA.suggestedLabelingPct[eff.mode];
    return labelPhotophysics(eff, lp, {
      density: Math.min(1, Math.max(0, pct / 100)), imagerNm: O(`${P}-imager-nm`),
      orientation: { mode: ORIENTATION_MODES[Math.trunc(O(`${P}-orient`))], polarDeg: O(`${P}-orient-polar-deg`),
        azimuthDeg: O(`${P}-orient-azimuth-deg`), wobbleDeg: O(`${P}-wobble-deg`) },
      motion: MOTION_MODES[Math.trunc(O(`${P}-motion`))] });
  });
}

// MakeScopeSetup: frame-equivalent parameters, world settings, the FOV query, the camera, light path and labels.
export function scopeSetup(P, spec) {
  const O = getter(spec);
  const seed = Math.trunc(O('seed'));
  const { width: W, height: H, frames: N } = scopeDims(spec);
  const expSec = Math.max(1e-6, O('exposure-ms') / 1000.0), t0Sec = Math.max(0.0, O('start-sec'));
  const pixelSizeNm = O('pixel-nm');
  const camera = scopeCamera(spec);
  const lp = scopeLightPath(spec, camera);
  const brightField = O('modality') === 1;
  const labels = scopeLabels(spec, lp);
  // Fluorescence: the photon image is already in detected photons (QE(lambda) in each dye's detected fraction), so
  // the noise chain runs at QE 1; the flat background takes the QE at the emission filter's centre. BrightField:
  // the QE at its lamp wavelength.
  const bgQe = backgroundQe(lp);
  const p = { pixelSizeNm, backgroundPhotons: O('background-per-sec') * expSec * bgQe, frameDurationSec: expSec,
    na: O('na') };
  const cam = {
    quantumEfficiency: brightField ? sampleAt(lp.qe, O('bf-wavelength-nm')) : 1.0,
    darkCurrentElectrons: camera.darkPerSec * expSec, gainPhotonsPerAdu: camera.gainPhotonsPerAdu,
    offsetAdu: camera.offsetAdu, offsetStdAdu: camera.offsetStdAdu, readNoiseElectrons: camera.readNoiseElectrons,
    gainStdFraction: camera.gainStdFraction, readNoiseStdFraction: camera.readNoiseStdFraction,
    emccd: camera.emccd, emGain: camera.emGain, cicElectrons: camera.cicElectrons, bitDepth: camera.bitDepth,
  };
  const ws = O('world-seed');
  const worldSeed = ws >= 0 ? Number(BigInt.asUintN(32, BigInt(Math.trunc(ws))))
    : Number(BigInt.asUintN(32, BigInt(seed) ^ 0x43454C4Cn));
  // Named world options, then p.* (prototype names) on top; the rest are the prototype's defaults.
  const vals = { ...P.defaults, chunkSize: O('chunk-um'), density: O('occupancy'), cellDiamMin: O('cell-diam-min-um'),
    cellDiamMax: O('cell-diam-max-um'), mtDensity: O('mt-density'), enablePacking: O('packing') !== 0 };
  for (const [k, v] of Object.entries(spec)) {
    if (!k.startsWith('p.')) continue;
    const name = k.slice(2);
    if (name in P.defaults) vals[name] = typeof P.defaults[name] === 'boolean' ? v !== 0 : v;
  }
  const worldParams = P.paramsFrom(vals);
  const um = pixelSizeNm / 1000.0, margin = 2.0;
  const originXUm = O('x') - W * um / 2, originYUm = O('y') - H * um / 2;
  const q = {
    originXUm, originYUm, x0Um: originXUm - margin, x1Um: originXUm + W * um + margin,
    y0Um: originYUm - margin, y1Um: originYUm + H * um + margin,
    zRefUm: O('focus-um'), zCullCentreUm: O('focus-um') + O('z'), zHalfRangeUm: Math.max(0.0, O('z-range-um')) / 2,
    frameSec: expSec, tSec: t0Sec, spanSec: N * expSec, frameIndex: 0,
  };
  return { O, seed, W, H, N, expSec, t0Sec, p, cam, camera, lp, labels, worldSeed, worldParams, q,
    meanField: { densityPerUm2: Math.max(0, O('mean-field-density-per-um2')), slabNm: Math.max(0, O('mean-field-slab-nm')),
      maxEmitters: Math.max(0, O('mean-field-max-emitters')) } };
}

// The PSF request of a spec at an emission wavelength (ScopePsfRequest), or null for the Gaussian.
export function scopePsfRequest(spec, wavelengthNm) {
  const O = getter(spec);
  const model = Math.trunc(O('psf-model'));
  if (model === 0) return null;
  if (model !== 3) throw new Error(`psf-model ${model}: only 0 (Gaussian) and 3 (GibsonLanniZernike) run here`);
  const preset = Math.trunc(O('psf-zernike-preset'));
  if (preset < 0 || preset >= ZERNIKE_PRESETS.length) throw new Error(`psf-zernike-preset ${preset} out of range`);
  const zernike = zernikePresetCoefficients(ZERNIKE_PRESETS[preset]);
  for (const [k, v] of Object.entries(spec)) {
    const m = /^zern\.(\d+)$/.exec(k);
    if (m && +m[1] < NUM_ZERNIKE) zernike[+m[1]] = v;
  }
  const zStepUm = Math.max(O('psf-z-step-um'), 0.001);
  const pixelSizeNm = O('pixel-nm'), na = O('na');
  return {
    wavelengthNm, na, immersionIndex: O('immersion-index'), pixelSizeNm,
    oversampling: Math.trunc(Math.min(16, Math.max(1, O('psf-oversampling')))),
    kernelHalfWidthPx: psfKernelHalfWidthPx(Math.min(20000, Math.max(100, O('psf-kernel-half-width-nm'))), pixelSizeNm, wavelengthNm, na),
    nz: Math.round(Math.max(0, O('psf-z-range-um')) / zStepUm) + 1, zStepNm: zStepUm * 1000.0,
    sampleIndex: O('psf-sample-index'), workingDistanceUm: O('psf-working-distance-um'), sampleDepthNm: O('psf-sample-depth-nm'),
    zernike, maskType: O('psf-mask') === 1 ? 1 : 0, maskModes: Math.trunc(Math.min(8, Math.max(2, O('psf-mask-modes')))),
    maskWaist: O('psf-mask-waist'), interpMode: Math.trunc(Math.min(3, Math.max(0, O('psf-interp')))),
  };
}
// A PSF wavelength rounded to 2 nm (< 0.3 % in PSF width), so small light-path or dye changes reuse a kernel.
export const kernelWavelengthNm = lambdaNm => 2 * Math.round(lambdaNm / 2);

// ---- caches (speed only; same answers) ----
const kernelMemo = [];  // keyed by everything but interpMode; holds the movie's kernel groups + 1
let worldMemo = null;
export function scopeKernel(spec, wavelengthNm, onPlane, keep = 2) {
  const req = scopePsfRequest(spec, wavelengthNm);
  if (!req) return null;
  const key = JSON.stringify({ ...req, interpMode: 0 });
  let i = kernelMemo.findIndex(e => e.key === key), hit;
  if (i < 0) hit = { key, cache: buildZernikeKernelCache(req, onPlane) };
  else hit = kernelMemo.splice(i, 1)[0];
  kernelMemo.unshift(hit);
  kernelMemo.length = Math.min(kernelMemo.length, Math.max(2, keep));
  return { ...hit.cache, interpMode: req.interpMode };
}
// The labels enter only the dye draw and the schedules (World.setLabels): a change of those alone keeps the world's
// cells and microtubules.
export function scopeWorld(P, S) {
  const geomKey = JSON.stringify([S.worldSeed, S.worldParams]);
  const labels = S.labels.map(l => l.label);
  if (!worldMemo || worldMemo.P !== P || worldMemo.geomKey !== geomKey)
    worldMemo = { P, geomKey, world: new World(P, S.worldSeed, S.worldParams, labels) };
  else if (JSON.stringify(worldMemo.world.labels) !== JSON.stringify(labels)) worldMemo.world.setLabels(labels);
  return worldMemo.world;
}

// CellFieldSource::Events: world events of q as BlinkEvents in the FOV frame (+ structure, state, aux).
const toFov = (q, e) => ({ xUm: e.x - q.originXUm, yUm: e.y - q.originYUm, zNm: (e.z - q.zRefUm) * 1000.0,
  tStart: q.frameIndex + (e.tOn - q.tSec) / q.frameSec, tEnd: q.frameIndex + (e.tOff - q.tSec) / q.frameSec,
  brightness: e.brightness, structure: e.structure, state: e.state, aux: e.aux });
const zWindow = q => [q.zHalfRangeUm > 0 ? q.zCullCentreUm - q.zHalfRangeUm : -Infinity,
  q.zHalfRangeUm > 0 ? q.zCullCentreUm + q.zHalfRangeUm : Infinity];
export function cellFieldEvents(world, q) {
  const [zMin, zMax] = zWindow(q);
  return world.eventsInWindow(q.x0Um, q.y0Um, q.x1Um, q.y1Um, zMin, zMax, q.tSec, q.tSec + q.spanSec).map(e => toFov(q, e));
}
export function cellFieldContinuous(world, q) {
  const [zMin, zMax] = zWindow(q);
  return world.continuousInWindow(q.x0Um, q.y0Um, q.x1Um, q.y1Um, zMin, zMax).map(e => toFov(q, e));
}

// RenderScopeMovie: onFrame(f, Uint16Array, photons) for every frame (return false to stop). Returns info.
// P: loadPrototype() instance (the geometry truth). opts.onProgress(stage, frac) optional.
export function renderScopeMovie(P, specIn, onFrame, opts = {}) {
  const spec = parseSpec(specIn);
  const t0 = performance.now();
  const S = scopeSetup(P, spec);
  if (S.O('prepare') >= 1) {
    // The world (scopeWorld's memo) and, for Fluorescence, the labels' PSF kernels; no frames (PrepareScope).
    scopeWorld(P, S);
    const tWorld = (performance.now() - t0) / 1000;
    const kernels = S.O('modality') === 1 ? [] : S.labels.flatMap(l => Object.values(l.states))
      .filter(st => st && st.detectedFraction > 0).map(st => scopeKernel(spec, kernelWavelengthNm(st.lambdaNm)));
    return { width: S.W, height: S.H, frames: 0, blinks: 0, querySec: tWorld, totalSec: (performance.now() - t0) / 1000,
      psf: kernels.some(Boolean) ? 'GibsonLanniZernike' : 'Gaussian' };
  }
  if (S.O('modality') === 1) return renderBrightfieldMovie(P, spec, S, onFrame, opts);
  return renderFluorescenceMovie(P, spec, S, onFrame, opts);
}
