// BrightField imaging in JS: the mirror of adapter/inSiliScope/Simulation/BrightfieldRender.cpp (+ ScopeMovie.cpp
// ScopeBrightfieldSpec / RenderBrightfieldMovie; spec/BRIGHTFIELD.md). Same model and discretisation:
//   refractive-index slices from the world's optical volume (World.opticalVolume: cytoplasm, nucleus,
//   microtubules), a cosine taper over the outer half of the grid margin, Abbe source points (equal-area
//   Fibonacci disk over the condenser NA, snapped to the grid), multislice angular-spectrum propagation down
//   through the slices (or one thin screen at the phase-weighted height), the detection pupil (NA + the PSF's
//   Zernike terms), defocus to the focal plane, |E|^2 binned to camera pixels, averaged over sources in source
//   order; then flux x exposure and the camera noise per frame.
// Not mirrored (speed only in the C++): the cache-blocked, band-pruned FFTs (the pruned columns are zeroed by
// the propagator/pupil anyway), the exit-spectrum / transmittance caches, threads. The C++ works in complex
// float32; this keeps float32 where the C++ stores values and rounds each 1D FFT pass to float32, so images
// agree to float rounding (not bit for bit).
import { scopePsfRequest, scopeWorld } from './scope_movie.js';
import { noiseMaps, applyNoiseChain } from './render.js';
import { roundZernike, zernikeWavefrontWaves, NUM_ZERNIKE } from './psf.js';
import { fft1, fastSize } from './widefield.js';

const f32 = Math.fround;
const kPi = 3.14159265358979323846;
const kVolumeQueryFloats = 48e6;   // slices held by one optical-volume query
const kThinSub = 8;                 // a thin screen is queried as 8 sub-slices

// [sources, upscale, sub, sliceUm, marginUm]; level 5 is a reference for checks (cli/viewer/MM clamp to 4).
const LEVELS = [
  { sources: 6, upscale: 1, sub: 1, sliceUm: 0.0, marginUm: 3.0 },
  { sources: 12, upscale: 1, sub: 1, sliceUm: 0.5, marginUm: 3.0 },
  { sources: 24, upscale: 1, sub: 2, sliceUm: 0.5, marginUm: 3.0 },
  { sources: 48, upscale: 1, sub: 2, sliceUm: 0.25, marginUm: 4.0 },
  { sources: 96, upscale: 1, sub: 4, sliceUm: 0.125, marginUm: 6.0 },
];
export const brightfieldQualityLevel = level => ({ ...LEVELS[Math.max(1, Math.min(5, level)) - 1] });

// BrightfieldSpec::Resolved: the quality level's values where the spec leaves 0 (sliceUm: < 0).
export function resolved(bs) {
  const q = brightfieldQualityLevel(bs.quality);
  if (bs.sources > 0) q.sources = bs.sources;
  if (bs.upscale > 0) q.upscale = bs.upscale;
  if (bs.sub > 0) q.sub = bs.sub;
  if (bs.sliceUm >= 0.0) q.sliceUm = bs.sliceUm;
  if (bs.marginUm > 0.0) q.marginUm = bs.marginUm;
  if (bs.condenserNa <= 0.0) q.sources = 1;
  return q;
}

// Grid cells per camera pixel: the quality's upscale, raised until the pitch is <= lambda / (4 n_medium).
export function upscaleFor(bs) {
  const q = resolved(bs);
  const maxPitch = bs.wavelengthNm * 1e-3 / (4.0 * Math.max(1.0, bs.nMedium));
  const need = Math.max(0, Math.ceil(bs.pixelUm / maxPitch - 1e-9));
  return Math.min(8, Math.max(1, Math.max(1, q.upscale), need));
}

export function gridFor(bs) {
  const q = resolved(bs), up = upscaleFor(bs), pitch = bs.pixelUm / up;
  const margin = Math.ceil(q.marginUm / pitch);
  return { nx: fastSize(bs.width * up + 2 * margin, 2), ny: fastSize(bs.height * up + 2 * margin, 2), margin, up, pitch };
}

// ScopeBrightfieldSpec: the BrightfieldSpec of a scope spec (S = scopeSetup()).
export function scopeBrightfieldSpec(spec, S) {
  const O = S.O;
  const bs = {
    originXUm: S.q.originXUm, originYUm: S.q.originYUm, width: S.W, height: S.H, pixelUm: S.p.pixelSizeNm / 1000.0,
    quality: Math.trunc(Math.min(4.0, Math.max(1.0, O('bf-quality')))),
    sources: Math.trunc(Math.min(1024.0, Math.max(0.0, O('bf-sources')))),
    upscale: Math.trunc(Math.min(8.0, Math.max(0.0, O('bf-upscale')))),
    sub: Math.trunc(Math.min(16.0, Math.max(0.0, O('bf-sub')))),
    sliceUm: O('bf-slice-um') < 0 ? -1.0 : Math.max(0.0, O('bf-slice-um')),
    marginUm: Math.max(0.0, O('bf-margin-um')),
    condenserNa: Math.max(0.0, O('bf-condenser-na')), wavelengthNm: Math.max(1.0, O('bf-wavelength-nm')),
    na: Math.max(0.01, O('na')),
    nMedium: O('bf-n-medium'), nCytoplasm: O('bf-n-cytoplasm'), nNucleus: O('bf-n-nucleus'),
    nMicrotubule: O('bf-n-microtubule'), absorptionPerUm: Math.max(0.0, O('bf-absorption-per-um')),
    zernike: new Array(NUM_ZERNIKE).fill(0),
  };
  if (O('bf-aberrations') !== 0) {
    const req = scopePsfRequest(spec); // null for the Gaussian: no aberrations
    if (req) bs.zernike = req.zernike.map(roundZernike); // the C++ passes them through their 6-digit text
  }
  return bs;
}

const freq = (i, n) => (i <= Math.floor(n / 2) ? i : i - n);
const lround = v => (v < 0 ? -Math.round(-v) : Math.round(v)); // half away from zero

// ---- complex float32 helpers (re/im in separate arrays) ----
// a[i] *= (br, bi), complex<float> arithmetic.
function cmulInto(ar, ai, i, br, bi) {
  const r = ar[i], m = ai[i];
  ar[i] = f32(f32(r * br) - f32(m * bi));
  ai[i] = f32(f32(r * bi) + f32(m * br));
}
// 2D FFT of (re, im) nx x ny, sign -1 forward / +1 inverse (unnormalised); each 1D pass rounded to float32.
// Pass order as the C++: forward rows then columns, inverse columns then rows.
function fft2f(re, im, nx, ny, sign, bR, bI) {
  const rows = () => { for (let y = 0; y < ny; y++) fft1(re, im, y * nx, 1, nx, sign, bR, bI); };
  const cols = () => { for (let x = 0; x < nx; x++) fft1(re, im, x, nx, ny, sign, bR, bI); };
  const round = () => { for (let i = 0; i < re.length; i++) { re[i] = f32(re[i]); im[i] = f32(im[i]); } };
  if (sign < 0) { rows(); round(); cols(); round(); } else { cols(); round(); rows(); round(); }
}

export class BrightfieldScene {
  // Builds the slices, sources, pupil and kz of bs over `world` (World of world.js).
  constructor(world, bs) {
    if (!(bs.width > 0 && bs.height > 0 && bs.pixelUm > 0 && bs.wavelengthNm > 0 && bs.na > 0 && bs.nMedium > 0))
      throw new Error('BrightField: bad FOV, wavelength, NA or medium index.');
    const t0 = performance.now();
    this.spec = bs;
    const q = this.q = resolved(bs);
    const G = gridFor(bs);
    const nx = this.nx = G.nx, ny = this.ny = G.ny, margin = this.margin = G.margin;
    this.up = G.up;
    const pitch = this.pitch = G.pitch;
    const k0 = this.k0 = 2.0 * kPi / (bs.wavelengthNm * 1e-3);
    const N = nx * ny;
    const gx0 = bs.originXUm - margin * pitch, gy0 = bs.originYUm - margin * pitch;
    const gx1 = gx0 + nx * pitch, gy1 = gy0 + ny * pitch;

    // Slices over [0, tallest cell].
    const zTop = world.maxCellHeight(gx0, gy0, gx1, gy1);
    const slices = this.slices = (q.sliceUm > 0.0 && zTop > 0.0) ? Math.max(1, Math.ceil(zTop / q.sliceUm)) : 1;
    const dz = this.dz = zTop > 0.0 ? zTop / slices : 1.0;
    this.objectZ = 0.5 * dz;
    const qSlices = slices === 1 && zTop > 0.0 ? kThinSub : slices;
    const qDz = zTop > 0.0 ? zTop / qSlices : 1.0;
    let phaseSum = 0.0, phaseZ = 0.0;
    const phase = this.phase = new Float32Array(N * slices);
    const atten = this.atten = bs.absorptionPerUm > 0.0 ? new Float32Array(N * slices).fill(1) : null;
    if (zTop > 0.0) {
      const group = Math.max(1, Math.min(qSlices, Math.trunc(kVolumeQueryFloats / (3.0 * N))));
      const dnC = bs.nCytoplasm - bs.nMedium, dnN = bs.nNucleus - bs.nMedium;
      const dnM = bs.nMicrotubule - bs.nCytoplasm; // a tube displaces cytoplasm
      for (let g0 = 0; g0 < qSlices; g0 += group) {
        const Gs = Math.min(group, qSlices - g0);
        const vol = new Float32Array(3 * N * Gs);
        const zMax = g0 + Gs === qSlices ? zTop : (g0 + Gs) * qDz;
        world.opticalVolume(gx0, gy0, gx1, gy1, g0 * qDz, zMax, nx, ny, Gs, Math.max(1, q.sub), vol);
        const chan = N * Gs;
        for (let k = 0; k < Gs; ++k)
          for (let i = 0; i < N; ++i) {
            const v = k * N + i;
            const fc = vol[v], fn = vol[chan + v], fm = vol[2 * chan + v];
            const ph = k0 * qDz * (fc * dnC + fn * dnN + fm * dnM);
            const o = (slices === 1 ? 0 : (g0 + k) * N) + i;
            phase[o] = phase[o] + ph;
            if (slices === 1) {
              phaseSum += Math.abs(ph);
              phaseZ += Math.abs(ph) * (g0 + k + 0.5) * qDz;
            }
            if (atten) atten[o] = atten[o] * Math.exp(-0.5 * bs.absorptionPerUm * qDz * (fc + fn));
          }
      }
    }
    if (slices === 1 && phaseSum > 0.0) this.objectZ = phaseZ / phaseSum;
    // The grid is periodic: fade the specimen out over the outer half of the margin (cosine taper).
    const t = Math.floor(margin / 2);
    const ramp = (i, n) => {
      const d = Math.min(i, n - 1 - i);
      return d >= t ? 1.0 : 0.5 - 0.5 * Math.cos(kPi * (d + 0.5) / Math.max(1, t));
    };
    if (t > 0)
      for (let y = 0; y < ny; ++y) {
        const wy = ramp(y, ny);
        for (let x = 0; x < nx; ++x) {
          const wgt = wy * ramp(x, nx);
          if (wgt >= 1.0) continue;
          for (let k = 0; k < slices; ++k) {
            const i = k * N + y * nx + x;
            phase[i] = phase[i] * wgt;
            if (atten) atten[i] = Math.pow(atten[i], wgt);
          }
        }
      }
    this.finish();
    this.setupMs = performance.now() - t0;
    this.imageFocus = null;
  }

  // Sources, pupil, kz, and the thin spectrum or the per-slice transmittances + propagator.
  finish() {
    const bs = this.spec, q = this.q, nx = this.nx, ny = this.ny, N = nx * ny, k0 = this.k0;
    this.bR = new Float64Array(Math.max(nx, ny)); this.bI = new Float64Array(Math.max(nx, ny));
    // Condenser source points: equal-area Fibonacci disk, snapped to the grid.
    const ns = Math.max(1, q.sources), Lx = nx * this.pitch, Ly = ny * this.pitch;
    this.src = [];
    for (let i = 0; i < ns; ++i) {
      let kx = 0, ky = 0;
      if (ns > 1 && bs.condenserNa > 0) {
        const r = bs.condenserNa * Math.sqrt((i + 0.5) / ns), th = i * 2.399963229728653;
        kx = k0 * r * Math.cos(th);
        ky = k0 * r * Math.sin(th);
      }
      this.src.push({ mx: lround(kx * Lx / (2 * kPi)), my: lround(ky * Ly / (2 * kPi)) });
    }
    // Detection pupil and the medium's kz.
    const pupR = this.pupR = new Float32Array(N), pupI = this.pupI = new Float32Array(N);
    const kz = this.kz = new Float32Array(N).fill(-1.0);
    const kMed = k0 * bs.nMedium, kNa = k0 * bs.na;
    for (let iy = 0; iy < ny; ++iy)
      for (let ix = 0; ix < nx; ++ix) {
        const kx = 2 * kPi * freq(ix, nx) / Lx, ky = 2 * kPi * freq(iy, ny) / Ly;
        const kr2 = kx * kx + ky * ky, i = iy * nx + ix;
        if (kr2 < kMed * kMed) kz[i] = Math.sqrt(kMed * kMed - kr2);
        const rho = Math.sqrt(kr2) / kNa;
        if (rho <= 1.0 && kz[i] >= 0) {
          const w = 2 * kPi * zernikeWavefrontWaves(bs.zernike, rho, Math.atan2(ky, kx));
          pupR[i] = Math.cos(w); pupI[i] = Math.sin(w);
        }
      }
    // Transmittance per slice: polar(atten, phase) in float.
    const S = this.slices, phase = this.phase, atten = this.atten;
    const tR = this.transR = new Float32Array(N * S), tI = this.transI = new Float32Array(N * S);
    for (let i = 0; i < N * S; i++) {
      const a = atten ? atten[i] : 1.0, ph = phase[i];
      tR[i] = f32(a * f32(Math.cos(ph))); tI[i] = f32(a * f32(Math.sin(ph)));
    }
    if (S === 1) {
      // Thin object: one transmittance spectrum that every source shifts.
      this.thinR = Float64Array.from(tR); this.thinI = Float64Array.from(tI);
      fft2f(this.thinR, this.thinI, nx, ny, -1, this.bR, this.bI);
    } else {
      // The slice step dz: exp(i kz dz), 0 where evanescent.
      this.propR = new Float32Array(N); this.propI = new Float32Array(N);
      for (let i = 0; i < N; i++)
        if (kz[i] >= 0) { const a = f32(kz[i] * this.dz); this.propR[i] = Math.cos(a); this.propI[i] = Math.sin(a); }
    }
  }

  // Source s's plane wave propagated down through the slices (top first): screen, then dz to the next screen.
  // Returns the spectrum just below the lowest screen.
  // One field buffer per scene: the sources are processed one after the other and
  // every element is written before it is read.
  fieldBuffers() {
    const N = this.nx * this.ny;
    if (!this.uR || this.uR.length !== N) { this.uR = new Float64Array(N); this.uI = new Float64Array(N); }
    return [this.uR, this.uI];
  }
  exitField(s) {
    const nx = this.nx, ny = this.ny, N = nx * ny;
    const [uR, uI] = this.fieldBuffers();
    const exR = new Float32Array(nx), exI = new Float32Array(nx);
    for (let x = 0; x < nx; ++x) { const a = f32(2 * kPi * s.mx * x / nx); exR[x] = Math.cos(a); exI[x] = Math.sin(a); }
    for (let y = 0; y < ny; ++y) {
      const a = f32(2 * kPi * s.my * y / ny), eyR = f32(Math.cos(a)), eyI = f32(Math.sin(a));
      for (let x = 0; x < nx; ++x) {
        const i = y * nx + x;
        uR[i] = f32(f32(exR[x] * eyR) - f32(exI[x] * eyI));
        uI[i] = f32(f32(exR[x] * eyI) + f32(exI[x] * eyR));
      }
    }
    const scale = f32(1.0 / (f32(nx) * f32(ny)));
    for (let k = this.slices - 1; k >= 0; --k) {
      const o = k * N;
      for (let i = 0; i < N; ++i) cmulInto(uR, uI, i, this.transR[o + i], this.transI[o + i]);
      fft2f(uR, uI, nx, ny, -1, this.bR, this.bI);
      if (k === 0) break;
      for (let i = 0; i < N; ++i) cmulInto(uR, uI, i, this.propR[i], this.propI[i]);
      fft2f(uR, uI, nx, ny, +1, this.bR, this.bI);
      for (let i = 0; i < N; ++i) { uR[i] = f32(uR[i] * scale); uI[i] = f32(uI[i] * scale); }
    }
    return [uR, uI];
  }

  sourceImage(s, dR, dI, camOut, off) {
    const nx = this.nx, ny = this.ny, N = nx * ny, sp = this.src[s];
    let uR, uI;
    if (this.slices === 1) {
      // E(k) = T(k - k_s): a circular shift of the transmittance spectrum (the column index tabulated).
      [uR, uI] = this.fieldBuffers();
      const sxs = new Int32Array(nx);
      for (let x = 0; x < nx; ++x) sxs[x] = (((x - sp.mx) % nx) + nx) % nx;
      for (let y = 0; y < ny; ++y) {
        const sy = (((y - sp.my) % ny) + ny) % ny, so = sy * nx, o = y * nx;
        for (let x = 0; x < nx; ++x) { uR[o + x] = this.thinR[so + sxs[x]]; uI[o + x] = this.thinI[so + sxs[x]]; }
      }
    } else [uR, uI] = this.exitField(sp);
    // Pupil and defocus to the focal plane, zero where evanescent.
    for (let i = 0; i < N; ++i) cmulInto(uR, uI, i, dR[i], dI[i]);
    fft2f(uR, uI, nx, ny, +1, this.bR, this.bI);
    const scale = f32(1.0 / (f32(nx) * f32(ny)));
    const W = this.spec.width, H = this.spec.height, up = this.up, m = this.margin;
    const norm = f32(1.0 / (up * up));
    for (let j = 0; j < H; ++j)
      for (let i = 0; i < W; ++i) {
        let acc = 0.0;
        for (let b = 0; b < up; ++b)
          for (let a = 0; a < up; ++a) {
            const v = (m + j * up + b) * nx + m + i * up + a;
            const re = f32(uR[v] * scale), im = f32(uI[v] * scale);
            acc = f32(acc + f32(f32(re * re) + f32(im * im)));
          }
        camOut[off + j * W + i] = f32(acc * norm);
      }
  }

  // Transmitted intensity per camera pixel (1 = the empty field), focal plane at focusUm above the coverslip.
  image(focusUm) {
    if (this.imageFocus === focusUm) return this.img;
    const t0 = performance.now();
    const N = this.nx * this.ny, P = this.spec.width * this.spec.height, ns = this.src.length;
    // From the lowest screen (objectZ above the coverslip) down to the focal plane.
    const d = this.objectZ - focusUm;
    const dR = new Float32Array(N), dI = new Float32Array(N);
    for (let i = 0; i < N; ++i) {
      if (!(this.kz[i] >= 0)) continue;
      const a = f32(this.kz[i] * d), cR = f32(Math.cos(a)), cI = f32(Math.sin(a));
      const pr = this.pupR[i], pi = this.pupI[i];
      dR[i] = f32(f32(pr * cR) - f32(pi * cI)); dI[i] = f32(f32(pr * cI) + f32(pi * cR));
    }
    const slots = new Float32Array(P * ns);
    for (let s = 0; s < ns; ++s) this.sourceImage(s, dR, dI, slots, s * P);
    const img = new Float32Array(P);
    for (let s = 0; s < ns; ++s) for (let i = 0; i < P; ++i) img[i] = img[i] + slots[s * P + i];
    const inv = f32(1.0 / ns);
    for (let i = 0; i < P; ++i) img[i] = img[i] * inv;
    this.img = img; this.imageFocus = focusUm;
    this.imageMs = performance.now() - t0;
    return img;
  }
}

// One scene kept across movies (speed only, like the C++ MovieCache).
let sceneMemo = null;
export function brightfieldScene(world, bs) {
  const key = JSON.stringify(bs);
  if (!sceneMemo || sceneMemo.world !== world || sceneMemo.key !== key) sceneMemo = { world, key, scene: new BrightfieldScene(world, bs) };
  return sceneMemo.scene;
}

// RenderBrightfieldMovie: the image at the spec's focus x bf-photons-per-px-per-sec x exposure, then the camera
// noise per frame (the specimen does not change between frames).
export function renderBrightfieldMovie(P, spec, S, onFrame, opts = {}) {
  const t0 = performance.now();
  const world = scopeWorld(P, S);
  const bs = scopeBrightfieldSpec(spec, S);
  const scene = brightfieldScene(world, bs);
  const focusUm = S.q.zCullCentreUm;
  const trans = scene.image(focusUm);
  const flux = Math.max(0.0, S.O('bf-photons-per-px-per-sec')) * S.expSec;
  const photons = new Float32Array(trans.length);
  for (let i = 0; i < trans.length; ++i) photons[i] = trans[i] * flux;
  const maps = noiseMaps(S.seed, S.W, S.H, S.cam);
  for (let f = 0; f < S.N; f++) {
    if (onFrame(f, applyNoiseChain(photons, S.cam, maps, f), photons) === false) break;
    if (opts.onProgress) opts.onProgress('frames', (f + 1) / S.N);
  }
  return { width: S.W, height: S.H, frames: S.N, quality: bs.quality, sources: scene.src.length, slices: scene.slices,
    grid: [scene.nx, scene.ny], focusUm, objectZUm: scene.objectZ, setupMs: scene.setupMs, imageMs: scene.imageMs,
    totalSec: (performance.now() - t0) / 1000, transmission: opts.keepImages ? trans : undefined };
}
