// Fiducial markers (beads) on any sample: the field (where the beads are, how big) and the bead's image (the PSF of a
// solid sphere). The JS reference of core/src/fiducials.cpp (the field; core params fid*) and of the engine's
// Simulation/Fiducials.* (the bead kernel). The photons, spectra and bleaching are fluorescence.js's (fiducialPlan).
// Data: data/fiducials (DYE_DATA.fiducials; Balinovic et al. 2019, balinovic2019). BSD-3-Clause.
//
// Placement: complete spatial randomness (a Poisson field, no hard core: doublets and overlaps happen as in a real
// sample), drawn per 2 um tile for address-based determinism: the tile's count Poisson(density x area), candidate i at
// pcg4d(seed ^ FID_SALT, tx, ty, CAND0 + 2 i) (x, y, the two uniforms of its diameter's normal draw) and its height
// draw on channel CAND0 + 2 i + 1. Height modes (fidHeight):
//   0 Coverslip: the centre at r above the glass (also under cells: beads put down before the cells);
//   1 OnTop: at the sample's surface there (World.surfaceHeightAt: the highest cytoplasm over (x, y), 0 on bare glass
//     and for the nanorulers, whose rods are 13-20 nm high) + r;
//   2 Layer: uniform in [r, fidLayerUm - r] (an agarose layer); a candidate inside a cell (below its surface + r) is
//     dropped, not redrawn, so the density per volume of gel stays uniform.
// Diameter: normal about fidDiameterNm with fidDiameterCvPct % spread (estimate, the user's 10-15 %), at least 10 % of
// it. A bead is a sphere: its brightness scales with d^3 (fluorescence.js).
import { pcg4d, pcgA, unit } from './rng.js';
import { DYE_DATA } from './dye_library_data.js';
import { buildBlockSums, splatSetup, PSF_INTERP } from './psf.js';
import { statePhotophysics, spectrum } from './dye_library.js';
import { crossSectionUm2, photonFluxPerUm2, sampleAt, GRID_N, gridNm } from './spectra.js';

export const FID_TILE_UM = 2.0;
export const FID_MAX_PER_TILE = 4096;
export const FID_SALT = 0x46494455;   // 'FIDU'
export const FID_CH = { COUNT: 0, CAND0: 16 };
export const FID_HEIGHTS = ['Coverslip', 'OnTop', 'Layer'];
export const FIDUCIAL_TYPES = DYE_DATA.fiducials.types;
export const FIDUCIAL_TYPE_IDS = FIDUCIAL_TYPES.map(t => t.id);
const D = DYE_DATA.fiducials.defaults;

// The core params of the field (core/src/params.cpp kFields; the engine's fid-* options and p.<name>). Not part of the
// packing fingerprint: beads move no cell.
export const FIDUCIAL_DEFAULTS = {
  fidDensity: D.densityPerUm2,        // beads per um^2 of coverslip (0 = none)
  fidDiameterNm: D.diameterNm,        // mean diameter, nm
  fidDiameterCvPct: D.diameterCvPct,  // diameter spread, % (estimate)
  fidHeight: FID_HEIGHTS.indexOf(D.height),
  fidLayerUm: D.layerUm,              // Layer: the gel's thickness, um
};
export const FIDUCIAL_PARAM_NAMES = Object.keys(FIDUCIAL_DEFAULTS);

export function normalizeFiducialParams(p) {
  const q = { ...FIDUCIAL_DEFAULTS, ...p };
  const clamp = (v, lo, hi) => Math.min(hi, Math.max(lo, v));
  q.fidDensity = clamp(+q.fidDensity, 0, 100);
  q.fidDiameterNm = clamp(+q.fidDiameterNm, 10, 2000);
  q.fidDiameterCvPct = clamp(+q.fidDiameterCvPct, 0, 50);
  q.fidHeight = Math.trunc(clamp(+q.fidHeight, 0, FID_HEIGHTS.length - 1));
  q.fidLayerUm = clamp(+q.fidLayerUm, 0, 1000);
  return q;
}

function poissonInverse(m, u) {
  let p = Math.exp(-m), F = p, c = 0;
  while (u > F && c < FID_MAX_PER_TILE) { c++; p *= m / c; F += p; }
  return c;
}

export class FiducialField {
  // surfaceAt(x, y): the sample's surface height there, um (World.surfaceHeightAt); used by OnTop and Layer.
  constructor(seed, params, surfaceAt) {
    this.seed = seed >>> 0;
    this.P = normalizeFiducialParams(params);
    this.surfaceAt = surfaceAt;
    this.tiles = new Map();
  }
  get on() { return this.P.fidDensity > 0; }
  // The beads of tile (tx, ty) in candidate order: [{tx, ty, i, x, y, z (um, the centre), d (nm)}].
  tile(tx, ty) {
    const key = tx + ',' + ty;
    let out = this.tiles.get(key);
    if (out) return out;
    const P = this.P, s = (this.seed ^ FID_SALT) >>> 0, T = FID_TILE_UM;
    const lambda = P.fidDensity * T * T;
    const count = lambda > 0 ? poissonInverse(lambda, unit(pcgA(s, tx >>> 0, ty >>> 0, FID_CH.COUNT))) : 0;
    const cv = P.fidDiameterCvPct / 100.0;
    out = [];
    for (let i = 0; i < count; i++) {
      const w = pcg4d(s, tx >>> 0, ty >>> 0, (FID_CH.CAND0 + 2 * i) >>> 0);
      const x = (tx + unit(w[0])) * T, y = (ty + unit(w[1])) * T;
      const g = Math.sqrt(-2.0 * Math.log(unit(w[2]))) * Math.cos(2.0 * Math.PI * unit(w[3]));
      const d = P.fidDiameterNm * Math.max(0.1, 1.0 + cv * g), r = d / 2000.0;
      let z = r;
      if (P.fidHeight === 1) z = this.surfaceAt(x, y) + r;
      else if (P.fidHeight === 2) {
        const u = unit(pcgA(s, tx >>> 0, ty >>> 0, (FID_CH.CAND0 + 2 * i + 1) >>> 0));
        z = P.fidLayerUm > 2.0 * r ? r + u * (P.fidLayerUm - 2.0 * r) : r;
        if (z < this.surfaceAt(x, y) + r) continue;   // inside a cell
      }
      out.push({ tx, ty, i, x, y, z, d });
    }
    this.tiles.set(key, out);
    return out;
  }
  // Beads whose centre lies in [x0, x1) x [y0, y1), tile by tile (tx outer, ty inner) (isc_fiducials_in_window).
  inRect(x0, y0, x1, y1) {
    if (!this.on) return [];
    const T = FID_TILE_UM, out = [];
    for (let tx = Math.floor(x0 / T); tx <= Math.floor(x1 / T); tx++)
      for (let ty = Math.floor(y0 / T); ty <= Math.floor(y1 / T); ty++)
        for (const b of this.tile(tx, ty)) if (b.x >= x0 && b.x < x1 && b.y >= y0 && b.y < y1) out.push(b);
    return out;
  }
}

// ---- the bead's image ----
// A bead is one emitter with the PSF of a uniformly bright solid sphere, never its ~1e4-1e6 dyes: one splat per bead
// and component per frame. Its image has two parts, on the camera pixels around the bead's centre pixel:
//   the core, |dx|, |dy| <= rc = ceil(BEAD_CORE_UM / pixel) pixels: the bead kernel -- the PSF averaged over the sphere
//     (in z over its slabs, BEAD_SLAB_NM apart, each at the PSF linearly interpolated between planes and weighted by
//     the slab's disc area pi (r^2 - z^2); then laterally over the sphere's projection, column density
//     ~ sqrt(r^2 - rho^2)), faded linearly into the point PSF over the outer BEAD_FADE_UM of the core radius --, read
//     with the movie's interpolation (Fft: Cubic);
//   the halo beyond it: the point PSF of the nearest plane (the whole kernel, no halo cut: as the continuous dyes), read
//     at the nearest sub-sample (one tap: the halo varies over many pixels).
// A bead of <= 0.4 um changes the halo by less than the PSF planes' own 0.1 um step does. The bead kernel is built per
// (point kernel, plane, diameter on a BEAD_BIN_NM grid), a (2 h + 1)^2 sample square, h = (rc + 2) x oversampling.
export const BEAD_CORE_UM = 1.0, BEAD_FADE_UM = 0.4, BEAD_SLAB_NM = 10, BEAD_BIN_NM = 10, BEAD_LATERAL_SUB = 4;
export const beadBinNm = dNm => Math.max(BEAD_BIN_NM, BEAD_BIN_NM * Math.round(dNm / BEAD_BIN_NM));

// The projected sphere of radius rS (kernel samples) as normalized taps: [{dx, dy, w}], each tap's weight its
// BEAD_LATERAL_SUB^2 sub-samples of sqrt(r^2 - rho^2).
function sphereTaps(rS) {
  const R = Math.ceil(rS + 0.5), taps = [], n = BEAD_LATERAL_SUB;
  let sum = 0.0;
  for (let dy = -R; dy <= R; dy++)
    for (let dx = -R; dx <= R; dx++) {
      let w = 0.0;
      for (let sy = 0; sy < n; sy++)
        for (let sx = 0; sx < n; sx++) {
          const u = dx + (sx + 0.5) / n - 0.5, v = dy + (sy + 0.5) / n - 0.5, q = rS * rS - (u * u + v * v);
          if (q > 0.0) w += Math.sqrt(q);
        }
      if (w > 0.0) { taps.push({ dx, dy, w }); sum += w; }
    }
  if (!(sum > 0.0)) return [{ dx: 0, dy: 0, w: 1.0 }];
  for (const t of taps) t.w /= sum;
  return taps;
}

// The bead kernel's plane: a bead of diameter dNm at plane k of point kernel K, Float32Array (2h + 1)^2.
function beadPlane(K, pixelNm, k, dNm, h) {
  const n = K.sizeOversampled, c = K.halfWidthOversampled, os = Math.max(1, K.oversampling);
  const sampleNm = pixelNm / os, rNm = dNm / 2.0;
  const taps = sphereTaps(rNm / sampleNm), R = taps.reduce((m, t) => Math.max(m, Math.abs(t.dx), Math.abs(t.dy)), 0);
  // z average: per plane j its weight (the slabs' disc areas x their interpolation weights).
  const nS = Math.max(1, Math.ceil(dNm / BEAD_SLAB_NM)), wz = new Float64Array(K.nz);
  let wsum = 0.0;
  for (let j = 0; j < nS; j++) {
    const zr = -rNm + (j + 0.5) * dNm / nS, w = Math.max(0.0, rNm * rNm - zr * zr);
    wsum += w;
    if (K.nz < 2) { wz[0] += w; continue; }
    const u = Math.min(K.nz - 1, Math.max(0, k + zr / K.zStepNm)), i0 = Math.min(K.nz - 2, Math.floor(u)), f = u - i0;
    wz[i0] += w * (1.0 - f); wz[i0 + 1] += w * f;
  }
  const H = h + R, m = 2 * H + 1, avg = new Float64Array(m * m);
  for (let j = 0; j < K.nz; j++) {
    if (!(wz[j] > 0)) continue;
    const pl = K.planes[j], w = wz[j] / wsum;
    for (let y = -H; y <= H; y++) {
      const ky = c + y;
      if (ky < 0 || ky >= n) continue;
      for (let x = -H; x <= H; x++) {
        const kx = c + x;
        if (kx >= 0 && kx < n) avg[(y + H) * m + x + H] += w * pl[ky * n + kx];
      }
    }
  }
  const size = 2 * h + 1, out = new Float32Array(size * size), pk = K.planes[k];
  const fadeIn = (BEAD_CORE_UM - BEAD_FADE_UM) * 1000.0 / sampleNm, fadeLen = BEAD_FADE_UM * 1000.0 / sampleNm;
  for (let y = -h; y <= h; y++)
    for (let x = -h; x <= h; x++) {
      const ky = c + y, kx = c + x, p = ky >= 0 && ky < n && kx >= 0 && kx < n ? pk[ky * n + kx] : 0.0;
      const rho = Math.hypot(x, y), fade = rho <= fadeIn ? 1.0 : Math.max(0.0, 1.0 - (rho - fadeIn) / fadeLen);
      let b = p;
      if (fade > 0.0) {
        let s = 0.0;
        for (const t of taps) s += t.w * avg[(y + t.dy + H) * m + x + t.dx + H];
        b = p + fade * (s - p);
      }
      out[(y + h) * size + x + h] = b;
    }
  // The sphere's light that the z average spreads beyond the core: given back in the core, in proportion to the point
  // PSF there (the bead keeps the point's photons).
  let sp = 0.0, sb = 0.0;
  for (let y = -h; y <= h; y++)
    for (let x = -h; x <= h; x++) {
      const ky = c + y, kx = c + x;
      if (ky >= 0 && ky < n && kx >= 0 && kx < n) sp += pk[ky * n + kx];
      sb += out[(y + h) * size + x + h];
    }
  if (sp > 0.0) {
    const g = (sp - sb) / sp;
    for (let y = -h; y <= h; y++)
      for (let x = -h; x <= h; x++) {
        const ky = c + y, kx = c + x;
        if (ky >= 0 && ky < n && kx >= 0 && kx < n) out[(y + h) * size + x + h] += g * pk[ky * n + kx];
      }
  }
  return out;
}

// The bead kernels of a point kernel K (pixelNm: its camera pixel), built on demand per (plane, diameter bin) and
// memoized with the kernel's planes.
const beadMemo = new WeakMap();
export function beadKernel(K, pixelNm) {
  let byKey = beadMemo.get(K.planes);
  if (!byKey) { byKey = new Map(); beadMemo.set(K.planes, byKey); }
  const key = `${K.interpMode},${pixelNm}`;
  let B = byKey.get(key);
  if (B) return B;
  const os = Math.max(1, K.oversampling), camRad = Math.trunc(K.halfWidthOversampled / os);
  const rc = Math.min(camRad, Math.ceil(BEAD_CORE_UM * 1000.0 / pixelNm));
  B = { K, pixelNm, rc, h: Math.min(K.halfWidthOversampled, (rc + 2) * os),
    interp: K.interpMode === PSF_INTERP.Fft ? PSF_INTERP.Cubic : K.interpMode, cores: new Map() };
  byKey.set(key, B);
  return B;
}
function beadCore(B, k, dBin) {
  const key = k * 100000 + dBin;
  let C = B.cores.get(key);
  if (C) return C;
  const K = B.K, os = Math.max(1, K.oversampling), size = 2 * B.h + 1;
  const plane = beadPlane(K, B.pixelNm, k, dBin, B.h);
  C = { oversampling: os, halfWidthOversampled: B.h, sizeOversampled: size, blockSums: buildBlockSums(plane, size, os),
    blockSumWidth: size + os - 1 };
  B.cores.set(key, C);
  return C;
}
// img += photons x the block sums of cache at setup st, over the camera pixels (x0 + dx, y0 + dy), dx in [dxLo, dxHi],
// dy in [dyLo, dyHi] (relative to st's centre pixel), clipped to the image and the block sums (SplatRows' arithmetic).
function splatRect(img, W, H, cache, st, B, photons, dxLo, dxHi, dyLo, dyHi) {
  const os = Math.max(1, cache.oversampling), off = os - 1, bw = cache.blockSumWidth, NT = st.nTaps, wx = st.wx, wy = st.wy;
  const yLo = Math.max(dyLo, -st.y0), yHi = Math.min(dyHi, H - 1 - st.y0);
  const xLo = Math.max(dxLo, -st.x0), xHi = Math.min(dxHi, W - 1 - st.x0);
  for (let dy = yLo; dy <= yHi; ++dy) {
    const r0 = st.by + dy * os + off, jLo = Math.max(0, -r0), jHi = Math.min(NT, bw - r0), rowOut = (st.y0 + dy) * W;
    for (let dx = xLo; dx <= xHi; ++dx) {
      const c0 = st.bx + dx * os + off, iLo = Math.max(0, -c0), iHi = Math.min(NT, bw - c0);
      let sum = 0.0;
      for (let j = jLo; j < jHi; ++j) {
        const brow = (r0 + j) * bw + c0;
        let row = 0.0;
        for (let i = iLo; i < iHi; ++i) row += wx[i] * B[brow + i];
        sum += wy[j] * row;
      }
      img[rowOut + st.x0 + dx] += Math.fround(photons * sum);
    }
  }
}
// Splat one bead of diameter dNm with `photons` (any sign) at (xPx, yPx) (camera pixels) and plane k into img: the core square,
// then the halo above, below, left and right of it.
export function splatBead(img, W, H, B, k, dNm, xPx, yPx, photons) {
  const K = B.K;
  if (photons === 0.0 || !K.valid || k < 0 || k >= K.nz) return;   // photons < 0: a Taylor image's odd order
  const C = beadCore(B, k, beadBinNm(dNm)), rc = B.rc, camRad = Math.trunc(K.halfWidthOversampled / Math.max(1, K.oversampling));
  splatRect(img, W, H, C, splatSetup(C, xPx, yPx, B.interp), C.blockSums, photons, -rc, rc, -rc, rc);
  const st = splatSetup(K, xPx, yPx, PSF_INTERP.Nearest), P = K.blockSums[k];
  splatRect(img, W, H, K, st, P, photons, -camRad, camRad, -camRad, -rc - 1);
  splatRect(img, W, H, K, st, P, photons, -camRad, camRad, rc + 1, camRad);
  splatRect(img, W, H, K, st, P, photons, -camRad, -rc - 1, -rc, rc);
  splatRect(img, W, H, K, st, P, photons, rc + 1, camRad, -rc, rc);
}

// Gold's elastic scattering cross section at a wavelength and diameter, um^2 (the Mie table, bilinear, clamped to it).
export function goldScatteringUm2(lambdaNm, dNm) {
  const M = DYE_DATA.fiducials.goldMie, L = M.lambdaNm, Dm = M.diameterNm;
  const at = (g, v) => {
    const n = Math.round((g.maxNm - g.minNm) / g.stepNm), x = Math.min(n, Math.max(0, (v - g.minNm) / g.stepNm));
    const i = Math.min(n - 1, Math.floor(x));
    return [i, x - i];
  };
  const [i, fi] = at(Dm, dNm), [j, fj] = at(L, lambdaNm), S = M.scatteringUm2;
  const v = (S[i][j] * (1 - fj) + S[i][j + 1] * fj) * (1 - fi) + (S[i + 1][j] * (1 - fj) + S[i + 1][j + 1] * fj) * fi;
  // Below the table's smallest sphere: the dipole limit, sigma ~ d^6.
  return dNm < Dm.minNm ? v * (dNm / Dm.minNm) ** 6 : v;
}

// ---- the light ----
// Per component of bead type t (FIDUCIAL_TYPES index) through light path lp, at excitation 1 and per equivalent dye
// (DYE_DATA.fiducials.equivalentDye: eps, QY; a bead of diameter d holds emitters x (d / refDiameterNm)^3 of them):
//   detectedPerSec, lambdaNm, lineShares (statePhotophysics);
//   bleachPerSec: the bleach hazard per second (a bead's signal is N e^-H: ~1e4-1e6 dyes bleach as their mean), from
//     the half-time halfSec at refKWcm2 at the component's own excitation peak: hazard ~ the emission rate, so
//     H' = (kEm / kEm at the reference) x ln 2 / halfSec; 0 for a component that does not bleach (gold, nanodiamond)
//     or halfSec <= 0.
// Gold also scatters each laser line elastically (scatter: 'gold'): a component per line with perUm2 = the detected
// photons per second per um^2 of scattering cross section (the line's photon flux at the sample x the collection
// (isotropic, an estimate) x the emission path's transmission at the line, floored at 10^-blockingOD: real filters
// block a laser by OD ~6, ideal ones fully) x the QE there.
// o: { dyes (> 0: every component's equivalent dye count at the reference diameter), halfSec, refKWcm2, blockingOD }.
export function fiducialPhotophysics(t, lp, o) {
  const T = FIDUCIAL_TYPES[t], eq = DYE_DATA.fiducials.equivalentDye, out = [];
  for (const c of T.components) {
    const em = spectrum(c.em);
    let emMaxNm = 0, best = -1;
    for (let i = 0; i < GRID_N; i++) if (em[i] > best) { best = em[i]; emMaxNm = gridNm(i); }
    const st = statePhotophysics({ isDark: false, ex: c.ex, em: c.em, extCoeff: eq.extCoeff, qy: eq.qy, emMaxNm }, lp);
    const kEmRef = eq.qy * crossSectionUm2(eq.extCoeff, spectrum(c.ex), c.exPeakNm) * photonFluxPerUm2(o.refKWcm2, c.exPeakNm);
    const bleachPerSec = c.bleaches && o.halfSec > 0 && kEmRef > 0 ? st.emissionPerSec / kEmRef * Math.LN2 / o.halfSec : 0.0;
    out.push({ id: c.id, name: c.name, scatter: false, lambdaNm: st.lambdaNm, detectedPerSec: st.detectedPerSec,
      detectedFraction: st.detectedFraction, lineShares: st.lineShares, bleachPerSec,
      emitters: o.dyes > 0 ? o.dyes : c.emitters, refDiameterNm: T.refDiameterNm });
  }
  if (T.scatter === 'gold') {
    const floor = Math.pow(10, -Math.max(0, o.blockingOD));
    for (const l of lp.lasers) {
      const flux = photonFluxPerUm2(l.kWPerCm2 * (1 - sampleAt(lp.dichroic.T, l.nm)), l.nm);
      const tr = Math.max(sampleAt(lp.dichroic.T, l.nm) * sampleAt(lp.emissionFilter.T, l.nm), floor) * sampleAt(lp.qe, l.nm);
      if (!(flux > 0)) continue;
      out.push({ id: `scatter${l.nm}`, name: `scattered ${l.nm} nm`, scatter: true, lambdaNm: l.nm, perUm2: flux * lp.eta * tr,
        lineShares: [{ nm: l.nm, share: 1 }], bleachPerSec: 0.0 });
    }
  }
  return out;
}
// A component's detected photons per second from one bead of diameter dNm at excitation 1 (before bleaching).
export const beadRatePerSec = (c, dNm) => c.scatter ? c.perUm2 * goldScatteringUm2(c.lambdaNm, dNm)
  : c.emitters * (dNm / c.refDiameterNm) ** 3 * c.detectedPerSec;
