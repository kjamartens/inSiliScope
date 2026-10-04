// PSF of the imaging path: a mirror of
//   ZernikePsf.cpp          GibsonLanniZernike planes (scalar Gibson-Lanni pupil + Zernike phase + optional
//                           double-helix mask, separable chirp-Z on a 64 x 64 pupil)
//   SMLMZernike.cpp         presets, coefficient formatting
//   FftRadix2.h             radix-2 FFT (same recurrence)
//   PsfGeneratorBridge.cpp  PsfKernelHalfWidthPx, NearestZIndex, block sums, SplatSetup/PlanSplat/SplatRows,
//                           Fft placement (FftShiftKernelTile)
// all in adapter/inSiliScope/Simulation/. Same operand order as the C++ (double), floats where it stores
// floats (Math.fround), so kernels agree to rounding.

export const ZERNIKE_PRESETS = ['None', 'AstigmatismWeak', 'AstigmatismModerate', 'AstigmatismStrong', 'ComaWeak',
  'ComaStrong', 'SphericalWeak', 'SphericalStrong', 'TrefoilModerate', 'MixedRealisticObjective', 'SaddlePoint',
  'ExtendedRange', 'ExtendedRangeStrong'];
export const NUM_ZERNIKE = 28;
const PRESET_MODES = {
  AstigmatismWeak: [[5, 0.07]], AstigmatismModerate: [[5, 0.15]], AstigmatismStrong: [[5, 0.30]],
  ComaWeak: [[7, 0.10]], ComaStrong: [[7, 0.25]], SphericalWeak: [[12, 0.10]], SphericalStrong: [[12, 0.25]],
  TrefoilModerate: [[6, 0.15]], MixedRealisticObjective: [[4, 0.05], [5, 0.08], [7, 0.06], [12, 0.07]],
  SaddlePoint: [[5, 0.6], [13, 0.2]], ExtendedRange: [[5, 1.0], [13, 0.4]],
  ExtendedRangeStrong: [[5, 1.8], [13, 0.8], [25, 0.3]],
};
export function zernikePresetCoefficients(name) {
  const z = new Array(NUM_ZERNIKE).fill(0);
  for (const [j, v] of PRESET_MODES[name] || []) z[j] = v;
  return z;
}
// FormatZernikeCoefficients writes with ostream's default precision (6 significant digits) and the PSF
// parses that text back: keep the same rounding.
export const roundZernike = v => Number(Number(v).toPrecision(6));

export const PSF_INTERP = { Nearest: 0, Linear: 1, Cubic: 2, Fft: 3 };

// ---- FFT (FftRadix2.h) ----
export function makeTwiddles(n, sign) {
  const c = new Float64Array(Math.max(0, n - 1)), s = new Float64Array(c.length);
  for (let len = 2; len <= n; len <<= 1) {
    const ang = sign * 2.0 * Math.PI / len;
    const wr = Math.cos(ang), wi = Math.sin(ang);
    const half = len / 2;
    let cr = 1.0, ci = 0.0;
    for (let k = 0; k < half; ++k) {
      c[half - 1 + k] = cr; s[half - 1 + k] = ci;
      const nr = cr * wr - ci * wi;
      ci = cr * wi + ci * wr;
      cr = nr;
    }
  }
  return { n, c, s };
}

// In place on re/im (offset o, contiguous n).
export function fft1d(re, im, tw, o = 0) {
  const n = tw.n;
  for (let i = 1, j = 0; i < n; ++i) {
    let bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) {
      let t = re[o + i]; re[o + i] = re[o + j]; re[o + j] = t;
      t = im[o + i]; im[o + i] = im[o + j]; im[o + j] = t;
    }
  }
  const tc = tw.c, ts = tw.s;
  for (let len = 2; len <= n; len <<= 1) {
    const half = len / 2, b0 = half - 1;
    for (let i = 0; i < n; i += len) {
      const a = o + i, b = o + i + half;
      for (let k = 0; k < half; ++k) {
        const cr = tc[b0 + k], ci = ts[b0 + k];
        const vr = re[b + k] * cr - im[b + k] * ci, vi = re[b + k] * ci + im[b + k] * cr;
        re[b + k] = re[a + k] - vr; im[b + k] = im[a + k] - vi;
        re[a + k] += vr; im[a + k] += vi;
      }
    }
  }
}

// ---- Zernike / mask (ZernikePsf.cpp) ----
function indexToNM(j) {
  for (let nn = 0; ; ++nn) for (let l = 0; l <= nn; ++l) if (nn * (nn + 1) / 2 + l === j) return [nn, -nn + 2 * l];
}
function binomial(a, b) {
  if (b < 0 || b > a) return 0.0;
  let r = 1.0;
  for (let i = 0; i < b; ++i) r = r * (a - i) / (i + 1);
  return r;
}
function zernikeRadial(n, m, rho) {
  let r = 0.0;
  for (let k = 0; k <= Math.trunc((n - m) / 2); ++k) {
    const coeff = (k % 2 === 0 ? 1.0 : -1.0) * binomial(n - k, k) * binomial(n - 2 * k, Math.trunc((n - m) / 2) - k);
    r += coeff * Math.pow(rho, n - 2 * k);
  }
  return r;
}
function zernikeValue(z, rho, phi) {
  const radial = rho <= 1.0 ? zernikeRadial(z.n, z.m, rho) : 0.0;
  return radial * (z.l >= 0 ? Math.cos(z.m * phi) : Math.sin(z.m * phi));
}
// ZernikeWavefrontWaves: the pupil wavefront sum_j c_j Z_j(rho, phi), waves (the BrightField detection pupil).
export function zernikeWavefrontWaves(coeffs, rho, phi) {
  let w = 0.0;
  for (let j = 0; j < coeffs.length; ++j) {
    if (coeffs[j] === 0.0) continue;
    const [n, l] = indexToNM(j);
    w += coeffs[j] * zernikeValue({ n, l, m: Math.abs(l) }, rho, phi);
  }
  return w;
}
function laguerreL(p, a, x) {
  let lm1 = 0.0, l = 1.0;
  for (let k = 0; k < p; ++k) {
    const next = ((2 * k + 1 + a - x) * l - (k + a) * lm1) / (k + 1);
    lm1 = l; l = next;
  }
  return l;
}
function maskPhase(doubleHelix, maskModes, maskWaist, rhoNorm, phi) {
  if (!doubleHelix) return 0.0;
  const n = Math.max(2, maskModes), w = Math.max(0.2, maskWaist);
  const u = Math.max(0.0, Math.min(1.0, rhoNorm)) / w, u2 = u * u;
  let re = 0.0, im = 0.0;
  for (let p = 0; p < n; ++p) {
    const l = 2 * p + 1;
    const amp = Math.pow(u, l) * Math.exp(-u2) * laguerreL(p, l, 2.0 * u2);
    re += amp * Math.cos(l * phi); im += amp * Math.sin(l * phi);
  }
  return (re === 0.0 && im === 0.0) ? 0.0 : Math.atan2(im, re);
}

// Bluestein chirp-Z with every input-independent factor tabulated (CztPlan).
class CztPlan {
  constructor(M, dk, k0, P, dx, x0) {
    this.M = M; this.P = P;
    const theta = dk * dx;
    let L = 1;
    while (L < M + P - 1) L <<= 1;
    this.L = L;
    this.fwd = makeTwiddles(L, -1); this.inv = makeTwiddles(L, 1);
    this.preC = new Float64Array(M); this.preS = new Float64Array(M);
    for (let m = 0; m < M; ++m) { const ang = m * dk * x0 + theta * m * m / 2.0; this.preC[m] = Math.cos(ang); this.preS[m] = Math.sin(ang); }
    this.gRe = new Float64Array(L); this.gIm = new Float64Array(L);
    for (let n = -(M - 1); n < P; ++n) {
      const ang = -theta * n * n / 2.0, idx = n >= 0 ? n : L + n;
      this.gRe[idx] = Math.cos(ang); this.gIm[idx] = Math.sin(ang);
    }
    fft1d(this.gRe, this.gIm, this.fwd);
    this.c1 = new Float64Array(P); this.s1 = new Float64Array(P); this.c2 = new Float64Array(P); this.s2 = new Float64Array(P);
    for (let p = 0; p < P; ++p) {
      const a1 = theta * p * p / 2.0;
      this.c1[p] = Math.cos(a1); this.s1[p] = Math.sin(a1);
      const a2 = k0 * (x0 + p * dx);
      this.c2[p] = Math.cos(a2); this.s2[p] = Math.sin(a2);
    }
  }
  apply(inRe, inIm, io, outRe, outIm, aRe, aIm) {
    const { M, P, L } = this;
    aRe.fill(0); aIm.fill(0);
    for (let m = 0; m < M; ++m) {
      const cr = this.preC[m], ci = this.preS[m];
      aRe[m] = inRe[io + m] * cr - inIm[io + m] * ci;
      aIm[m] = inRe[io + m] * ci + inIm[io + m] * cr;
    }
    fft1d(aRe, aIm, this.fwd);
    for (let i = 0; i < L; ++i) {
      const gr = this.gRe[i], gi = this.gIm[i];
      const re = aRe[i] * gr - aIm[i] * gi, im = aRe[i] * gi + aIm[i] * gr;
      aRe[i] = re; aIm[i] = im;
    }
    fft1d(aRe, aIm, this.inv);
    for (let p = 0; p < P; ++p) {
      const convRe = aRe[p] / L, convIm = aIm[p] / L;
      const cc1 = this.c1[p], ss1 = this.s1[p];
      const sRe = convRe * cc1 - convIm * ss1, sIm = convRe * ss1 + convIm * cc1;
      const cc2 = this.c2[p], ss2 = this.s2[p];
      outRe[p] = sRe * cc2 - sIm * ss2;
      outIm[p] = sRe * ss2 + sIm * cc2;
    }
  }
}

const FFT_M = 64;

export function psfGeometry(req) {
  const oversampling = Math.max(1, req.oversampling);
  const camHalf = Math.max(1, req.kernelHalfWidthPx);
  const halfOv = camHalf * oversampling;
  const nzWanted = Math.max(3, req.nz);
  return { oversampling, halfOv, size: 2 * halfOv + 1, nz: nzWanted % 2 === 1 ? nzWanted : nzWanted + 1,
    resLateralNm: req.pixelSizeNm / oversampling };
}

// Raw intensity planes (Float32Array size*size each), plane k at defocus (k - (nz-1)/2) zStep.
// onPlane(k, nz) is called after each plane (progress).
export function computeZernikePsfPlanes(req, onPlane) {
  const g = psfGeometry(req);
  if (!(req.na > 0) || !(req.wavelengthNm > 0) || !(req.immersionIndex > 0) || !(req.sampleIndex > 0) ||
      !(req.pixelSizeNm > 0) || !(req.zStepNm > 0))
    throw new Error('GibsonLanniZernike PSF: NA, wavelength, refractive indices, pixel size and z step must be > 0.');
  const NA = req.na, lambda = req.wavelengthNm * 1E-9, ni = req.immersionIndex, ns = req.sampleIndex;
  const ti0 = req.workingDistanceUm * 1E-6, particleAxialPosition = req.sampleDepthNm * 1E-9;
  const resAxialM = req.zStepNm * 1E-9, resLateralM = g.resLateralNm * 1E-9;
  const doubleHelix = req.maskType === 1;
  const modes = [];
  req.zernike.forEach((c, j) => {
    const v = roundZernike(c);
    if (v === 0) return;
    const [n, l] = indexToNM(j);
    modes.push([v, { n, l, m: Math.abs(l) }]);
  });
  const k0 = 2.0 * Math.PI / lambda;
  const bMax = Math.min(1.0, ns / NA);
  const kMax = k0 * NA * bMax;
  const dk = (2.0 * kMax) / (FFT_M - 4);
  const kMin = -Math.floor(FFT_M / 2.0) * dk;
  const nx = g.size, ny = g.size;
  const x0m = -((nx - 1) / 2.0) * resLateralM;
  const plan = new CztPlan(FFT_M, dk, kMin, nx, resLateralM, x0m);
  const M = FFT_M, c0 = FFT_M / 2, MM = M * M;
  const inside = new Uint8Array(MM), opd1Tab = new Float64Array(MM), opd3Factor = new Float64Array(MM);
  const zernTab = new Float64Array(MM), maskTab = new Float64Array(MM);
  for (let iy = 0; iy < M; ++iy) {
    const ky = (iy - c0) * dk;
    for (let ix = 0; ix < M; ++ix) {
      const kx = (ix - c0) * dk;
      const kr2 = kx * kx + ky * ky;
      if (kr2 > kMax * kMax) continue;
      const idx = iy * M + ix;
      inside[idx] = 1;
      const kr = Math.sqrt(kr2), rho = kr / (k0 * NA), phi = Math.atan2(ky, kx);
      const s1 = NA * rho / ns, s3 = NA * rho / ni;
      opd1Tab[idx] = ns * particleAxialPosition * Math.sqrt(Math.max(0.0, 1.0 - s1 * s1));
      opd3Factor[idx] = Math.sqrt(Math.max(0.0, 1.0 - s3 * s3));
      let zp = 0.0;
      for (const [c, z] of modes) zp += c * zernikeValue(z, rho / bMax, phi);
      zp *= 2.0 * Math.PI;
      zernTab[idx] = zp;
      maskTab[idx] = maskPhase(doubleHelix, req.maskModes, req.maskWaist, rho / bMax, phi);
    }
  }
  const nz = g.nz, out = [];
  const focalShift = particleAxialPosition * (ni / ns);
  const pupilRe = new Float64Array(MM), pupilIm = new Float64Array(MM);
  const aRe = new Float64Array(plan.L), aIm = new Float64Array(plan.L);
  const midRe = new Float64Array(M * ny), midIm = new Float64Array(M * ny);
  const lineRe = new Float64Array(M), lineIm = new Float64Array(M);
  const oRe = new Float64Array(Math.max(nx, ny)), oIm = new Float64Array(oRe.length);
  for (let z = 0; z < nz; ++z) {
    const ti = (ti0 - focalShift) + resAxialM * (z - (nz - 1.0) / 2.0);
    pupilRe.fill(0); pupilIm.fill(0);
    for (let idx = 0; idx < MM; ++idx) {
      if (!inside[idx]) continue;
      const opd3 = ni * (ti - ti0) * opd3Factor[idx];
      const phase = k0 * (opd1Tab[idx] + opd3) + zernTab[idx] + maskTab[idx];
      pupilRe[idx] = Math.cos(phase); pupilIm[idx] = Math.sin(phase);
    }
    midRe.fill(0); midIm.fill(0);
    for (let m = 0; m < M; ++m) {
      let any = false;
      for (let n = 0; n < M; ++n) { lineRe[n] = pupilRe[n * M + m]; lineIm[n] = pupilIm[n * M + m]; any = any || inside[n * M + m] === 1; }
      if (!any) continue;
      plan.apply(lineRe, lineIm, 0, oRe, oIm, aRe, aIm);
      for (let y = 0; y < ny; ++y) { midRe[y * M + m] = oRe[y]; midIm[y * M + m] = oIm[y]; }
    }
    const slice = new Float32Array(nx * ny);
    for (let y = 0; y < ny; ++y) {
      plan.apply(midRe, midIm, y * M, oRe, oIm, aRe, aIm);
      for (let x = 0; x < nx; ++x) slice[y * nx + x] = oRe[x] * oRe[x] + oIm[x] * oIm[x];
    }
    out.push(slice);
    if (onPlane) onPlane(z, nz);
  }
  return out;
}

// webSMLM's buildSummedKernel: os x os block sums, offset os-1, width n + os - 1.
export function buildBlockSums(kernel, n, os) {
  const off = os - 1, bw = n + off;
  const hsum = new Float64Array(bw * n);
  for (let y = 0; y < n; ++y)
    for (let bi = 0; bi < bw; ++bi) {
      const b = bi - off, x0 = Math.max(0, b), x1 = Math.min(n - 1, b + off);
      let acc = 0.0;
      for (let x = x0; x <= x1; ++x) acc += kernel[y * n + x];
      hsum[y * bw + bi] = acc;
    }
  const out = new Float32Array(bw * bw);
  for (let ai = 0; ai < bw; ++ai) {
    const a = ai - off, y0 = Math.max(0, a), y1 = Math.min(n - 1, a + off);
    for (let bi = 0; bi < bw; ++bi) {
      let acc = 0.0;
      for (let y = y0; y <= y1; ++y) acc += hsum[y * bw + bi];
      out[ai * bw + bi] = acc;
    }
  }
  return out;
}

// Sum-1 planes + block sums (BuildZernikePsfKernelCache).
export function buildZernikeKernelCache(req, onPlane) {
  const planes = computeZernikePsfPlanes(req, onPlane);
  const g = psfGeometry(req);
  const cache = { valid: true, oversampling: g.oversampling, halfWidthOversampled: g.halfOv, sizeOversampled: g.size,
    nz: g.nz, zStepNm: req.zStepNm, interpMode: req.interpMode, planes, blockSums: [], blockSumWidth: g.size + g.oversampling - 1 };
  for (const plane of planes) {
    let sum = 0.0;
    for (let i = 0; i < plane.length; i++) sum += plane[i];
    if (sum > 0.0) for (let i = 0; i < plane.length; i++) plane[i] = plane[i] / sum;
    cache.blockSums.push(buildBlockSums(plane, g.size, g.oversampling));
  }
  return cache;
}

export function psfKernelHalfWidthPx(halfWidthNm, pixelSizeNm, wavelengthNm, na) {
  let requested = lround(halfWidthNm / pixelSizeNm);
  requested = Math.max(requested, 1);
  const naSafe = na > 0.0 ? na : 0.01;
  const rayleighRadiusNm = 0.61 * wavelengthNm / naSafe;
  let minHalf = Math.ceil(3.0 * rayleighRadiusNm / pixelSizeNm);
  minHalf = Math.min(Math.max(minHalf, 2), 48);
  return Math.max(requested, minHalf);
}

// std::lround: half away from zero.
export const lround = x => (x < 0 ? -Math.round(-x) : Math.round(x));

export function nearestZIndex(cache, zUm) {
  if (cache.nz <= 1 || cache.zStepNm <= 0.0) return 0;
  const idx = lround(zUm * 1000.0 / cache.zStepNm + (cache.nz - 1) / 2.0);
  return Math.min(cache.nz - 1, Math.max(0, idx));
}

// ---- placement ----
function catmullRomWeights(f) {
  const f2 = f * f, f3 = f2 * f;
  return [-0.5 * f3 + f2 - 0.5 * f, 1.5 * f3 - 2.5 * f2 + 1.0, -1.5 * f3 + 2.0 * f2 + 0.5 * f, 0.5 * f3 - 0.5 * f2];
}
const roundHalfUp = v => Math.floor(v + 0.5);

export function splatSetup(cache, xPx, yPx, interpMode) {
  const os = Math.max(1, cache.oversampling);
  const kc = (cache.sizeOversampled - 1) / 2.0;
  const r = { x0: roundHalfUp(xPx), y0: roundHalfUp(yPx), bx: 0, by: 0, nTaps: 1, wx: [1, 0, 0, 0], wy: [1, 0, 0, 0] };
  const tx = kc + (r.x0 - xPx - 0.5) * os + 0.5;
  const ty = kc + (r.y0 - yPx - 0.5) * os + 0.5;
  if (interpMode === PSF_INTERP.Nearest || interpMode === PSF_INTERP.Fft) {
    r.bx = Math.floor(tx + 0.5); r.by = Math.floor(ty + 0.5); return r;
  }
  const bx = Math.floor(tx), by = Math.floor(ty), fx = tx - bx, fy = ty - by;
  if (interpMode === PSF_INTERP.Cubic) {
    r.bx = bx - 1; r.by = by - 1; r.nTaps = 4; r.wx = catmullRomWeights(fx); r.wy = catmullRomWeights(fy); return r;
  }
  r.bx = bx; r.by = by; r.nTaps = 2; r.wx = [1.0 - fx, fx, 0, 0]; r.wy = [1.0 - fy, fy, 0, 0];
  return r;
}

// FftShiftKernelTile: shift an n x n tile by (shiftX, shiftY) kernel cells (Fourier shift theorem, row then
// column 1D passes on a zero-padded line).
let fftShiftTw = null;
export function fftShiftKernelTile(kernel, n, shiftX, shiftY) {
  let N = 1;
  while (N < 2 * n) N <<= 1;
  const half = N / 2;
  const table = shift => {
    const pc = new Float64Array(N), ps = new Float64Array(N);
    for (let k = 0; k < N; ++k) {
      const kk = k < half ? k : k - N;
      const ang = 2.0 * Math.PI * kk * shift / N;
      pc[k] = Math.cos(ang); ps[k] = Math.sin(ang);
    }
    return [pc, ps];
  };
  const [xc, xs] = table(shiftX), [yc, ys] = table(shiftY);
  if (!fftShiftTw || fftShiftTw[0].n !== N) fftShiftTw = [makeTwiddles(N, -1), makeTwiddles(N, 1)];
  const [fwd, inv] = fftShiftTw;
  const re = new Float64Array(N), im = new Float64Array(N);
  const line = (pc, ps) => {
    fft1d(re, im, fwd);
    for (let k = 0; k < N; ++k) { const r = re[k], i = im[k]; re[k] = r * pc[k] - i * ps[k]; im[k] = r * ps[k] + i * pc[k]; }
    fft1d(re, im, inv);
    const norm = 1.0 / N;
    for (let k = 0; k < N; ++k) { re[k] *= norm; im[k] *= norm; }
  };
  const midRe = new Float64Array(n * n), midIm = new Float64Array(n * n);
  for (let y = 0; y < n; ++y) {
    re.fill(0); im.fill(0);
    for (let x = 0; x < n; ++x) re[x] = kernel[y * n + x];
    line(xc, xs);
    for (let x = 0; x < n; ++x) { midRe[y * n + x] = re[x]; midIm[y * n + x] = im[x]; }
  }
  const out = new Float32Array(n * n);
  for (let x = 0; x < n; ++x) {
    re.fill(0); im.fill(0);
    for (let y = 0; y < n; ++y) { re[y] = midRe[y * n + x]; im[y] = midIm[y * n + x]; }
    line(yc, ys);
    for (let y = 0; y < n; ++y) out[y * n + x] = re[y];
  }
  return out;
}

// ApplySplatCutoff: per plane, the splat's half-width in camera pixels for `cutoff`
// (0: the whole window): the farthest block sum at or above cutoff x the plane's peak,
// in camera pixels from the centre, rounded up, plus one. Returns the cache (radii set).
export function applySplatCutoff(cache, cutoff) {
  if (!cache.valid || cache.radiiCutoff === cutoff) return cache;
  const os = Math.max(1, cache.oversampling), camRad = Math.trunc(cache.halfWidthOversampled / os), bw = cache.blockSumWidth;
  const radii = new Int32Array(cache.nz).fill(camRad);
  cache.radii = radii; cache.radiiCutoff = cutoff;
  if (!(cutoff > 0.0) || cache.blockSums.length !== cache.nz || bw <= 0) return cache;
  const half = (bw - 1) / 2.0;
  for (let z = 0; z < cache.nz; z++) {
    const B = cache.blockSums[z];
    if (B.length !== bw * bw) continue;
    let peak = 0.0;
    for (let i = 0; i < B.length; i++) if (B[i] > peak) peak = B[i];
    const thr = cutoff * peak;
    let maxDist = 0.0;   // camera pixels: the farthest block at or above the threshold
    for (let a = 0; a < bw; a++) {
      const da = Math.abs(a - half), row = a * bw;
      for (let b = 0; b < bw; b++) if (B[row + b] >= thr) { const d = Math.max(da, Math.abs(b - half)); if (d > maxDist) maxDist = d; }
    }
    radii[z] = Math.min(camRad, Math.ceil(maxDist / os) + 1);
  }
  return cache;
}
const planeRadius = (cache, zIndex) => cache.radii && zIndex < cache.radii.length ? cache.radii[zIndex]
  : Math.trunc(cache.halfWidthOversampled / Math.max(1, cache.oversampling));

// PlanSplat: setup + the block sums to read (Fft: of the shifted plane) + the plane's
// splat half-width. null if nothing to draw.
export function planSplat(cache, zIndex, xPx, yPx, totalPhotons, interpMode) {
  if (!cache.valid || totalPhotons <= 0.0) return null;
  if (zIndex < 0 || zIndex >= cache.nz) return null;
  const os = Math.max(1, cache.oversampling), n = cache.sizeOversampled, camRad = planeRadius(cache, zIndex);
  if (interpMode === PSF_INTERP.Fft) {
    const st = splatSetup(cache, xPx, yPx, PSF_INTERP.Nearest);
    const kc = (n - 1) / 2.0;
    const tx = kc + (st.x0 - xPx - 0.5) * os + 0.5, ty = kc + (st.y0 - yPx - 0.5) * os + 0.5;
    const rx = roundHalfUp(tx), ry = roundHalfUp(ty);
    const shifted = fftShiftKernelTile(cache.planes[zIndex], n, tx - rx, ty - ry);
    st.bx = rx; st.by = ry;
    return { st, B: buildBlockSums(shifted, n, os), camRad };
  }
  return { st: splatSetup(cache, xPx, yPx, interpMode), B: cache.blockSums[zIndex], camRad };
}

// SplatRows over rows [rowLo, rowHi): img (Float32Array) += photons x interpolated block sums.
export function splatRows(img, width, height, rowLo, rowHi, cache, plan, totalPhotons) {
  const yLo = Math.max(0, rowLo), yHi = Math.min(height, rowHi);
  const os = Math.max(1, cache.oversampling);
  const off = os - 1, bw = cache.blockSumWidth;
  const { st, B, camRad } = plan, NT = st.nTaps, wx = st.wx, wy = st.wy;
  const dyLo = Math.max(-camRad, yLo - st.y0), dyHi = Math.min(camRad, yHi - 1 - st.y0);
  const dxLo = Math.max(-camRad, -st.x0), dxHi = Math.min(camRad, width - 1 - st.x0);
  for (let dy = dyLo; dy <= dyHi; ++dy) {
    const r0 = st.by + dy * os + off;
    const jLo = Math.max(0, -r0), jHi = Math.min(NT, bw - r0);
    const rowOut = (st.y0 + dy) * width;
    for (let dx = dxLo; dx <= dxHi; ++dx) {
      const c0 = st.bx + dx * os + off;
      const iLo = Math.max(0, -c0), iHi = Math.min(NT, bw - c0);
      let sum = 0.0;
      for (let j = jLo; j < jHi; ++j) {
        const brow = (r0 + j) * bw + c0;
        let row = 0.0;
        for (let i = iLo; i < iHi; ++i) row += wx[i] * B[brow + i];
        sum += wy[j] * row;
      }
      img[rowOut + st.x0 + dx] += Math.fround(totalPhotons * sum);
    }
  }
}
