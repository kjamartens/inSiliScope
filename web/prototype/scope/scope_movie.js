// A cell-field movie in JS: the mirror of adapter/inSiliScope/Simulation/ScopeMovie.cpp (+ CellFieldSource),
// i.e. what insiliscope_cli / the viewer's isc_scope_movie render, option for option:
//   world (seed, p.* geometry, one label per structure) -> modality 0 Fluorescence (fluorescence.js: every label in
//   its mode -- blinks splatted with their own PSF, continuous populations mean-field or per dye, the DNA-PAINT imager
//   background -- then ApplyNoiseChain) or modality 1 BrightField (brightfield.js); light-epi / light-trans (the
//   shutters) both on: the two summed before one noise chain, both off: dark frames.
// Issue 16: dyes (dye_library.js, data/dyes) on structures, a light path (lasers, dichroic, emission filter, camera QE
// curve) and camera presets replace the single dye and the SuperRes/WideField split.
// This is the JS REFERENCE for imaging. Iterate on photophysics/PSF/camera here (web/lab), then port to
// the C++ files named in each module's header when merging (web/lab/README.md, tests/parity/scope_parity.mjs).
import { World, STRUCTURES } from './world.js';
import { ZERNIKE_PRESETS, zernikePresetCoefficients, psfKernelHalfWidthPx, buildZernikeKernelCache, NUM_ZERNIKE, planSplat,
  splatRows } from './psf.js';
import { renderGaussian, noiseMaps, applyNoiseChain } from './render.js';
import { renderBrightfieldMovie, brightfieldPhotons } from './brightfield.js';
import { driftOn, driftTrajectory, driftRange } from './drift.js';
import { renderFluorescenceMovie } from './fluorescence.js';
import { DYE_DATA, DYE_IDS, DYE_CHOICES, DYE_FIELDS, MODES, EXCITATION_FILTER_IDS, DICHROIC_IDS, EMISSION_FILTER_IDS, CAMERA_IDS, LASER_LINES,
  effectiveDye, makeLightPath, labelPhotophysics, cameraPreset, cameraPresetGain, cameraPreamp, emGainFromGain,
  backgroundQe } from './dye_library.js';
import { ORIENTATION_MODES, MOTION_MODES } from './dyes.js';
import { sampleAt } from './spectra.js';

const LIGHT_PRESET_CHOICES = ['auto', ...DYE_DATA.lightPresets.map(q => q.id)];
export { LIGHT_PRESET_CHOICES };
const idx = (list, id) => { const i = list.indexOf(id); if (i < 0) throw new Error(`no '${id}'`); return i; };
// The default 640 nm intensity, the DNA-PAINT presets' 1 kW/cm^2 (estimate; was 0.1607 until 2026-10-05, what gave
// ATTO 655 the single-dye default's 6375 photoelectrons/s while ON; scope/dye_library.js, issue 16).
export const DEFAULT_LASER_640_KW = 1.0;
// k_on 1e6 /M/s x 1 nM = 1e-3 bindings per site per second (*estimate*, 2026-10-07: was 1.43, the former default
// activation rate; 1 nM gives about the emitters per frame of the dSTORM and PALM typical labels).
export const DEFAULT_IMAGER_NM = 1;

// [name, default, help]: ScopeMovieOptions(), same order and defaults.
export const SCOPE_OPTIONS = [
  ['seed', 42, 'Hub.RandomSeed (cell field = seed ^ 0x43454C4C unless world-seed >= 0; noise as the adapter)'],
  ['world-seed', -1, 'cell-field world seed used as is (the viewer\'s seed); -1 = derive it from seed'],
  ['disk-cache', 1, 'per-user cache on disk (the C++ hosts; no file here): 0 = none, 1 = the packed cell positions, 2 = also the PSF kernel'],
  ['prepare', 0, '1 = build the world and the PSF kernel only (warms the caches), no frames'],
  ['x', 0, 'FOV centre x, world um (XY stage position)'],
  ['y', 0, 'FOV centre y, world um'],
  ['z', 0.5, 'Z stage: focal-plane height above the coverslip, um (as the ZStage device; it starts at 0.5)'],
  ['size', 128, 'FOV width = height, pixels'],
  ['frames', 1000, 'number of frames'],
  ['exposure-ms', 50, 'frame duration, ms (simulated time per frame)'],
  ['start-sec', 60, 'simulated time of the first frame after the illumination starts, s (60: past the dSTORM initial ON phase, near steady state)'],
  ['drift-xy-speed-nm-per-sec', 0, 'SampleHolder.DriftXySpeedNmPerSec: directed sample drift, mean xy speed, nm/s (0 = none)'],
  ['drift-z-speed-nm-per-sec', 0, 'SampleHolder.DriftZSpeedNmPerSec: directed sample drift, mean z speed, nm/s (its direction: drift-z-direction)'],
  ['drift-xy-angle-deg', -1, 'SampleHolder.DriftXyAngleDeg: direction of the xy drift, deg from +x (-1 = random per seed; advanced)'],
  ['drift-z-direction', 0, 'SampleHolder.DriftZDirection: direction of the z drift, 1 = away from the coverslip, -1 = towards it, 0 = random per seed (advanced)'],
  ['drift-xy-angle-wander-deg', 180, 'SampleHolder.DriftXyAngleWanderDeg: the xy direction swings slowly within +/- this, deg (180 = any direction; advanced)'],
  ['drift-z-angle-wander-deg', 90, 'SampleHolder.DriftZAngleWanderDeg: the z drift swings within +/- this, deg: speed x cos(angle), 90 = between full speed and still, 180 = also back (advanced)'],
  ['drift-speed-wander-pct', 0, 'SampleHolder.DriftSpeedWanderPct: how much the xy and z drift strengths fluctuate, % RMS of the mean (advanced)'],
  ['drift-wander-time-sec', 60, 'SampleHolder.DriftWanderTimeSec: how slowly direction and strength wander (correlation time), s (advanced)'],
  ['drift-xy-nm-per-sqrt-sec', 0, 'SampleHolder.DriftXyNmPerSqrtSec: random-walk drift on top, RMS nm per axis after 1 s (advanced)'],
  ['drift-z-nm-per-sqrt-sec', 0, 'SampleHolder.DriftZNmPerSqrtSec: random-walk drift in z, RMS nm after 1 s (advanced)'],
  ['pixel-nm', 100, 'pixel size, nm'],
  ['background-per-sec', 0, 'SampleHolder.BackgroundPhotonsPerSec (photons/pixel/s at the camera, x the QE at the emission filter centre)'],
  ['na', 1.4, 'Objective.NA: numerical aperture'],
  ['focus-um', 0, 'CellField.FocusHeightUm (focus offset added to z)'],
  ['z-range-um', 7.0, 'CellField.ZRangeUm: dyes within +/- z-range/2 of the focal plane are rendered (0 = all)'],
  // ---- the microtubules' label (Micro-Manager: CellField.Microtubules_*, Fluorophores) ----
  ['specimen', 0, 'SampleHolder: the mounted specimen (0 = CellField; data/specimens.json; names accepted)'],
  ['mode', -1, 'Fluorophores Mode: the experiment\'s label mode, for every target whose <prefix>-mode is -2 (Global): 0 dSTORM, 1 PALM, 2 DNA-PAINT, 3 WideField; -1 None (each target\'s own; names accepted)'],
  ['mt-dye', idx(DYE_CHOICES, 'ATTO655'), 'CellField Microtubules_Label: a library dye or Dye1..Dye3; -1 Typical = the microtubules\' typical dye in their mode (data/dyes/library.json typicalLabels; names accepted)'],
  ['mt-mode', -1, 'CellField Microtubules_Mode: -1 DyeDefault = the dye\'s default (Typical: the library\'s default mode), -2 Global = the mode option, 0 dSTORM, 1 PALM, 2 DNA-PAINT, 3 WideField (names accepted)'],
  ['mt-label-pct', -1, 'CellField Microtubules_LabelingPct: % of the binding sites (13 x 8 nm lattice, 1625 /um) that carry a label; -1 = the target\'s typical % in its mode (data/dyes/library.json typicalLabels: DNA-PAINT 70, dSTORM 3, PALM 25, WideField 70)'],
  ['mt-imager-nm', DEFAULT_IMAGER_NM, 'CellField.Microtubules_ImagerNm: DNA-PAINT imager concentration, nM (binding rate k_on x c; the free imager adds a uniform background -- taken as constant: no depletion by binding or bleaching, no exclusion from cells)'],
  ['mt-orient', 0, 'CellField.Microtubules_Orientation: 0 Free (isotropic), 1 Fixed, 2 Random (no effect on the image yet)'],
  ['mt-orient-polar-deg', 90, 'CellField.Microtubules_OrientPolarDeg: Fixed dipole angle from the microtubule axis'],
  ['mt-orient-azimuth-deg', 0, 'CellField.Microtubules_OrientAzimuthDeg: Fixed dipole azimuth about the axis, from the radial direction'],
  ['mt-wobble-deg', 0, 'CellField.Microtubules_WobbleConeDeg: fast wobble cone half-angle (Fixed, Random)'],
  ['mt-motion', 0, 'CellField.Microtubules_Motion: 0 Static (single-particle tracking: future)'],
  // ---- dye slots (Fluorophores.DyeN_*): a library dye + overrides, chosen by mt-dye = Dye1..Dye3 ----
  ['dye1.source', idx(DYE_IDS, 'AF647'), 'Fluorophores.Dye1_Source: library dye of slot 1 (dye1.<field> overrides it)'],
  ['dye2.source', idx(DYE_IDS, 'mEos3.2'), 'Fluorophores.Dye2_Source'],
  ['dye3.source', idx(DYE_IDS, 'mEGFP'), 'Fluorophores.Dye3_Source'],
  // ---- light path (Micro-Manager: Lasers, the filter wheels) ----
  ...LASER_LINES.map(nm => [`laser-${nm}`, nm === 640 ? DEFAULT_LASER_640_KW : 0, `Lasers.Laser${nm}KWcm2: ${nm} nm laser intensity at the sample, kW/cm^2 (0 = off)`]),
  ['laser-custom-nm', 0, 'wavelength of an extra laser line, nm (0 = none; not in Micro-Manager)'],
  ['laser-custom', 0, 'its intensity, kW/cm^2'],
  ['light-preset', -1, 'Lasers.Preset: -1 = none (the laser/dichroic/filter options as given); auto = the light preset of the first structure\'s dye in its mode; or a preset name/index (data/dyes/light_path.json presets). A preset sets every laser-*, ex-filter, dichroic and em-filter the spec does not give.'],
  ['illum-geometry', 0, 'Lasers.IlluminationGeometry: 0 Epi (TIRF/HILO: future)'],
  ['chamber-height-um', 5, 'Lasers.ChamberHeightUm: imager solution depth that adds to the DNA-PAINT background (Epi: the whole chamber; small by default, standing in for HILO/TIRF)'],
  ['ex-filter', idx(EXCITATION_FILTER_IDS, DYE_DATA.lightPathDefaults.excitationFilter), 'ExcitationFilter: laser clean-up filter in front of the dichroic; each laser line is scaled by its transmission there (names accepted)'],
  ['ex-lo-nm', 635, 'ExcitationFilter Custom band pass, low edge'],
  ['ex-hi-nm', 645, 'ExcitationFilter Custom band pass, high edge'],
  ['dichroic', idx(DICHROIC_IDS, DYE_DATA.lightPathDefaults.dichroic), 'Dichroic: reflects the lasers (R = 1 - T), transmits the emission (names accepted)'],
  ['dichroic-edge-nm', 650, 'Dichroic.CustomEdgeNm: the Custom dichroic\'s long-pass edge'],
  ['em-filter', idx(EMISSION_FILTER_IDS, DYE_DATA.lightPathDefaults.emissionFilter), 'EmissionFilter (names accepted)'],
  ['em-lo-nm', 657.5, 'EmissionFilter.CustomLoNm: Custom band pass, low edge'],
  ['em-hi-nm', 694.5, 'EmissionFilter.CustomHiNm: Custom band pass, high edge'],
  ['chunk-um', 26, 'CellField.ChunkSizeUm'],
  ['occupancy', 0.33, 'CellField.Occupancy'],
  ['cell-diam-min-um', 25, 'CellField.CellDiameterMinUm'],
  ['cell-diam-max-um', 35, 'CellField.CellDiameterMaxUm'],
  ['mt-density', 0.9, 'CellField.MicrotubuleDensityPerUm2'],
  ['packing', 1, 'CellField.Packing (1 on, 0 off)'],
  // ---- camera (Micro-Manager: Camera): a preset sets every value below that the spec does not give ----
  ['camera-preset', idx(CAMERA_IDS, DYE_DATA.cameraDefault), 'Camera.CameraPreset: Kinetix22, iXonUltra897, Custom (names accepted; data/dyes/cameras.json)'],
  ['qe-curve', -1, 'Camera.QeCurve: QE(lambda) of a camera (index/name), -1 = the preset\'s, Custom = flat at qe'],
  ['qe', 0.85, 'Camera.QuantumEfficiency (the flat QE of the Custom curve)'],
  ['camera-type', -1, 'Camera.CameraType: -1 = the preset\'s, 0 sCMOS, 1 EMCCD'],
  ['dark-per-sec', 1.03, 'Camera.DarkCurrentElectronsPerSec'],
  ['gain', 0.25, 'Camera.GainElectronsPerADU (electrons per ADU)'],
  ['offset', 100, 'Camera.OffsetADU'],
  ['offset-std', 0.5, 'Camera.OffsetStdADU'],
  ['read-noise', 1.2, 'Camera.ReadNoiseElectrons'],
  ['gain-std-pct', 0.5, 'Camera.sCMOS_GainStdPctPerPixel (per-pixel gain spread, PRNU)'],
  ['read-noise-std-pct', 20, 'Camera.sCMOS_ReadNoiseStdPctPerPixel'],
  ['em-gain', -1, 'Camera.EMCCD_EmGain (EMCCD): -1 = the pre-amplifier sensitivity of the camera preset (1 e-/ADU when it has none) / gain, as the viewer and Micro-Manager derive it; > 0 sets it'],
  ['cic', 0.002, 'Camera.EMCCD_CicElectrons (EMCCD clock-induced charge, e-/pixel/frame)'],
  ['bit-depth', 16, 'Camera.BitDepth (EMCCD)'],
  ['modality', 0, '0 = Fluorescence (every label in its mode), 1 = BrightField (transmitted light; names accepted); the shorthand for light-epi / light-trans (Micro-Manager: the Lasers and TransmittedLamp shutters)'],
  ['light-epi', -1, 'Lasers shutter: 1 open, 0 closed, -1 = from modality (open in Fluorescence)'],
  ['light-trans', -1, 'TransmittedLamp shutter: 1 open, 0 closed, -1 = from modality (open in BrightField). Both open: fluorescence + BrightField through one camera; none: dark frames'],
  ['wf-upscale', 1, 'Renderer.WideFieldUpscaling: mean-field grid cells per pixel, per axis (1-4)'],
  ['wf-plane-nm', 25, 'Renderer.WideFieldZPlaneNm: mean-field dye plane thickness, nm'],
  ['wf-kernel-um', 7, 'mean-field PSF kernel radius cap, um'],
  ['mean-field-density-per-um2', 20, 'Renderer.MeanFieldDensityPerUm2: a continuous population (WideField dyes, pre states, dSTORM initial ON) renders mean-field above this many emitting dyes per um^2 of the focal slab, per dye below'],
  ['mean-field-slab-nm', 500, 'Renderer.MeanFieldSlabNm: that slab\'s thickness around the focal plane'],
  ['mean-field-max-emitters', 5000, 'Renderer.MeanFieldMaxEmitters: and mean-field above this many emitting dyes in the z range (cost cap of the per-dye path)'],
  ['bf-quality', 3, 'Renderer.BrightFieldQuality: speed vs precision, 1 (fast) .. 4 (precise); sets the four below unless given'],
  ['bf-sources', 0, 'Renderer.BrightFieldSources: condenser source points (0 = from bf-quality: 6/12/24/48)'],
  ['bf-upscale', 0, 'Renderer.BrightFieldUpscaling: optical grid cells per pixel, per axis (a minimum, raised to keep the grid pitch <= lambda / 4n; 0 = from bf-quality: 1)'],
  ['bf-sub', 0, 'Renderer.BrightFieldGeometrySamples: geometry samples per grid cell side (0 = from bf-quality: 1/1/2/2)'],
  ['bf-slice-um', -1, 'Renderer.BrightFieldSliceUm: multislice step, um; 0 = one thin slice (-1 = from bf-quality: 0/0.5/0.5/0.25)'],
  ['bf-margin-um', 0, 'BrightField grid margin around the FOV, um (0 = from bf-quality: 3-5)'],
  ['bf-condenser-na', 0.4, 'TransmittedLamp.CondenserNA: illumination NA (0 = coherent)'],
  ['bf-wavelength-nm', 550, 'TransmittedLamp.WavelengthNm: illumination wavelength (the camera QE is read there)'],
  ['bf-photons-per-px-per-sec', 80000, 'TransmittedLamp.IntensityPhotonsPerPxPerSec: empty-field photons per pixel per second'],
  ['bf-aberrations', 1, 'TransmittedLamp.UseObjectiveAberrations: 1 = the PSF\'s Zernike aberrations in the detection pupil, 0 = none'],
  ['bf-n-medium', 1.337, 'CellField.IndexMedium: refractive index of the medium'],
  ['bf-n-cytoplasm', 1.35, 'CellField.IndexCytoplasm'],
  ['bf-n-nucleus', 1.35, 'CellField.IndexNucleus'],
  ['bf-n-microtubule', 1.48, 'CellField.IndexMicrotubule (12.5 nm tubes)'],
  ['bf-absorption-per-um', 0, 'CellField.AbsorptionPerUm: intensity absorption of cell material, 1/um (unstained: 0)'],
  ['immersion-index', 1.518, 'Objective.ImmersionIndex (PSF and collection efficiency)'],
  ['psf-model', 3, 'Renderer.PsfModel: 0 = Gaussian, 3 = GibsonLanniZernike (names accepted; 1/2 need the adapter\'s JVM)'],
  ['psf-zernike-preset', 9, 'Objective.ZernikePreset: index or name (0 None ... 9 MixedRealisticObjective ... 12)'],
  ['psf-mask', 0, 'pupil mask: 0 = None, 1 = DoubleHelix (names accepted; not in Micro-Manager)'],
  ['psf-mask-modes', 5, 'double-helix Gauss-Laguerre modes (2-8)'],
  ['psf-mask-waist', 1.0, 'double-helix waist, pupil radii'],
  ['psf-oversampling', 6, 'Renderer.PsfOversampling: kernel samples per camera pixel, per axis (1-16)'],
  ['psf-kernel-half-width-nm', 7000, 'Objective.PsfKernelHalfWidthNm (a minimum: grown to 3x the Rayleigh radius)'],
  ['psf-z-range-um', 7.0, 'Objective.PsfZRangeUm: span of the PSF z stack'],
  ['psf-z-step-um', 0.1, 'Objective.PsfZStepUm: PSF z plane spacing'],
  ['psf-sample-index', 1.518, 'SampleHolder.PsfSampleIndex: sample refractive index (Gibson-Lanni)'],
  ['psf-working-distance-um', 150, 'Objective.WorkingDistanceUm (Gibson-Lanni ti0)'],
  ['psf-sample-depth-nm', 0, 'SampleHolder.PsfSampleDepthNm: emitter depth below the coverslip (Gibson-Lanni)'],
  ['psf-pupil-samples', 0, 'Renderer.PsfPupilSamples: pupil samples per axis of the PSF evaluation (0 = as the window needs; 64 = webSMLM)'],
  ['psf-halo-cut', 3e-6, "Renderer.PsfHaloCut: blink splats leave out camera pixels below this share of the emitter's photons (Quality Fast 1e-5, Realistic 3e-6, Exhaustive 0 = the whole kernel; WideField keeps the whole kernel)"],
  ['psf-interp', 2, 'Renderer.PsfInterp: 0 Nearest, 1 Linear, 2 Cubic, 3 Fft (names accepted)'],
];
// Per-structure dye overrides `<prefix>-dye.<field>` and slot overrides `dye<N>.<field>` (DYE_FIELDS keys).
export const DYE_FIELD_NAMES = Object.keys(DYE_FIELDS);
const DEFAULTS = Object.fromEntries(SCOPE_OPTIONS.map(([k, v]) => [k, v]));
const NAMES = {
  modality: ['Fluorescence', 'BrightField'], 'psf-model': ['Gaussian', 'RichardsWolf', 'GibsonLanni', 'GibsonLanniZernike'],
  'psf-mask': ['None', 'DoubleHelix'], 'psf-interp': ['Nearest', 'Linear', 'Cubic', 'Fft'], 'psf-zernike-preset': ZERNIKE_PRESETS,
  'mt-dye': DYE_CHOICES, 'mt-mode': MODES, 'mt-orient': ORIENTATION_MODES, 'mt-motion': MOTION_MODES,
  mode: MODES, specimen: DYE_DATA.specimens.map(sp => sp.id),
  'dye1.source': DYE_IDS, 'dye2.source': DYE_IDS, 'dye3.source': DYE_IDS,
  'ex-filter': EXCITATION_FILTER_IDS, dichroic: DICHROIC_IDS, 'em-filter': EMISSION_FILTER_IDS, 'light-preset': LIGHT_PRESET_CHOICES, 'camera-preset': CAMERA_IDS, 'qe-curve': CAMERA_IDS,
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
    // The special values (ScopeOptionValue): a target's Typical dye, its DyeDefault / Global mode; no global mode.
    if (Number.isNaN(x)) {
      const t = String(v);
      if ((k.endsWith('-dye') && t === 'Typical') || (k.endsWith('-mode') && t === 'DyeDefault') || (k === 'mode' && t === 'None')) x = -1;
      else if (k.endsWith('-mode') && t === 'Global') x = -2;
    }
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
  'read-noise-std-pct': 'readNoiseStdPct', cic: 'cicElectrons', 'bit-depth': 'bitDepth' };
// Which lights are on (ScopeLights): light-epi (the lasers' shutter) and light-trans (the lamp's), each 1 open / 0
// closed / -1 from modality (Fluorescence: epi, BrightField: trans).
export function scopeLights(spec) {
  const O = getter(spec), e = O('light-epi'), t = O('light-trans'), brightField = O('modality') === 1;
  return { epi: e >= 0 ? e !== 0 : !brightField, trans: t >= 0 ? t !== 0 : brightField };
}

// The preset's gain depends on the imaging (cameraPresetGain): the lamp on (BrightField, alone or with fluorescence),
// or every structure in WideField mode.
function wideFieldOrBrightField(spec) {
  if (scopeLights(spec).trans) return true;
  return STRUCTURES.every((s, i) => scopeStructureDye(spec, i).eff.mode === 'WideField');
}

// A structure's dye choice and mode as the spec gives them, resolved (ScopeStructureDye): <prefix>-mode -2 = the global
// mode option (-1 there: none); <prefix>-dye -1 = the target's typical dye in that mode (the library's default mode
// when none is asked). { eff, choice }: the effective dye and the resolved index into DYE_CHOICES.
export function scopeStructureDye(spec, s) {
  const O = getter(spec), P = STRUCTURES[s].prefix, { slots, byStructure } = dyeOverrides(spec);
  let mode = Math.trunc(O(`${P}-mode`));
  if (mode === -2) mode = Math.trunc(O('mode'));
  if (mode < -1) mode = -1;
  let choice = Math.trunc(O(`${P}-dye`));
  if (choice === -1) {
    if (mode < 0) mode = MODES.indexOf(DYE_DATA.dyeDefault.mode);
    choice = DYE_IDS.indexOf(DYE_DATA.typicalLabels[STRUCTURES[s].id][MODES[mode]].dye);
  }
  return { eff: effectiveDye(choice, slots, byStructure[P], mode), choice };
}
// A structure's labelled % (-1: the target's typical % in its mode).
export function scopeStructureLabelingPct(spec, s, mode) {
  const pct = getter(spec)(`${STRUCTURES[s].prefix}-label-pct`);
  return pct >= 0 ? pct : DYE_DATA.typicalLabels[STRUCTURES[s].id][mode].labelingPct;
}
export function scopeCamera(spec) {
  const O = getter(spec), preset = cameraPreset(Math.trunc(O('camera-preset')));
  const C = n => (n in spec || preset[CAMERA_KEYS[n]] === undefined ? O(n)
    : n === 'gain' ? cameraPresetGain(preset, wideFieldOrBrightField(spec)) : preset[CAMERA_KEYS[n]]);
  const type = O('camera-type') >= 0 ? O('camera-type') : (preset.type === 'EMCCD' ? 1 : 0);
  const qeCurve = O('qe-curve') >= 0 ? Math.trunc(O('qe-curve')) : CAMERA_IDS.indexOf(preset.id);
  return { preset: preset.id, qeCurve, qeFlat: C('qe'), emccd: type === 1, darkPerSec: C('dark-per-sec'),
    gainPhotonsPerAdu: C('gain'), offsetAdu: C('offset'), offsetStdAdu: C('offset-std'), readNoiseElectrons: C('read-noise'),
    gainStdFraction: C('gain-std-pct') / 100.0, readNoiseStdFraction: C('read-noise-std-pct') / 100.0,
    emGain: O('em-gain') > 0 ? O('em-gain') : emGainFromGain(cameraPreamp(preset), C('gain')), cicElectrons: C('cic'), bitDepth: C('bit-depth') };
}

// The light preset a spec asks for (LightPreset): null, or {lasers: {nm: kW}, excitationFilter?, dichroic, emissionFilter}.
export function scopeLightPreset(spec) {
  const O = getter(spec), i = Math.trunc(O('light-preset'));
  if (i < 0) return null;
  let id = LIGHT_PRESET_CHOICES[i];
  if (id === 'auto') {
    const { eff } = scopeStructureDye(spec, 0);
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
    if (!('ex-filter' in spec)) spec['ex-filter'] = EXCITATION_FILTER_IDS.indexOf(q.excitationFilter ?? 'None');
    if (!('dichroic' in spec)) spec.dichroic = DICHROIC_IDS.indexOf(q.dichroic);
    if (!('em-filter' in spec)) spec['em-filter'] = EMISSION_FILTER_IDS.indexOf(q.emissionFilter);
  }
  const O = getter(spec);
  const lasers = LASER_LINES.map(nm => ({ nm, kWPerCm2: Math.max(0, O(`laser-${nm}`)) }));
  if (O('laser-custom-nm') > 0) lasers.push({ nm: O('laser-custom-nm'), kWPerCm2: Math.max(0, O('laser-custom')) });
  return makeLightPath({ lasers, excitationFilter: Math.trunc(O('ex-filter')), exLoNm: O('ex-lo-nm'), exHiNm: O('ex-hi-nm'),
    dichroic: Math.trunc(O('dichroic')), dichroicEdgeNm: O('dichroic-edge-nm'),
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
  const O = getter(spec);
  return STRUCTURES.map((s, i) => {
    const P = s.prefix;
    const { eff } = scopeStructureDye(spec, i);
    const pct = scopeStructureLabelingPct(spec, i, eff.mode);
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
  if (Math.trunc(O('specimen')) !== 0) throw new Error(`specimen ${Math.trunc(O('specimen'))}: only the CellField (0) exists`);
  const pixelSizeNm = O('pixel-nm');
  const camera = scopeCamera(spec);
  const lp = scopeLightPath(spec, camera);
  const lights = scopeLights(spec);
  const brightField = lights.trans && !lights.epi;   // both: the fluorescence chain at QE 1, the lamp's photons x its QE
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
    // The per-pixel gain and read-noise spreads are an sCMOS's (an amplifier per pixel); an EMCCD reads every pixel
    // through one (MakeScopeSetup).
    gainStdFraction: camera.emccd ? 0.0 : camera.gainStdFraction,
    readNoiseStdFraction: camera.emccd ? 0.0 : camera.readNoiseStdFraction,
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
  // Sample drift: the per-frame path (nm, zero at frame 0) and the query rect grown so every frame's dyes are in it
  // (the renderer adds the drift) and the z window by the largest |dz| (the focus itself stays).
  const driftSettings = { xyNmPerSqrtSec: Math.max(0.0, O('drift-xy-nm-per-sqrt-sec')), zNmPerSqrtSec: Math.max(0.0, O('drift-z-nm-per-sqrt-sec')),
    xySpeedNmPerSec: Math.max(0.0, O('drift-xy-speed-nm-per-sec')), zSpeedNmPerSec: O('drift-z-speed-nm-per-sec'),
    xyAngleDeg: O('drift-xy-angle-deg'), zDirection: Math.round(O('drift-z-direction')),
    angleWanderDeg: Math.max(0.0, O('drift-xy-angle-wander-deg')), zAngleWanderDeg: Math.max(0.0, O('drift-z-angle-wander-deg')),
    speedWanderPct: Math.max(0.0, O('drift-speed-wander-pct')), wanderTimeSec: Math.max(0.0, O('drift-wander-time-sec')) };
  const isDrift = driftOn(driftSettings);
  const drift = isDrift ? driftTrajectory(seed, N, expSec, driftSettings) : [];
  const driftBounds = isDrift ? driftRange(drift) : null;
  if (isDrift) {
    const b = driftBounds;
    q.x0Um -= b.xHi / 1000.0; q.x1Um -= b.xLo / 1000.0;
    q.y0Um -= b.yHi / 1000.0; q.y1Um -= b.yLo / 1000.0;
    if (q.zHalfRangeUm > 0.0) q.zHalfRangeUm += Math.max(-b.zLo, b.zHi) / 1000.0;
  }
  return { O, seed, W, H, N, expSec, t0Sec, p, cam, camera, lp, labels, worldSeed, worldParams, q, driftSettings, driftOn: isDrift, drift, driftBounds,
    meanField: { densityPerUm2: Math.max(0, O('mean-field-density-per-um2')), slabNm: Math.max(0, O('mean-field-slab-nm')),
      maxEmitters: Math.max(0, O('mean-field-max-emitters')) } };
}

// info.driftNm: x, y, z per frame (empty without drift).
export const driftInfo = S => S.drift.flatMap(d => [d.x, d.y, d.z]);

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
    maskWaist: O('psf-mask-waist'), pupilSamples: Math.trunc(Math.max(0, O('psf-pupil-samples'))), interpMode: Math.trunc(Math.min(3, Math.max(0, O('psf-interp')))),
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
// The PSF a movie of the spec uses, for display (MakeScopePsfPreview, the viewer's Preview PSF): the microtubules'
// emitting state's kernel (the pre state when the main one is dark) and, per z plane, the camera image of one emitter
// of 1 photon at the centre of the middle pixel, splatted as a movie does. Gaussian: one plane, sampled at
// psf-oversampling, and its renderGaussian image. onPlane(k, nz): kernel progress.
export function scopePsfPreview(spec, onPlane) {
  const O = getter(spec), lp = scopeLightPath(spec, scopeCamera(spec)), L = scopeLabels(spec, lp)[0];
  const st = L.states.main || L.states.pre;
  const lambdaNm = kernelWavelengthNm(st ? st.lambdaNm : 670.0);
  const c = scopeKernel(spec, lambdaNm, onPlane);
  if (c) {
    const os = Math.max(1, c.oversampling), camRad = Math.trunc(c.halfWidthOversampled / os), N = 2 * camRad + 1;
    const P = c.sizeOversampled * c.sizeOversampled, planes = new Float32Array(P * c.nz), cams = new Float32Array(N * N * c.nz);
    for (let z = 0; z < c.nz; z++) {
      planes.set(c.planes[z], z * P);
      const plan = planSplat(c, z, camRad, camRad, 1.0, c.interpMode);
      if (plan) splatRows(cams.subarray(z * N * N, (z + 1) * N * N), N, N, 0, N, c, plan, 1.0);
    }
    return { gaussian: false, oversampling: os, size: c.sizeOversampled, nz: c.nz, camSize: N, zStepNm: c.zStepNm, lambdaNm, planes, cams };
  }
  const sigma = Math.min(Math.max(0.21 * lambdaNm / Math.max(0.01, O('na')) / O('pixel-nm'), 0.3), 20.0);
  const os = Math.trunc(Math.min(16, Math.max(1, O('psf-oversampling')))), camRad = Math.ceil(4 * sigma) + 1, N = 2 * camRad + 1;
  const size = N * os, planes = new Float32Array(size * size), cams = new Float32Array(N * N);
  for (let y = 0; y < size; y++) for (let x = 0; x < size; x++) {
    const dx = (x + 0.5) / os - 0.5 - camRad, dy = (y + 0.5) / os - 0.5 - camRad;
    planes[y * size + x] = Math.exp(-(dx * dx + dy * dy) / (2 * sigma * sigma));
  }
  renderGaussian(cams, N, N, camRad, camRad, sigma, 1.0);
  return { gaussian: true, oversampling: os, size, nz: 1, camSize: N, zStepNm: 0, lambdaNm, planes, cams };
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
  return world.continuousInWindow(q.x0Um, q.y0Um, q.x1Um, q.y1Um, zMin, zMax, q.tSec).map(e => toFov(q, e));
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
    const kernels = !scopeLights(spec).epi ? [] : S.labels.flatMap(l => Object.values(l.states))
      .filter(st => st && st.detectedFraction > 0).map(st => scopeKernel(spec, kernelWavelengthNm(st.lambdaNm)));
    return { width: S.W, height: S.H, frames: 0, blinks: 0, querySec: tWorld, totalSec: (performance.now() - t0) / 1000,
      psf: kernels.some(Boolean) ? 'GibsonLanniZernike' : 'Gaussian' };
  }
  const { epi, trans } = scopeLights(spec);
  if (!epi && !trans) return renderDarkMovie(S, onFrame);
  if (epi && trans) return renderCombinedMovie(P, spec, S, onFrame, opts);
  if (trans) return renderBrightfieldMovie(P, spec, S, onFrame, opts);
  return renderFluorescenceMovie(P, spec, S, onFrame, opts);
}

// No light: the camera's noise on zero photons (RenderDarkMovie).
function renderDarkMovie(S, onFrame) {
  const t0 = performance.now(), maps = noiseMaps(S.seed, S.W, S.H, S.cam), photons = new Float32Array(S.W * S.H);
  for (let f = 0; f < S.N; f++) if (onFrame(f, applyNoiseChain(photons, S.cam, maps, f), photons) === false) break;
  return { width: S.W, height: S.H, frames: S.N, blinks: 0, totalSec: (performance.now() - t0) / 1000, light: 'none' };
}

// Both lights (RenderCombinedMovie): the fluorescence photons (its noise chain at QE 1) plus the lamp's photons x the
// camera's QE at the lamp wavelength, then that one noise chain.
function renderCombinedMovie(P, spec, S, onFrame, opts) {
  const lamp = brightfieldPhotons(P, spec, S);
  const qeLamp = sampleAt(S.lp.qe, S.O('bf-wavelength-nm'));
  const maps = noiseMaps(S.seed, S.W, S.H, S.cam), sum = new Float32Array(S.W * S.H);
  const info = renderFluorescenceMovie(P, spec, S, onFrame, { ...opts, onPhotons: (f, fl) => {
    const bf = lamp.at(f);
    for (let i = 0; i < sum.length; i++) sum[i] = fl[i] + Math.fround(bf[i] * qeLamp);
    return onFrame(f, applyNoiseChain(sum, S.cam, maps, f), sum);
  } });
  return { ...info, light: 'epi+trans', bfPhotonsPerPx: lamp.flux, bfQe: qeLamp };
}
