// Frame rendering and camera noise of the imaging path, mirroring adapter/inSiliScope/Simulation/
//   SMLMSimulation.cpp  RenderPhotonImage (Gaussian or kernel splat), BucketEventsByFrame
//   SMLMNoise.cpp       per-pixel offset/gain/read-noise maps (mt19937_64), ApplyNoiseChain (sCMOS, EMCCD)
import { Mt19937_64, gaussianRng, CounterRng, counterPoisson, counterGauss, counterGamma } from './rng.js';
import { nearestZIndex, planSplat, splatRows, lround } from './psf.js';

// Blink event in a FOV's frame: {xUm, yUm, zNm, tStart, tEnd (frames), brightness}.
export function bucketEventsByFrame(events, nFrames) {
  const buckets = Array.from({ length: Math.max(0, nFrames) }, () => []);
  for (let i = 0; i < events.length; i++) {
    const e = events[i];
    if (!(e.tEnd > 0.0) || !(e.tStart < nFrames)) continue;
    const f0 = Math.max(0, Math.floor(e.tStart)), f1 = Math.min(nFrames - 1, Math.floor(e.tEnd));
    for (let f = f0; f <= f1; ++f) buckets[f].push(i);
  }
  return buckets;
}

function renderGaussian(img, width, height, xPx, yPx, sigmaPx, totalPhotons) {
  if (totalPhotons <= 0.0 || sigmaPx <= 0.0) return;
  const rad = Math.ceil(3.0 * sigmaPx);
  const cx = lround(xPx), cy = lround(yPx);
  const amplitude = totalPhotons / (2.0 * Math.PI * sigmaPx * sigmaPx);
  const twoSigmaSq = 2.0 * sigmaPx * sigmaPx;
  const yLo = Math.max(0, cy - rad), yHi = Math.min(height - 1, cy + rad);
  const xLo = Math.max(0, cx - rad), xHi = Math.min(width - 1, cx + rad);
  for (let py = yLo; py <= yHi; ++py) {
    const ddy = py - yPx, row = py * width;
    for (let px = xLo; px <= xHi; ++px) {
      const ddx = px - xPx;
      img[row + px] += Math.fround(amplitude * Math.exp(-(ddx * ddx + ddy * ddy) / twoSigmaSq));
    }
  }
}

// One frame's photon image (background + every event overlapping [f, f+1)). kernel: PSF kernel cache or
// null (Gaussian of psfSigmaPx). zStageUm: focal-plane height; an emitter's plane is zNm/1000 - zStageUm.
export function renderPhotonImage(width, height, events, frameIndex, o, kernel, zStageUm) {
  const img = new Float32Array(width * height).fill(o.backgroundPhotons);
  for (const e of events) {
    let ov = Math.min(frameIndex + 1, e.tEnd) - Math.max(frameIndex, e.tStart);
    if (ov <= 0.0) continue;
    if (ov > 1.0) ov = 1.0;
    const xPx = e.xUm * 1000.0 / o.pixelSizeNm, yPx = e.yUm * 1000.0 / o.pixelSizeNm;
    let photons = o.photonsPerBlink * ov;
    if (e.brightness !== 1.0) photons *= e.brightness;
    if (!kernel) { renderGaussian(img, width, height, xPx, yPx, o.psfSigmaPx, photons); continue; }
    const plan = planSplat(kernel, nearestZIndex(kernel, e.zNm / 1000.0 - zStageUm), xPx, yPx, photons, kernel.interpMode);
    if (plan) splatRows(img, width, height, 0, height, kernel, plan, photons);
  }
  return img;
}

// ---- camera ----
export class NoiseMaps {
  // The camera's maps for a RandomSeed: offset, gain, read noise off one mt19937_64(seed), in that order.
  constructor(seed, W, H, cam) {
    const rng = new Mt19937_64(seed), n = W * H;
    this.offset = new Float32Array(n); this.gain = new Float32Array(n); this.readNoise = new Float32Array(n);
    for (let i = 0; i < n; i++) this.offset[i] = cam.offsetAdu + cam.offsetStdAdu * gaussianRng(rng, 0.0, 1.0);
    for (let i = 0; i < n; i++) { const v = cam.gainPhotonsPerAdu * (1.0 + cam.gainStdFraction * gaussianRng(rng, 0.0, 1.0)); this.gain[i] = v < 0.01 ? 0.01 : v; }
    for (let i = 0; i < n; i++) { const v = cam.readNoiseElectrons * (1.0 + cam.readNoiseStdFraction * gaussianRng(rng, 0.0, 1.0)); this.readNoise[i] = v < 0.0 ? 0.0 : v; }
    this.noiseSeed = Number(BigInt.asUintN(32, BigInt(seed) ^ 0x9E3779B9n));
  }
}

// Photon image -> uint16 ADU (counter-based draws per (seed, frame, pixel)).
export function applyNoiseChain(photons, cam, maps, frame) {
  const n = photons.length, out = new Uint16Array(n);
  const u = new CounterRng(maps.noiseSeed, frame);
  if (cam.emccd) {
    const maxAdu = Math.pow(2, Math.min(16, Math.max(1, cam.bitDepth))) - 1.0;
    const emGain = Math.max(1.0, cam.emGain);
    for (let i = 0; i < n; ++i) {
      u.pixel(i);
      const ph = Math.max(0.0, photons[i]);
      const ne = counterPoisson(ph * cam.quantumEfficiency + cam.darkCurrentElectrons + cam.cicElectrons, u);
      const o = ne > 0.0 ? counterGamma(ne, u) : 0.0;
      const readE = maps.readNoise[i] / emGain;
      const g = counterGauss(u);
      const adu = Math.floor(maps.offset[i] + (o + readE * g) / maps.gain[i] + 0.5);
      out[i] = adu < 0.0 ? 0.0 : (adu > maxAdu ? maxAdu : adu);
    }
    return out;
  }
  for (let i = 0; i < n; ++i) {
    u.pixel(i);
    const ph = Math.max(0.0, photons[i]);
    const ne = counterPoisson(ph * cam.quantumEfficiency + cam.darkCurrentElectrons, u);
    const g = counterGauss(u);
    let adu = maps.offset[i] + (ne + maps.readNoise[i] * g) / maps.gain[i];
    adu = adu < 0.0 ? 0.0 : (adu > 65535.0 ? 65535.0 : adu);
    out[i] = Math.floor(adu + 0.5);
  }
  return out;
}
