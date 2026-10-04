// A cell-field movie in JS: the mirror of adapter/inSiliScope/Simulation/ScopeMovie.cpp (+ CellFieldSource),
// i.e. what insiliscope_cli / the viewer's isc_scope_movie render, option for option:
//   world (seed, p.* geometry) -> blink events of the FOV -> per frame: RenderPhotonImage (PSF kernel splat
//   or Gaussian) -> ApplyNoiseChain;   modality=1: WideField (widefield.js);   modality=2: BrightField (brightfield.js).
// This is the JS REFERENCE for imaging. Iterate on photophysics/PSF/camera here (web/lab), then port to
// the C++ files named in each module's header when merging (web/lab/README.md, tests/parity/scope_parity.mjs).
import { World } from './world.js';
import { ZERNIKE_PRESETS, zernikePresetCoefficients, psfKernelHalfWidthPx, buildZernikeKernelCache, NUM_ZERNIKE } from './psf.js';
import { bucketEventsByFrame, renderPhotonImage, noiseMaps, applyNoiseChain } from './render.js';
import { renderWidefieldMovie } from './widefield.js';
import { renderBrightfieldMovie } from './brightfield.js';

// [name, default, help]: ScopeMovieOptions(), same order and defaults.
export const SCOPE_OPTIONS = [
  ['seed', 42, 'SimType_RandomSeed (cell field = seed ^ 0x43454C4C unless world-seed >= 0; noise as the adapter)'],
  ['world-seed', -1, 'cell-field world seed used as is (the viewer\'s seed); -1 = derive it from seed'],
  ['x', 0, 'FOV centre x, world um (XY stage position)'],
  ['y', 0, 'FOV centre y, world um'],
  ['z', 0.5, 'Z stage: focal-plane height above the coverslip, um (as the ZStage device; it starts at 0.5)'],
  ['size', 128, 'FOV width = height, pixels'],
  ['frames', 1000, 'number of frames'],
  ['exposure-ms', 50, 'frame duration, ms (simulated time per frame)'],
  ['start-sec', 0, 'simulated time of the first frame, s'],
  ['pixel-nm', 100, 'pixel size, nm'],
  ['photons-per-sec', 7500, 'FluoParam_PhotonsPerSecond'],
  ['on-sec', 0.05, 'FluoParam_OnLifetimeSec'],
  ['off-sec', 1.0, 'FluoParam_OffLifetimeSec'],
  ['bleach-prob', 1.0, 'FluoParam_BlinkBleachProb'],
  ['photon-cv', 0.5, 'FluoParam_PhotonCV (per-blink log-normal brightness spread)'],
  ['background-per-sec', 0, 'Background_BackgroundPhotonsPerSec (photons/pixel/s)'],
  ['wavelength-nm', 660, 'PSFParam_PsfEmissionWavelengthNm (Gaussian sigma = 0.21 lambda / NA)'],
  ['na', 1.4, 'PSFParam_PsfNa: numerical aperture'],
  ['focus-um', 0, 'SimType_CellFieldFocusHeightUm (focus offset added to z)'],
  ['z-range-um', 7.0, 'SimType_CellFieldZRangeUm: dyes within +/- z-range/2 of the focal plane are rendered (0 = all)'],
  ['milli-activation-rate', 1.43, 'SimType_CellFieldMilliActivationRatePerDyePerSec (per dark dye, 1e-3/s)'],
  ['labeling-pct-bleaching', 0, 'SimType_CellFieldLabelingPctBleaching (bleaching dyes, % of lattice sites)'],
  ['labeling-pct-nonbleaching', 70, 'SimType_CellFieldLabelingPctNonBleaching (persistent, DNA-PAINT-like sites)'],
  ['chunk-um', 26, 'SimType_CellFieldChunkSizeUm'],
  ['occupancy', 0.33, 'SimType_CellFieldOccupancy'],
  ['cell-diam-min-um', 25, 'SimType_CellFieldCellDiameterMinUm'],
  ['cell-diam-max-um', 35, 'SimType_CellFieldCellDiameterMaxUm'],
  ['mt-density', 0.9, 'SimType_CellFieldMicrotubuleDensityPerUm2'],
  ['packing', 1, 'SimType_CellFieldPacking (1 on, 0 off)'],
  ['qe', 0.85, 'CamParam_QuantumEfficiency'],
  ['dark-per-sec', 1.03, 'CamParam_DarkCurrentElectronsPerSec'],
  ['gain', 0.25, 'CamParam_GainPhotonsPerADU'],
  ['offset', 100, 'CamParam_OffsetADU'],
  ['offset-std', 0.5, 'CamParam_OffsetStdADU'],
  ['read-noise', 1.2, 'CamParam_ReadNoiseElectrons'],
  ['gain-std-pct', 0.5, 'CamParam_GainStdPctPerPixel (per-pixel gain spread, PRNU)'],
  ['read-noise-std-pct', 20, 'CamParam_ReadNoiseStdPctPerPixel'],
  ['modality', 0, 'General_ImagingModality: 0 = SuperRes (blinks), 1 = WideField (all dyes), 2 = BrightField (transmitted light; names accepted)'],
  ['wf-upscale', 1, 'General_WideFieldUpscaling: WideField grid cells per pixel, per axis (1-4)'],
  ['wf-plane-nm', 25, 'General_WideFieldZPlaneNm: WideField dye plane thickness, nm'],
  ['wf-kernel-um', 7, 'WideField PSF kernel radius cap, um'],
  ['wf-excitation-photons-per-um2-per-sec', 4e8, 'FluoParam_WideFieldExcitationPhotonsPerUm2PerSec'],
  ['wf-quantum-yield', 0.7, 'FluoParam_WideFieldQuantumYield'],
  ['wf-photon-budget', 5000, 'FluoParam_WideFieldPhotonBudget: emitted photons per dye (0 = never bleaches)'],
  ['wf-extinction-coeff', 270000, 'FluoParam_WideFieldExtinctionCoeff, M^-1 cm^-1'],
  ['bf-quality', 3, 'General_BrightFieldQuality: speed vs precision, 1 (fast) .. 4 (precise); sets the four below unless given'],
  ['bf-sources', 0, 'General_BrightFieldSources: condenser source points (0 = from bf-quality: 6/12/24/48)'],
  ['bf-upscale', 0, 'General_BrightFieldUpscaling: optical grid cells per pixel, per axis (a minimum, raised to keep the grid pitch <= lambda / 4n; 0 = from bf-quality: 1)'],
  ['bf-sub', 0, 'General_BrightFieldGeometrySamples: geometry samples per grid cell side (0 = from bf-quality: 1/1/2/2)'],
  ['bf-slice-um', -1, 'General_BrightFieldSliceUm: multislice step, um; 0 = one thin slice (-1 = from bf-quality: 0/0.5/0.5/0.25)'],
  ['bf-margin-um', 0, 'BrightField grid margin around the FOV, um (0 = from bf-quality: 3-5)'],
  ['bf-condenser-na', 0.55, 'General_BrightFieldCondenserNa: illumination NA (0 = coherent)'],
  ['bf-wavelength-nm', 550, 'General_BrightFieldWavelengthNm: illumination wavelength'],
  ['bf-photons-per-px-per-sec', 40000, 'General_BrightFieldPhotonsPerPxPerSec: empty-field photons per pixel per second'],
  ['bf-aberrations', 1, 'General_BrightFieldAberrations: 1 = the PSF\'s Zernike aberrations in the detection pupil, 0 = none'],
  ['bf-n-medium', 1.337, 'SimType_CellFieldIndexMedium: refractive index of the medium'],
  ['bf-n-cytoplasm', 1.345, 'SimType_CellFieldIndexCytoplasm'],
  ['bf-n-nucleus', 1.345, 'SimType_CellFieldIndexNucleus'],
  ['bf-n-microtubule', 1.48, 'SimType_CellFieldIndexMicrotubule (12.5 nm tubes)'],
  ['bf-absorption-per-um', 0, 'SimType_CellFieldAbsorptionPerUm: intensity absorption of cell material, 1/um (unstained: 0)'],
  ['immersion-index', 1.518, 'PSFParam_PsfImmersionIndex (PSF and WideField collection efficiency)'],
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
const DEFAULTS = Object.fromEntries(SCOPE_OPTIONS.map(([k, v]) => [k, v]));
const NAMES = {
  modality: ['SuperRes', 'WideField', 'BrightField'], 'psf-model': ['Gaussian', 'RichardsWolf', 'GibsonLanni', 'GibsonLanniZernike'],
  'psf-mask': ['None', 'DoubleHelix'], 'psf-interp': ['Nearest', 'Linear', 'Cubic', 'Fft'], 'psf-zernike-preset': ZERNIKE_PRESETS,
};

// "k=v k=v" (spaces, commas, semicolons) or an object -> spec object. Names accepted where the C++ does.
export function parseSpec(spec) {
  const out = {};
  const set = (k, v) => {
    if (!(k in DEFAULTS) && !/^p\.\w+$/.test(k) && !/^zern\.\d{1,2}$/.test(k)) throw new Error(`bad option '${k}'`);
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
  return { width: w, height: w, frames: Math.trunc(Math.min(100000, Math.max(1, O('frames')))) };
}

// MakeScopeSetup: frame-equivalent parameters, world settings, the FOV query.
export function scopeSetup(P, spec) {
  const O = getter(spec);
  const seed = Math.trunc(O('seed'));
  const { width: W, height: H, frames: N } = scopeDims(spec);
  const expSec = Math.max(1e-6, O('exposure-ms') / 1000.0), t0Sec = Math.max(0.0, O('start-sec'));
  const pixelSizeNm = O('pixel-nm');
  const p = {
    pixelSizeNm, photonsPerBlink: O('photons-per-sec') * expSec, backgroundPhotons: O('background-per-sec') * expSec,
    psfSigmaPx: Math.min(Math.max(0.21 * O('wavelength-nm') / Math.max(0.01, O('na')) / pixelSizeNm, 0.3), 20.0),
    frameDurationSec: expSec,
  };
  const cam = {
    quantumEfficiency: O('qe'), darkCurrentElectrons: O('dark-per-sec') * expSec, gainPhotonsPerAdu: O('gain'),
    offsetAdu: O('offset'), offsetStdAdu: O('offset-std'), readNoiseElectrons: O('read-noise'),
    gainStdFraction: O('gain-std-pct') / 100.0, readNoiseStdFraction: O('read-noise-std-pct') / 100.0,
    emccd: false, emGain: 300, cicElectrons: 0.002, bitDepth: 16,
  };
  const ws = O('world-seed');
  const worldSeed = ws >= 0 ? Number(BigInt.asUintN(32, BigInt(Math.trunc(ws))))
    : Number(BigInt.asUintN(32, BigInt(seed) ^ 0x43454C4Cn));
  // Named world options, then p.* (prototype names) on top; the rest are the prototype's defaults.
  const vals = { ...P.defaults, chunkSize: O('chunk-um'), density: O('occupancy'), cellDiamMin: O('cell-diam-min-um'),
    cellDiamMax: O('cell-diam-max-um'), mtDensity: O('mt-density'), enablePacking: O('packing') !== 0 };
  const extra = { labelEfficiency: O('labeling-pct-bleaching') / 100.0, labelNonBleaching: O('labeling-pct-nonbleaching') / 100.0 };
  for (const [k, v] of Object.entries(spec)) {
    if (!k.startsWith('p.')) continue;
    const name = k.slice(2);
    if (name in extra) extra[name] = v;
    else if (name in P.defaults) vals[name] = typeof P.defaults[name] === 'boolean' ? v !== 0 : v;
  }
  const worldParams = { ...P.paramsFrom(vals), ...extra };
  const kin = { activationRatePerSec: O('milli-activation-rate') / 1000.0, onSec: Math.max(1e-6, O('on-sec')),
    offSec: Math.max(0.0, O('off-sec')), bleachProb: O('bleach-prob'), photonCV: O('photon-cv') };
  const um = pixelSizeNm / 1000.0, margin = 2.0;
  const originXUm = O('x') - W * um / 2, originYUm = O('y') - H * um / 2;
  const q = {
    originXUm, originYUm, x0Um: originXUm - margin, x1Um: originXUm + W * um + margin,
    y0Um: originYUm - margin, y1Um: originYUm + H * um + margin,
    zRefUm: O('focus-um'), zCullCentreUm: O('focus-um') + O('z'), zHalfRangeUm: Math.max(0.0, O('z-range-um')) / 2,
    frameSec: expSec, tSec: t0Sec, spanSec: N * expSec, frameIndex: 0,
  };
  return { O, seed, W, H, N, expSec, t0Sec, p, cam, worldSeed, worldParams, kin, q };
}

// The PSF request of a spec (ScopePsfRequest), or null for the Gaussian.
export function scopePsfRequest(spec) {
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
  const pixelSizeNm = O('pixel-nm'), wavelengthNm = O('wavelength-nm'), na = O('na');
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

// ---- caches (speed only; same answers) ----
const kernelMemo = [];  // last 2, keyed by everything but interpMode
let worldMemo = null;
export function scopeKernel(spec, onPlane) {
  const req = scopePsfRequest(spec);
  if (!req) return null;
  const key = JSON.stringify({ ...req, interpMode: 0 });
  let hit = kernelMemo.find(e => e.key === key);
  if (!hit) {
    hit = { key, cache: buildZernikeKernelCache(req, onPlane) };
    kernelMemo.unshift(hit);
    kernelMemo.length = Math.min(kernelMemo.length, 2);
  }
  return { ...hit.cache, interpMode: req.interpMode };
}
// The labelling fractions enter only the dye draw (World.dyeBlock): a change of
// those alone keeps the world's cells and microtubules and redraws the dyes.
const LABEL_PARAMS = new Set(['labelEfficiency', 'labelNonBleaching']);
export function scopeWorld(P, S) {
  const geom = Object.fromEntries(Object.entries(S.worldParams).filter(([k]) => !LABEL_PARAMS.has(k)));
  const geomKey = JSON.stringify([S.worldSeed, geom]), key = JSON.stringify([S.worldSeed, S.worldParams]);
  if (!worldMemo || worldMemo.P !== P || worldMemo.geomKey !== geomKey)
    worldMemo = { P, geomKey, key, world: new World(P, S.worldSeed, S.worldParams, S.kin) };
  else {
    if (worldMemo.key !== key) { worldMemo.world.p = S.worldParams; worldMemo.world.dyeBlocks.clear(); worldMemo.key = key; }
    if (JSON.stringify(worldMemo.world.kin) !== JSON.stringify(S.kin)) worldMemo.world.setKinetics(S.kin);
  }
  return worldMemo.world;
}

// CellFieldSource::Events: world blinks of q as BlinkEvents in the FOV frame.
export function cellFieldEvents(world, q) {
  const zMin = q.zHalfRangeUm > 0 ? q.zCullCentreUm - q.zHalfRangeUm : -Infinity;
  const zMax = q.zHalfRangeUm > 0 ? q.zCullCentreUm + q.zHalfRangeUm : Infinity;
  const ev = world.eventsInWindow(q.x0Um, q.y0Um, q.x1Um, q.y1Um, zMin, zMax, q.tSec, q.tSec + q.spanSec);
  return ev.map(e => ({ xUm: e.x - q.originXUm, yUm: e.y - q.originYUm, zNm: (e.z - q.zRefUm) * 1000.0,
    tStart: q.frameIndex + (e.tOn - q.tSec) / q.frameSec, tEnd: q.frameIndex + (e.tOff - q.tSec) / q.frameSec,
    brightness: e.brightness }));
}

// RenderScopeMovie: onFrame(f, Uint16Array) for every frame (return false to stop). Returns info.
// P: loadPrototype() instance (the geometry truth). opts.onProgress(stage, frac) optional.
export function renderScopeMovie(P, specIn, onFrame, opts = {}) {
  const spec = parseSpec(specIn);
  const t0 = performance.now();
  const S = scopeSetup(P, spec);
  if (S.O('modality') === 1) return renderWidefieldMovie(P, spec, S, onFrame, opts);
  if (S.O('modality') === 2) return renderBrightfieldMovie(P, spec, S, onFrame, opts);
  const kernel = scopeKernel(spec, opts.onProgress && ((k, nz) => opts.onProgress('psf', (k + 1) / nz)));
  const tPsf = performance.now();
  const world = scopeWorld(P, S);
  const events = cellFieldEvents(world, S.q);
  if (opts.onEvents) opts.onEvents(events, S);
  const querySec = (performance.now() - t0) / 1000;
  const maps = noiseMaps(S.seed, S.W, S.H, S.cam);
  const buckets = bucketEventsByFrame(events, S.N);
  const zStage = S.O('z');
  for (let f = 0; f < S.N; f++) {
    const photons = renderPhotonImage(S.W, S.H, buckets[f].map(i => events[i]), f, S.p, kernel, zStage);
    if (onFrame(f, applyNoiseChain(photons, S.cam, maps, f), photons) === false) break;
    if (opts.onProgress) opts.onProgress('frames', (f + 1) / S.N);
  }
  return { width: S.W, height: S.H, frames: S.N, blinks: events.length, psfSec: (tPsf - t0) / 1000, querySec,
    totalSec: (performance.now() - t0) / 1000, psf: kernel ? 'GibsonLanniZernike' : 'Gaussian' };
}
