// The mean-field (FFT) image of a population of dyes in JS: the mirror of the CPU path of
// adapter/inSiliScope/Simulation/WidefieldRender.cpp. Same discretisation: dyes binned into world-anchored z planes
// on 64-cell world tiles (WidefieldDyeTiles) of an upscaled grid (GridSpecFor), each plane deposited on its two
// nearest PSF planes with linear weights (PlanFocus), convolved by FFT (FastSize grid, 2^a 3^b 5^c), shifted to the
// camera's sub-cell position by a Fourier phase ramp, cropped, max(0, .) per cell, binned.
// Issue 16: no WideField movie of its own any more -- fluorescence.js uses meanFieldImage for every continuous
// population (WideField labels, PALM pre states, the dSTORM initial ON) above the mean-field density, and scales it
// per frame by the population's exact mean photons per dye.
// Not mirrored (speed only in the C++): spectra caches, focus bands (gated off for diffraction PSFs),
// dye-tile LRU, GPU. The C++ convolves in float32; this in float64 (agreement ~1e-6 relative).
const COLUMN_MIN_UM = -5.0, COLUMN_MAX_UM = 50.0;
const TILE = 64;
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
export function kernelPsf(c, upscale) {
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

// WidefieldDyeTiles::Fill + Planes: per world plane k, Map(cell -> count) in grid-rect cells of the dyes of the
// structures in structureMask (issue 16: one population per structure; the old bleaching/persistent split is a
// label mode now). Binned exactly as the C++ (tile rect, plane window from the tile's own histogram).
function dyePlanes(world, g, structureMask) {
  const p = g.pitchUm, dz = g.zPlaneUm;
  const kLo = snapFloor(COLUMN_MIN_UM / dz), kHi = -snapFloor(-COLUMN_MAX_UM / dz), nH = kHi - kLo;
  const planes = new Map();
  let nDyes = 0;
  const tx0 = floorDiv(g.ix0, TILE), tx1 = floorDiv(g.ix0 + g.nx - 1, TILE);
  const ty0 = floorDiv(g.iy0, TILE), ty1 = floorDiv(g.iy0 + g.ny - 1, TILE);
  for (let ty = ty0; ty <= ty1; ++ty)
    for (let tx = tx0; tx <= tx1; ++tx) {
      const x0 = (tx * TILE) * p, x1 = ((tx + 1) * TILE) * p, y0 = (ty * TILE) * p, y1 = ((ty + 1) * TILE) * p;
      const zA0 = kLo * dz, zB0 = kHi * dz;
      // Two passes over the tile's dyes (no copies): the 1 x 1 x nH histogram, as Density3d bins it, then the cells.
      const hist = new Float32Array(nH), sz0 = nH / (zB0 - zA0);
      let any = 0;
      world.forEachDye(x0, y0, x1, y1, zA0, zB0, (b, i) => {
        if (!((structureMask >> b.structure) & 1)) return;
        hist[Math.min(nH - 1, Math.floor((b.z[i] - zA0) * sz0))]++;
        any++;
      });
      if (!any) continue;
      let first = 0, last = nH - 1;
      while (first < nH && hist[first] === 0) ++first;
      while (last > first && hist[last] === 0) --last;
      first = Math.max(0, first - 1); last = Math.min(nH - 1, last + 1);
      const k0 = kLo + first, nz = last - first + 1, zA = k0 * dz, zB = (k0 + nz) * dz;
      const sx = TILE / (x1 - x0), sy = TILE / (y1 - y0), sz = nz > 1 ? nz / (zB - zA) : 0.0;
      world.forEachDye(x0, y0, x1, y1, zA0, zB0, (b, i) => {
        if (!((structureMask >> b.structure) & 1)) return;
        const x = b.x[i], y = b.y[i], z = b.z[i];
        if (!(z >= zA && z < zB)) return;
        const ix = Math.min(TILE - 1, Math.floor((x - x0) * sx)), iy = Math.min(TILE - 1, Math.floor((y - y0) * sy));
        const iz = nz > 1 ? Math.min(nz - 1, Math.floor((z - zA) * sz)) : 0;
        const wx = tx * TILE + ix, wy = ty * TILE + iy;
        if (wx < g.ix0 || wx >= g.ix0 + g.nx || wy < g.iy0 || wy >= g.iy0 + g.ny) return;
        const k = k0 + iz;
        let pl = planes.get(k);
        if (!pl) { pl = new Map(); planes.set(k, pl); }
        const cell = (wy - g.iy0) * g.nx + (wx - g.ix0);
        pl.set(cell, (pl.get(cell) || 0) + 1);
        nDyes++;
      });
    }
  return { planes, nDyes };
}

// ---- the scene: the image of one population of dyes with per-cell weights ----
// opt.prevR: the radius of the scene's previous update (SetupFft keeps it if the new one would only shrink);
// opt.fixedR: the radius as is (no SetupFft: the grid rect is unchanged); opt.keep: also the full spectrum before
// the sub-cell shift (spec: {re, im}; a drifting sample).
function sceneImages(ws, g, dyes, psf, weight, opt = {}) {
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
  const R0 = Math.max(1, Math.min(cap, natural));
  const R = opt.fixedR !== undefined ? opt.fixedR
    : opt.prevR !== undefined && opt.prevR >= R0 && opt.prevR <= cap ? opt.prevR : R0;
  const u = Math.max(1, ws.upscale), cw = ws.width * u, ch = ws.height * u;
  const need = (n, fov0, fovN) => fastSize(Math.max(n + 8, fov0 + fovN + R + 6, n + R + 6 - Math.min(fov0, n)), 8);
  const nx = need(g.nx, g.fovX, cw), ny = need(g.ny, g.fovY, ch);
  // Focus plan: dye plane k -> PSF planes p0 (w0) and p0 + 1 (w1).
  const kLo = snapFloor(g.zMinUm / dz), kHi = -snapFloor(-g.zMaxUm / dz);
  const deps = [];
  let clamped = 0;
  for (const k of ks) {
    if (k < kLo || k >= kHi) continue;
    let t = psf.planeCoord((k + 0.5) * dz - ws.focusWorldUm);
    if (t < psf.minPlane || t > psf.maxPlane) {
      for (const c of dyes.planes.get(k).values()) clamped += c;
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
  const channel = weight => {
    if (!dyes.nDyes) return null;
    // Spatial pre-sum per PSF plane: G_p = sum_k w(k, p) x dyes_k x weight (linear = the C++'s spectrum sum).
    const G = new Map();
    for (const d of deps) {
      const cells = dyes.planes.get(d.k);
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
    return { img: shiftedCrop(Sr, Si, nx, ny, g.fracX, g.fracY, g.fovX, g.fovY, cw, ch), spec };
  };
  const c = channel(weight);
  return { image: c && c.img, spec: c && c.spec, cw, ch, u, R, nx, ny, clamped, fovX: g.fovX, fovY: g.fovY,
    fracX: g.fracX, fracY: g.fracY };
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

// RenderShiftedImages: images i0 and i1 (weight w of i1, two foci) with the sample moved by (dxCells, dyCells):
// spectra summed, one phase ramp, crop, max(0, .), binned and added into cam.
export function renderShiftedImages(i0, i1, w, dxCells, dyCells, cam) {
  if (!i0.spec) return;
  const { nx, ny } = i0, n = nx * ny, Sr = new Float64Array(n), Si = new Float64Array(n);
  const add = (spec, c) => {
    if (!spec || spec.re.length !== n || c === 0) return;
    for (let i = 0; i < n; i++) { Sr[i] += c * spec.re[i]; Si[i] += c * spec.im[i]; }
  };
  add(i0.spec, Math.fround(i1 ? 1.0 - w : 1.0));
  if (i1 && w !== 0) add(i1.spec, Math.fround(w));
  const img = shiftedCrop(Sr, Si, nx, ny, i0.fracX - dxCells, i0.fracY - dyCells, i0.fovX, i0.fovY, i0.cw, i0.ch);
  const { cw, u } = i0, W = cw / u, H = i0.ch / u;
  for (let Y = 0; Y < H; ++Y) for (let X = 0; X < W; ++X) {
    let acc = 0.0;
    for (let sy = 0; sy < u; ++sy) for (let sx = 0; sx < u; ++sx) acc += Math.max(0, img[(Y * u + sy) * cw + X * u + sx]);
    cam[Y * W + X] += acc;
  }
}

// WidefieldImages::Render for one channel: bin(max(0, I)) per camera pixel (photons per unit weight).
function binnedImage(im, W, H) {
  const out = new Float32Array(W * H);
  if (!im.image) return out;
  const { cw, u } = im;
  for (let Y = 0; Y < H; ++Y) for (let X = 0; X < W; ++X) {
    let acc = 0.0;
    for (let sy = 0; sy < u; ++sy) for (let sx = 0; sx < u; ++sx) acc += Math.max(0, im.image[(Y * u + sy) * cw + X * u + sx]);
    out[Y * W + X] = acc;
  }
  return out;
}

// The mean-field image of a structure's dyes (issue 16, WidefieldMeanField): every fluorescent dye of the structures
// in structureMask with weight 1 on the world-anchored grid, convolved with the PSF at wavelengthNm (the kernel, or the
// in-focus Gaussian), binned to the camera: W x H photons per (photon per dye). A frame of a continuous population
// adds (mean detected photons per dye in that frame) x this image. Uniform illumination over the grid (the FOV and
// its margin, as the per-dye path's emitters).
// marginExtraUm: the drift margin (a drifting sample's frames are this image shifted; imagesAt gives them).
export function meanFieldImage(world, S, structureMask, kernel, wavelengthNm, marginExtraUm = 0.0) {
  const O = S.O, um = S.p.pixelSizeNm / 1000.0;
  const ws = {
    originXUm: S.q.originXUm, originYUm: S.q.originYUm, width: S.W, height: S.H, pixelUm: um,
    focusWorldUm: S.q.zCullCentreUm, slabCentreUm: S.q.zCullCentreUm, slabHalfUm: S.q.zHalfRangeUm,
    upscale: Math.trunc(Math.min(4, Math.max(1, O('wf-upscale')))), zPlaneNm: Math.min(500, Math.max(5, O('wf-plane-nm'))),
    marginUm: 2.0 + marginExtraUm, kernelCapUm: Math.max(0.1, O('wf-kernel-um')),
  };
  // The illuminated square spans the FOV and the margin (as the per-dye path's query rect), so the grid does too.
  const sq = { w: S.W * um + 2 * ws.marginUm, h: S.H * um + 2 * ws.marginUm };
  let psf;
  if (kernel) { ws.upscale = validUpscale(kernel.oversampling, ws.upscale); psf = kernelPsf(kernel, ws.upscale); }
  else psf = gaussianPsf(um / ws.upscale, wavelengthNm, O('na'));
  const g = gridSpecFor(ws, sq);
  const dyes = dyePlanes(world, g, structureMask);
  const ones = new Float32Array(g.nx * g.ny).fill(1);
  const images = sceneImages(ws, g, dyes, psf, ones, { keep: marginExtraUm > 0 });
  // The scene at other foci (a z-drifting sample, DriftMeanField): the slab follows the focus, the radius is kept
  // unless it grows (WidefieldScene::Update / SetupFft); images with spectra, cached per focus.
  let R = images.R;
  const cache = new Map();
  const imagesAt = focus => {
    let im = cache.get(focus);
    if (im) return im;
    const slab = ws.slabHalfUm > 0.0, wsk = { ...ws, focusWorldUm: focus, slabCentreUm: focus };
    const gk = gridSpecFor(wsk, sq);
    im = sceneImages(wsk, gk, dyes, psf, ones, slab ? { keep: true, prevR: R } : { keep: true, fixedR: R });
    R = im.R;
    cache.set(focus, im);
    return im;
  };
  return { imagesAt, pitchUm: g.pitchUm, image: binnedImage(images, S.W, S.H), dyes: dyes.nDyes, clamped: images.clamped, fft: [images.nx, images.ny],
    kernelRadius: images.R, grid: { nx: g.nx, ny: g.ny, fovX: g.fovX, fovY: g.fovY, fracX: g.fracX, fracY: g.fracY } };
}
