// WideField imaging in JS: the mirror of the CPU path of adapter/inSiliScope/Simulation/WidefieldRender.cpp
// (+ ScopeMovie.cpp WidefieldMovie, Illumination.cpp). Same model and the same discretisation:
//   dyes binned into world-anchored z planes on 64-cell world tiles (WidefieldDyeTiles) of an upscaled
//   grid (GridSpecFor), each plane deposited on its two nearest PSF planes with linear weights
//   (PlanFocus), convolved by FFT (FastSize grid, 2^a 3^b 5^c), shifted to the camera's sub-cell
//   position by a Fourier phase ramp, cropped, max(0, .) per cell, binned; exact per-frame bleaching
//   integral (WidefieldBleachingPhotons) with the one-group basis of a square illumination.
// Not mirrored (speed only in the C++): spectra caches, focus bands (gated off for diffraction PSFs),
// dye-tile LRU, GPU. The C++ convolves in float32; this in float64 (agreement ~1e-6 relative).
import { scopeKernel, scopeWorld } from './scope_movie.js';
import { noiseMaps, applyNoiseChain } from './render.js';
import { driftMaxXyNm, driftFocusGrid, driftGridDzNm, driftGridWeights } from './drift.js';

const AVOGADRO = 6.02214076e23;
const COLUMN_MIN_UM = -5.0, COLUMN_MAX_UM = 50.0;
const TILE = 64;

export const phot = {
  crossSectionUm2: e => Math.log(10.0) * 1000.0 * e.extinctionCoeff / AVOGADRO * 1e8,
  emissionRatePerSec: (e, I) => e.quantumYield * phot.crossSectionUm2(e) * e.excitationPhotonsPerUm2PerSec * I,
  halfTimeSec(e, I) {
    const k = phot.emissionRatePerSec(e, I);
    return !(e.photonBudget > 0) || !(k > 0) ? Infinity : e.photonBudget * Math.log(2.0) / k;
  },
};
export const collectionEfficiency = (na, n) => {
  const r = Math.min(1.0, Math.max(0.0, na / Math.max(1e-6, n)));
  return 0.5 * (1.0 - Math.sqrt(1.0 - r * r));
};
export function bleachingPhotons(eta, budget, d0, dD) {
  if (!(budget > 0.0)) return eta * dD;
  return eta * budget * Math.exp(-d0 / budget) * (1.0 - Math.exp(-dD / budget));
}
// TODO(human) in the C++ too: the Gaussian WideField PSF ignores defocus (in-focus sigma, 0.21 lambda / NA).
export const gaussianSigmaUm = (defocusUm, lambdaNm, na) => 0.21 * lambdaNm / Math.max(0.01, na) / 1000.0;

const snapFloor = x => { const r = Math.round(x); return Math.abs(x - r) < 1e-9 ? r : Math.floor(x); };
const floorDiv = (a, b) => Math.floor(a / b);

// ---- PSF providers ----
function gaussianPsf(pitch, lambdaNm, na, step = 0.1) {
  const sigma = p => gaussianSigmaUm(p * step, lambdaNm, na);
  return {
    planeCoord: d => d / step, minPlane: -1000000, maxPlane: 1000000,
    radius(pLo, pHi) {
      let s = Math.max(sigma(pLo), sigma(pHi));
      if (pLo <= 0 && pHi >= 0) s = Math.max(s, sigma(0));
      return Math.max(1, Math.ceil(3.0 * s / pitch));
    },
    kernel(p, R) {
      const D = 2 * R + 1, out = new Float32Array(D * D), k = new Float64Array(D * D);
      const s = sigma(p) / pitch, twoS2 = 2.0 * s * s;
      let sum = 0.0;
      for (let y = -R; y <= R; ++y) for (let x = -R; x <= R; ++x) { const v = Math.exp(-(x * x + y * y) / twoS2); k[(y + R) * D + x + R] = v; sum += v; }
      for (let i = 0; i < k.length; i++) out[i] = k[i] / sum;
      return out;
    },
  };
}
export function validUpscale(os, requested) {
  os = Math.max(1, os);
  for (let u = Math.max(1, Math.min(requested, os)); u > 1; --u) if (os % u === 0) return u;
  return 1;
}
function kernelPsf(c, upscale) {
  const os = Math.max(1, c.oversampling), r = Math.max(1, Math.trunc(os / validUpscale(os, upscale)));
  const kc = (c.sizeOversampled - 1) / 2.0, s0 = Math.floor(kc - r / 2.0 + 1.0), n = c.sizeOversampled;
  return {
    planeCoord: d => (c.nz <= 1 || !(c.zStepNm > 0)) ? 0.0 : d * 1000.0 / c.zStepNm + (c.nz - 1) / 2.0,
    minPlane: 0, maxPlane: Math.max(0, c.nz - 1),
    radius() {
      const dMin = Math.ceil((-s0 - r + 1) / r), dMax = Math.floor((n - 1 - s0) / r);
      return Math.max(1, Math.max(-dMin, dMax));
    },
    kernel(p, R) {
      const D = 2 * R + 1, out = new Float32Array(D * D);
      if (p < 0 || p >= c.planes.length) return out;
      const P = c.planes[p];
      for (let dy = -R; dy <= R; ++dy) {
        const ya = s0 + dy * r, yb = Math.min(n, ya + r);
        for (let dx = -R; dx <= R; ++dx) {
          const xa = s0 + dx * r, xb = Math.min(n, xa + r);
          let acc = 0.0;
          for (let y = Math.max(0, ya); y < yb; ++y) for (let x = Math.max(0, xa); x < xb; ++x) acc += P[y * n + x];
          out[(dy + R) * D + dx + R] = acc;
        }
      }
      return out;
    },
  };
}

// ---- mixed-radix complex FFT (2, 3, 4, 5), double ----
const planCache = new Map();
function fftPlan(n) {
  let p = planCache.get(n);
  if (p) return p;
  const factors = [];
  let m = n;
  for (const r of [4, 2, 3, 5]) while (m % r === 0) { factors.push(r); m /= r; }
  if (m !== 1) throw new Error('FFT size ' + n + ' is not 2^a 3^b 5^c');
  const wr = new Float64Array(n), wi = new Float64Array(n), wiNeg = new Float64Array(n);
  for (let k = 0; k < n; k++) { const a = -2 * Math.PI * k / n; wr[k] = Math.cos(a); wi[k] = Math.sin(a); wiNeg[k] = -wi[k]; }
  // wiNeg = -sign * wi for sign = +1 (negation is exact); tr/ti: the radix-sized scratch of the butterflies.
  p = { n, factors, wr, wi, wiNeg, sr: new Float64Array(n), si: new Float64Array(n), tr: new Float64Array(8), ti: new Float64Array(8) };
  planCache.set(n, p);
  return p;
}
// out[k] = sum_j in[j] exp(sign 2 pi i jk/n), in: (xr, xi)[off + j*stride].
function fftRec(p, xr, xi, off, stride, n, or, oi, oo, fi, tstep, sign) {
  if (n === 1) { or[oo] = xr[off]; oi[oo] = xi[off]; return; }
  const r = p.factors[fi], m = n / r;
  for (let q = 0; q < r; q++) fftRec(p, xr, xi, off + q * stride, stride * r, m, or, oi, oo + q * m, fi + 1, tstep * r, sign);
  // The children are done: the plan's scratch is free. wis = -sign * wi, per element.
  const N = p.n, tr = p.tr, ti = p.ti, wr = p.wr, wis = sign < 0 ? p.wi : p.wiNeg;
  for (let k = 0; k < m; k++) {
    for (let q = 0; q < r; q++) {
      const idx = (q * k * tstep) % N, cr = wr[idx], ci = wis[idx];
      const ar = or[oo + q * m + k], ai = oi[oo + q * m + k];
      tr[q] = ar * cr - ai * ci; ti[q] = ar * ci + ai * cr;
    }
    for (let u = 0; u < r; u++) {
      let sr = 0, si = 0;
      for (let q = 0; q < r; q++) {
        const idx = ((q * u * m) * tstep) % N, cr = wr[idx], ci = wis[idx];
        sr += tr[q] * cr - ti[q] * ci; si += tr[q] * ci + ti[q] * cr;
      }
      or[oo + u * m + k] = sr; oi[oo + u * m + k] = si;
    }
  }
}
export function fft1(re, im, off, stride, n, sign, bufR, bufI) {
  const p = fftPlan(n);
  for (let j = 0; j < n; j++) { bufR[j] = re[off + j * stride]; bufI[j] = im[off + j * stride]; }
  fftRec(p, bufR, bufI, 0, 1, n, p.sr, p.si, 0, 0, 1, sign);
  for (let j = 0; j < n; j++) { re[off + j * stride] = p.sr[j]; im[off + j * stride] = p.si[j]; }
}
export function fft2(re, im, nx, ny, sign) {
  const bR = new Float64Array(Math.max(nx, ny)), bI = new Float64Array(bR.length);
  for (let y = 0; y < ny; y++) fft1(re, im, y * nx, 1, nx, sign, bR, bI);
  for (let x = 0; x < nx; x++) fft1(re, im, x, nx, ny, sign, bR, bI);
}
export function fastSize(n, multiple) {
  for (let m = Math.max(n, 1); ; ++m) {
    if (m % multiple) continue;
    let r = m;
    for (const p of [2, 3, 5]) while (r % p === 0) r /= p;
    if (r === 1) return m;
  }
}

// ---- grid and dyes ----
function gridSpecFor(ws, sq) {
  const u = Math.max(1, ws.upscale), pitch = ws.pixelUm / u;
  const axisX = ws.originXUm + (ws.width - 1) / 2.0 * ws.pixelUm, axisY = ws.originYUm + (ws.height - 1) / 2.0 * ws.pixelUm;
  const fx0 = ws.originXUm - ws.pixelUm / 2, fy0 = ws.originYUm - ws.pixelUm / 2;
  const fx1 = fx0 + ws.width * ws.pixelUm, fy1 = fy0 + ws.height * ws.pixelUm;
  const [sx0, sy0, sx1, sy1] = [-sq.w / 2, -sq.h / 2, sq.w / 2, sq.h / 2];
  const m = Math.max(0.0, ws.marginUm);
  const ex0 = Math.min(fx0, Math.max(axisX + sx0, fx0 - m)), ex1 = Math.max(fx1, Math.min(axisX + sx1, fx1 + m));
  const ey0 = Math.min(fy0, Math.max(axisY + sy0, fy0 - m)), ey1 = Math.max(fy1, Math.min(axisY + sy1, fy1 + m));
  const g = { pitchUm: pitch, ix0: snapFloor(ex0 / pitch), iy0: snapFloor(ey0 / pitch) };
  const ix1 = Math.max(-snapFloor(-ex1 / pitch), snapFloor(fx1 / pitch) + 1);
  const iy1 = Math.max(-snapFloor(-ey1 / pitch), snapFloor(fy1 / pitch) + 1);
  g.nx = ix1 - g.ix0; g.ny = iy1 - g.iy0; g.x0Um = g.ix0 * pitch; g.y0Um = g.iy0 * pitch;
  const cx = snapFloor(fx0 / pitch), cy = snapFloor(fy0 / pitch);
  g.fovX = cx - g.ix0; g.fovY = cy - g.iy0;
  g.fracX = Math.max(0.0, fx0 / pitch - cx); g.fracY = Math.max(0.0, fy0 / pitch - cy);
  g.zPlaneUm = Math.max(1e-3, ws.zPlaneNm / 1000.0);
  if (ws.slabHalfUm > 0.0) { g.zMinUm = ws.slabCentreUm - ws.slabHalfUm; g.zMaxUm = ws.slabCentreUm + ws.slabHalfUm; }
  else { g.zMinUm = COLUMN_MIN_UM; g.zMaxUm = COLUMN_MAX_UM; }
  g.axisX = axisX; g.axisY = axisY;
  return g;
}

// WidefieldDyeTiles::Fill + Planes: per world plane k, per population (0 bleaching, 1 persistent),
// Map(cell -> count) in grid-rect cells. Binned exactly as the C++ (tile rect, plane window from the
// tile's own histogram).
function dyePlanes(world, g) {
  const p = g.pitchUm, dz = g.zPlaneUm;
  const kLo = snapFloor(COLUMN_MIN_UM / dz), kHi = -snapFloor(-COLUMN_MAX_UM / dz), nH = kHi - kLo;
  const planes = new Map();
  let nB = 0, nP = 0;
  const tx0 = floorDiv(g.ix0, TILE), tx1 = floorDiv(g.ix0 + g.nx - 1, TILE);
  const ty0 = floorDiv(g.iy0, TILE), ty1 = floorDiv(g.iy0 + g.ny - 1, TILE);
  for (let ty = ty0; ty <= ty1; ++ty)
    for (let tx = tx0; tx <= tx1; ++tx) {
      const x0 = (tx * TILE) * p, x1 = ((tx + 1) * TILE) * p, y0 = (ty * TILE) * p, y1 = ((ty + 1) * TILE) * p;
      const zA0 = kLo * dz, zB0 = kHi * dz;
      const dyes = world.sitesInWindow(x0, y0, x1, y1, zA0, zB0);
      if (!dyes.length) continue;
      // 1 x 1 x nH histogram, as Density3d bins it.
      const hist = new Float32Array(nH), sz0 = nH / (zB0 - zA0);
      for (const d of dyes) hist[Math.min(nH - 1, Math.floor((d.z - zA0) * sz0))]++;
      let first = 0, last = nH - 1;
      while (first < nH && hist[first] === 0) ++first;
      while (last > first && hist[last] === 0) --last;
      first = Math.max(0, first - 1); last = Math.min(nH - 1, last + 1);
      const k0 = kLo + first, nz = last - first + 1, zA = k0 * dz, zB = (k0 + nz) * dz;
      const sx = TILE / (x1 - x0), sy = TILE / (y1 - y0), sz = nz > 1 ? nz / (zB - zA) : 0.0;
      for (const d of dyes) {
        if (!(d.z >= zA && d.z < zB)) continue;
        const ix = Math.min(TILE - 1, Math.floor((d.x - x0) * sx)), iy = Math.min(TILE - 1, Math.floor((d.y - y0) * sy));
        const iz = nz > 1 ? Math.min(nz - 1, Math.floor((d.z - zA) * sz)) : 0;
        const wx = tx * TILE + ix, wy = ty * TILE + iy;
        if (wx < g.ix0 || wx >= g.ix0 + g.nx || wy < g.iy0 || wy >= g.iy0 + g.ny) continue;
        const k = k0 + iz, pop = d.persistent ? 1 : 0;
        let pl = planes.get(k);
        if (!pl) { pl = [new Map(), new Map()]; planes.set(k, pl); }
        const cell = (wy - g.iy0) * g.nx + (wx - g.ix0);
        pl[pop].set(cell, (pl[pop].get(cell) || 0) + 1);
        if (pop) nP++; else nB++;
      }
    }
  return { planes, nBleaching: nB, nPersistent: nP };
}

// ApplyShiftRamp (Drift.cpp) on a full complex spectrum, inverse, crop the FOV cells: image(j + frac).
export function shiftedCrop(Sr, Si, nx, ny, fracX, fracY, fovX, fovY, cw, ch) {
  if (fracX !== 0 || fracY !== 0) {
    const px = new Float64Array(2 * nx), py = new Float64Array(2 * ny);
    for (let kx = 0; kx < nx; kx++) {
      const s = kx <= nx / 2 ? kx : kx - nx, a = 2.0 * Math.PI * s * fracX / nx;
      px[2 * kx] = Math.cos(a); px[2 * kx + 1] = kx === nx / 2 ? 0 : Math.sin(a);
    }
    for (let ky = 0; ky < ny; ky++) {
      const s = ky <= ny / 2 ? ky : ky - ny, a = 2.0 * Math.PI * s * fracY / ny;
      py[2 * ky] = Math.cos(a); py[2 * ky + 1] = ky === ny / 2 ? 0 : Math.sin(a);
    }
    for (let ky = 0; ky < ny; ky++) for (let kx = 0; kx < nx; kx++) {
      const fr = px[2 * kx] * py[2 * ky] - px[2 * kx + 1] * py[2 * ky + 1], fi = px[2 * kx] * py[2 * ky + 1] + px[2 * kx + 1] * py[2 * ky];
      const i = ky * nx + kx, r = Sr[i], m = Si[i];
      Sr[i] = r * fr - m * fi; Si[i] = r * fi + m * fr;
    }
  }
  fft2(Sr, Si, nx, ny, 1);
  const img = new Float32Array(cw * ch), scale = 1 / (nx * ny);
  for (let y = 0; y < ch; y++) for (let x = 0; x < cw; x++) img[y * cw + x] = Sr[(fovY + y) * nx + fovX + x] * scale;
  return img;
}

// ---- the scene: images of the persistent channel and the bleaching channel (anchor weights) ----
// opt.keep: also each channel's full spectrum before the sub-cell shift (spec: {re, im}; a drifting sample);
// opt.fixed: {R, nx, ny} of the scene (FocusSeries: the scene's FFT at other foci).
function sceneImages(ws, g, dyes, psf, wp, wbAnchor, opt = {}) {
  const dz = g.zPlaneUm;
  // Kernel radius (SetupFft).
  const cap = Math.max(1, Math.ceil(ws.kernelCapUm / g.pitchUm - 1e-9));
  let pLo = psf.maxPlane, pHi = psf.minPlane;
  const ks = [...dyes.planes.keys()].sort((a, b) => a - b);
  const clampP = t => Math.trunc(Math.min(psf.maxPlane, Math.max(psf.minPlane, Math.floor(t))));
  if (ks.length) {
    const a = clampP(psf.planeCoord((ks[0] + 0.5) * dz - ws.focusWorldUm));
    const b = clampP(psf.planeCoord((ks[ks.length - 1] + 0.5) * dz - ws.focusWorldUm));
    pLo = Math.min(a, b); pHi = Math.min(psf.maxPlane, Math.max(a, b) + 1);
  }
  const natural = pLo <= pHi ? psf.radius(pLo, pHi) : psf.radius(0, 0);
  const R = opt.fixed ? opt.fixed.R : Math.max(1, Math.min(cap, natural));
  const u = Math.max(1, ws.upscale), cw = ws.width * u, ch = ws.height * u;
  const need = (n, fov0, fovN) => fastSize(Math.max(n + 8, fov0 + fovN + R + 6, n + R + 6 - Math.min(fov0, n)), 8);
  const nx = opt.fixed ? opt.fixed.nx : need(g.nx, g.fovX, cw), ny = opt.fixed ? opt.fixed.ny : need(g.ny, g.fovY, ch);
  // Focus plan: dye plane k -> PSF planes p0 (w0) and p0 + 1 (w1).
  const kLo = snapFloor(g.zMinUm / dz), kHi = -snapFloor(-g.zMaxUm / dz);
  const deps = [];
  let clamped = 0;
  for (const k of ks) {
    if (k < kLo || k >= kHi) continue;
    let t = psf.planeCoord((k + 0.5) * dz - ws.focusWorldUm);
    if (t < psf.minPlane || t > psf.maxPlane) {
      for (const pop of dyes.planes.get(k)) for (const c of pop.values()) clamped += c;
      t = Math.min(psf.maxPlane, Math.max(psf.minPlane, t));
    }
    let p0 = Math.floor(t), frac = t - p0;
    if (p0 >= psf.maxPlane) { p0 = psf.maxPlane; frac = 0.0; }
    deps.push({ k, p0, w0: Math.fround(1.0 - frac), w1: Math.fround(frac) });
  }
  const kernelSpec = new Map();
  const kSpec = p => {
    let s = kernelSpec.get(p);
    if (s) return s;
    const k = psf.kernel(p, R), D = 2 * R + 1;
    const re = new Float64Array(nx * ny), im = new Float64Array(nx * ny);
    for (let dy = -R; dy <= R; ++dy) for (let dx = -R; dx <= R; ++dx)
      re[((dy + ny) % ny) * nx + (dx + nx) % nx] = k[(dy + R) * D + dx + R];
    fft2(re, im, nx, ny, -1);
    s = { re, im };
    kernelSpec.set(p, s);
    return s;
  };
  const channel = (pop, weight) => {
    if (!(pop === 1 ? dyes.nPersistent : dyes.nBleaching)) return null;
    // Spatial pre-sum per PSF plane: G_p = sum_k w(k, p) x dyes_k x weight (linear = the C++'s spectrum sum).
    const G = new Map();
    for (const d of deps) {
      const cells = dyes.planes.get(d.k)[pop];
      if (!cells.size) continue;
      for (const [p, w] of [[d.p0, d.w0], [d.p0 + 1, d.w1]]) {
        if (!(w > 0)) continue;
        let a = G.get(p);
        if (!a) { a = new Float64Array(nx * ny); G.set(p, a); }
        for (const [cell, cnt] of cells) {
          const v = Math.fround(cnt * weight[cell]);
          if (v) a[Math.floor(cell / g.nx) * nx + cell % g.nx] += w * v;
        }
      }
    }
    if (!G.size) return null;
    const Sr = new Float64Array(nx * ny), Si = new Float64Array(nx * ny);
    for (const [p, a] of G) {
      const im = new Float64Array(nx * ny);
      fft2(a, im, nx, ny, -1);
      const K = kSpec(p);
      for (let i = 0; i < Sr.length; i++) { Sr[i] += a[i] * K.re[i] - im[i] * K.im[i]; Si[i] += a[i] * K.im[i] + im[i] * K.re[i]; }
    }
    // Sub-cell shift (Nyquist bins: real factor), inverse, crop.
    const spec = opt.keep ? { re: Sr.slice(), im: Si.slice() } : null;
    const img = shiftedCrop(Sr, Si, nx, ny, g.fracX, g.fracY, g.fovX, g.fovY, cw, ch);
    return opt.keep ? { img, spec } : img;
  };
  const im = { cw, ch, u, R, nx, ny, clamped, fovX: g.fovX, fovY: g.fovY, fracX: g.fracX, fracY: g.fracY };
  const P = channel(1, wp), Bc = wbAnchor ? channel(0, wbAnchor) : null;
  if (!opt.keep) return { ...im, persistent: P, bleach: Bc };
  return { ...im, persistent: P && P.img, bleach: Bc && Bc.img, specP: P && P.spec, specB: Bc && Bc.spec };
}

// RenderShiftedImages: the frame (bleach coefficient a) interpolated between two foci' images (weight w of i1)
// with the sample moved by (dxCells, dyCells): spectra summed, one phase ramp, crop, max(0, .), bin into cam.
function renderShiftedImages(i0, i1, w, a, dxCells, dyCells, cam) {
  const { nx, ny } = i0, n = nx * ny, Sr = new Float64Array(n), Si = new Float64Array(n);
  let any = false;
  const add = (spec, c) => {
    if (!spec || c === 0) return;
    any = true;
    for (let i = 0; i < n; i++) { Sr[i] += c * spec.re[i]; Si[i] += c * spec.im[i]; }
  };
  const addImages = (im, wi) => { add(im.specP, wi); if (a) add(im.specB, a * wi); };
  addImages(i0, i1 ? 1.0 - w : 1.0);
  if (i1 && w !== 0) addImages(i1, w);
  if (!any) return;
  const img = shiftedCrop(Sr, Si, nx, ny, i0.fracX - dxCells, i0.fracY - dyCells, i0.fovX, i0.fovY, i0.cw, i0.ch);
  const { cw, u } = i0, W = cw / u, H = i0.ch / u;
  for (let Y = 0; Y < H; ++Y) for (let X = 0; X < W; ++X) {
    let acc = 0.0;
    for (let sy = 0; sy < u; ++sy) for (let sx = 0; sx < u; ++sx) acc += Math.max(0, img[(Y * u + sy) * cw + X * u + sx]);
    cam[Y * W + X] += acc;
  }
}

// WidefieldImages::Render: cam += bin(max(0, P + a B)).
function renderImages(im, a, cam) {
  const { cw, u } = im, W = cw / u, H = im.ch / u;
  if (!im.persistent && !(im.bleach && a)) return;
  const af = Math.fround(a);
  for (let Y = 0; Y < H; ++Y) for (let X = 0; X < W; ++X) {
    let acc = 0.0;
    for (let sy = 0; sy < u; ++sy) for (let sx = 0; sx < u; ++sx) {
      const i = (Y * u + sy) * cw + X * u + sx;
      let v = im.persistent ? im.persistent[i] : 0;
      if (im.bleach && a) v = Math.fround(v + Math.fround(af * im.bleach[i]));
      acc += Math.max(0, v);
    }
    cam[Y * W + X] += acc;
  }
}

// WidefieldMovie::Begin + Render.
export function renderWidefieldMovie(P, spec, S, onFrame, opts = {}) {
  const t0 = performance.now();
  const O = S.O, um = S.p.pixelSizeNm / 1000.0;
  const ws = {
    originXUm: S.q.originXUm, originYUm: S.q.originYUm, width: S.W, height: S.H, pixelUm: um,
    focusWorldUm: S.q.zCullCentreUm, slabCentreUm: S.q.zCullCentreUm, slabHalfUm: S.q.zHalfRangeUm,
    upscale: Math.trunc(Math.min(4, Math.max(1, O('wf-upscale')))), zPlaneNm: Math.min(500, Math.max(5, O('wf-plane-nm'))),
    marginUm: 2.0, kernelCapUm: Math.max(0.1, O('wf-kernel-um')),
    phot: { excitationPhotonsPerUm2PerSec: Math.max(0, O('wf-excitation-photons-per-um2-per-sec')),
      quantumYield: Math.min(1, Math.max(0, O('wf-quantum-yield'))), photonBudget: Math.max(0, O('wf-photon-budget')),
      extinctionCoeff: Math.max(0, O('wf-extinction-coeff')) },
    eta: collectionEfficiency(O('na'), O('immersion-index')), exposureSec: S.p.frameDurationSec,
  };
  // A drifting sample: its frames are its images shifted, so the square lights the FOV grown by the xy drift
  // (plus a pixel) and the grid holds the dyes the drift brings within the kernel's reach.
  const driftMarginUm = S.driftOn ? (Math.ceil(driftMaxXyNm(S.driftBounds) / S.p.pixelSizeNm) + 1.0) * um : 0.0;
  ws.marginUm += driftMarginUm;
  const sq = { w: S.W * um + 2.0 * driftMarginUm, h: S.H * um + 2.0 * driftMarginUm };
  const kernel = scopeKernel(spec, opts.onProgress && ((k, nz) => opts.onProgress('psf', (k + 1) / nz)));
  let psf;
  if (kernel) { ws.upscale = validUpscale(kernel.oversampling, ws.upscale); psf = kernelPsf(kernel, ws.upscale); }
  else psf = gaussianPsf(um / ws.upscale, O('wavelength-nm'), O('na'));
  const g = gridSpecFor(ws, sq);
  const world = scopeWorld(P, S);
  const dyes = dyePlanes(world, g);
  // Illumination (cell-centre samples of the square, relative to the axis) and frame dose per grid cell.
  const n2 = g.nx * g.ny, dD = new Float32Array(n2), wp = new Float32Array(n2);
  const dD1 = phot.emissionRatePerSec(ws.phot, 1.0) * ws.exposureSec;
  for (let iy = 0; iy < g.ny; iy++) {
    const y = g.y0Um - g.axisY + (iy + 0.5) * g.pitchUm;
    for (let ix = 0; ix < g.nx; ix++) {
      const x = g.x0Um - g.axisX + (ix + 0.5) * g.pitchUm;
      const ill = (x >= -sq.w / 2 && x < sq.w / 2 && y >= -sq.h / 2 && y < sq.h / 2) ? 1.0 : 0.0;
      const i = iy * g.nx + ix;
      dD[i] = dD1 * ill; wp[i] = ws.eta * dD[i];
    }
  }
  const B = ws.phot.photonBudget;
  const freshWeights = framesBefore => {
    const wb = new Float32Array(n2);
    for (let i = 0; i < n2; i++) wb[i] = bleachingPhotons(ws.eta, B, framesBefore * dD[i], dD[i]);
    return wb;
  };
  const framesBefore0 = Math.max(0.0, O('start-sec')) / S.expSec;
  const wb0 = freshWeights(framesBefore0);
  const images = sceneImages(ws, g, dyes, psf, wp, dyes.nBleaching ? wb0 : null, { keep: S.driftOn });
  // Drift: the images on the focus grid (FocusSeries: the scene's FFT, the slab moved with the focus).
  let grid = null, series = null;
  if (S.driftOn) {
    grid = driftFocusGrid(S.driftBounds);
    series = [];
    for (let k = 0; k < grid.n; k++) {
      const f = ws.focusWorldUm - driftGridDzNm(grid, k) / 1000.0, shift = f - ws.focusWorldUm;
      const wsk = { ...ws, focusWorldUm: f, slabCentreUm: ws.slabCentreUm + shift };
      const gk = ws.slabHalfUm > 0.0 ? { ...g, zMinUm: g.zMinUm + shift, zMaxUm: g.zMaxUm + shift } : g;
      series.push(sceneImages(wsk, gk, dyes, psf, wp, dyes.nBleaching ? wb0 : null,
        { keep: true, fixed: { R: images.R, nx: images.nx, ny: images.ny } }));
    }
  }
  // One-group basis coefficient: wb[iMax] / phi[iMax] (BleachCoefficients).
  let iMax = 0;
  for (let i = 1; i < n2; i++) if (Math.abs(wb0[i]) > Math.abs(wb0[iMax])) iMax = i;
  const maps = noiseMaps(S.seed, S.W, S.H, S.cam);
  const bg = S.p.backgroundPhotons;
  for (let f = 0; f < S.N; f++) {
    const cam = new Float32Array(S.W * S.H).fill(bg);
    // Only the anchor column's weight is used: freshWeights' value there (a float32).
    const wbMax = Math.fround(bleachingPhotons(ws.eta, B, (framesBefore0 + f) * dD[iMax], dD[iMax]));
    const a = wb0[iMax] !== 0 ? wbMax / wb0[iMax] : 0.0;
    if (S.driftOn) {
      const d = S.drift[f], [k, w] = driftGridWeights(grid, d.z), pitchNm = g.pitchUm * 1000.0;
      const i1 = w !== 0 && k + 1 < grid.n ? series[k + 1] : null;
      renderShiftedImages(series[k], i1, i1 ? w : 0.0, dyes.nBleaching ? a : 0, d.x / pitchNm, d.y / pitchNm, cam);
    } else renderImages(images, a, cam);
    if (onFrame(f, applyNoiseChain(cam, S.cam, maps, f), cam) === false) break;
    if (opts.onProgress) opts.onProgress('frames', (f + 1) / S.N);
  }
  return { width: S.W, height: S.H, frames: S.N, dyes: dyes.nBleaching + dyes.nPersistent, bleachingDyes: dyes.nBleaching,
    halfTimeSec: phot.halfTimeSec(ws.phot, 1.0), clamped: images.clamped, fft: [images.nx, images.ny], kernelRadius: images.R,
    totalSec: (performance.now() - t0) / 1000, psf: kernel ? 'GibsonLanniZernike' : 'Gaussian',
    driftNm: S.drift.flatMap(d => [d.x, d.y, d.z]),
    grid: { nx: g.nx, ny: g.ny, fovX: g.fovX, fovY: g.fovY, fracX: g.fracX, fracY: g.fracY },
    images: opts.keepImages ? images : undefined };
}
