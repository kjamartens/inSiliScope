// The binned blink regime (approximate SMLM), the mirror of adapter/inSiliScope/Simulation/BinnedBlinks.cpp: a frame's
// blink emitters (frameEmitters: the splat's photons, position and kernel plane) deposited on a grid of `upscale` cells
// per pixel, one density per PSF z plane, each FFT-convolved with that plane's kernel resampled to the grid
// (kernelPsf), summed in the spectrum, one inverse, cropped to the FOV, max(0, .) per cell, binned. Only the xy position
// is snapped (to the nearest cell). The C++ convolves in float32 (real FFT, sparse rows); this in float64.
import { fft2, kernelPsf, validUpscale } from './widefield.js';

// 2^a 3^b sizes, even (RealFft2d::FastSize(n, 2, no5)).
function fastSizeNo5(n) {
  for (let m = Math.max(n, 1); ; ++m) {
    if (m % 2) continue;
    let r = m;
    for (const p of [2, 3]) while (r % p === 0) r /= p;
    if (r === 1) return m;
  }
}

// kernel: the blink kernel (its halo spans bound the radius) or null (the in-focus Gaussian of sigmaPx).
export function binnedBlinkRenderer(kernel, sigmaPx, W, H, upscale) {
  let u, R, nz, cellKernel;
  if (kernel) {
    u = validUpscale(kernel.oversampling, upscale);
    const psf = kernelPsf(kernel, u);
    R = psf.radius(0, psf.maxPlane);
    const h = kernel.halo;
    if (h) {
      const n = 2 * h.camRad + 1;
      let r2 = -1;
      for (let z = 0; z < h.nz; ++z) for (let dy = -h.camRad; dy <= h.camRad; ++dy) {
        const l = h.lo[z * n + dy + h.camRad], hh = h.hi[z * n + dy + h.camRad];
        if (l <= hh) r2 = Math.max(r2, dy * dy + Math.max(l * l, hh * hh));
      }
      if (r2 >= 0) R = Math.min(R, Math.ceil((Math.sqrt(r2) + 1.0) * u));
    }
    nz = psf.maxPlane + 1;
    cellKernel = p => psf.kernel(p, R);
  } else {
    if (!(sigmaPx > 0)) return null;
    u = Math.max(1, Math.min(upscale, 8));
    R = Math.max(1, Math.ceil(3.0 * sigmaPx * u));
    nz = 1;
    const D = 2 * R + 1, sc = sigmaPx * u, twoS2 = 2.0 * sc * sc, k = new Float64Array(D * D), out = new Float32Array(D * D);
    let sum = 0.0;
    for (let y = -R; y <= R; ++y) for (let x = -R; x <= R; ++x) { const v = Math.exp(-(x * x + y * y) / twoS2); k[(y + R) * D + x + R] = v; sum += v; }
    for (let i = 0; i < k.length; i++) out[i] = k[i] / sum;
    cellKernel = () => out;
  }
  R = Math.max(1, R);
  const gw = W * u + 2 * R, gh = H * u + 2 * R, nx = fastSizeNo5(gw), ny = fastSizeNo5(gh);
  const spectra = new Map();
  const spectrum = p => {
    let s = spectra.get(p);
    if (s) return s;
    const k = cellKernel(p), D = 2 * R + 1, re = new Float64Array(nx * ny), im = new Float64Array(nx * ny);
    for (let dy = -R; dy <= R; ++dy) for (let dx = -R; dx <= R; ++dx)
      re[((dy + ny) % ny) * nx + (dx + nx) % nx] += k[(dy + R) * D + dx + R];
    fft2(re, im, nx, ny, -1);
    s = { re, im };
    spectra.set(p, s);
    return s;
  };
  return {
    u, R, nx, ny,
    // img (W x H) += the emitters' photon image.
    render(emitters, img) {
      const byPlane = new Map();
      for (const e of emitters) {
        if (!(e.photons > 0)) continue;
        const p = Math.max(0, Math.min(nz - 1, e.zIndex));
        if (!byPlane.has(p)) byPlane.set(p, []);
        byPlane.get(p).push(e);
      }
      const Sr = new Float64Array(nx * ny), Si = new Float64Array(nx * ny);
      let any = false;
      for (const p of [...byPlane.keys()].sort((a, b) => a - b)) {
        const re = new Float64Array(nx * ny), im = new Float64Array(nx * ny);
        let hit = false;
        for (const e of byPlane.get(p)) {
          const cx = Math.floor((e.xPx + 0.5) * u) + R, cy = Math.floor((e.yPx + 0.5) * u) + R;
          if (cx < 0 || cy < 0 || cx >= gw || cy >= gh) continue;
          re[cy * nx + cx] += Math.fround(e.photons);
          hit = true;
        }
        if (!hit) continue;
        any = true;
        fft2(re, im, nx, ny, -1);
        const K = spectrum(p);
        for (let i = 0; i < Sr.length; i++) { Sr[i] += re[i] * K.re[i] - im[i] * K.im[i]; Si[i] += re[i] * K.im[i] + im[i] * K.re[i]; }
      }
      if (!any) return;
      fft2(Sr, Si, nx, ny, 1);
      const cw = W * u, scale = 1 / (nx * ny);
      for (let Y = 0; Y < H; ++Y) for (let X = 0; X < W; ++X) {
        let acc = 0.0;
        for (let sy = 0; sy < u; ++sy) for (let sx = 0; sx < u; ++sx)
          acc += Math.max(0, Math.fround(Sr[(R + Y * u + sy) * nx + R + X * u + sx] * scale));
        img[Y * W + X] += acc;
      }
    },
  };
}
