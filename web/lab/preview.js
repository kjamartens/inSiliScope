// Quick imaging PREVIEW for the lab: dye sites -> Gaussian PSF with a defocus-broadened width -> shot +
// read noise. Deliberately simple and fast; NOT the C++ renderer (Gibson-Lanni+Zernike PSF, dye schedules,
// sCMOS/EMCCD chain live in adapter/inSiliScope/Simulation and stay the truth for imaging).

export const PREVIEW_DEFAULTS = {
  mode: 'wf',          // 'wf' widefield | 'sr' one SR camera frame | 'recon' localisation map
  fovUm: 12.8, pxUm: 0.1, focusUm: 0.5, labelPct: 10,
  lambdaNm: 670, na: 1.45, immersion: 1.518,
  wfPhotonsPerDye: 0.45, srOnPct: 0.05, srPhotons: 1000, bgPhotons: 20, readNoise: 1.2,
  reconPxUm: 0.02, locPrecNm: 10, reconRangeUm: 0.6, seed: 1,
};

function mulberry32(a) {
  return () => {
    a |= 0; a = (a + 0x6D2B79F5) | 0;
    let t = Math.imul(a ^ (a >>> 15), 1 | a);
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}
function gauss(rng) { return Math.sqrt(-2 * Math.log(1 - rng())) * Math.cos(2 * Math.PI * rng()); }
function poisson(rng, m) {
  if (m <= 0) return 0;
  if (m > 30) return Math.max(0, Math.round(m + Math.sqrt(m) * gauss(rng)));
  const L = Math.exp(-m);
  let k = 0, p = 1;
  do { k++; p *= rng(); } while (p > L);
  return k - 1;
}

export function psfModel(o) {
  const s0 = 0.21 * o.lambdaNm / o.na / 1000;                       // in-focus sigma, um
  const zR = (o.lambdaNm / 1000) * o.immersion / (o.na * o.na);      // ~depth of field, um
  return dz => s0 * Math.sqrt(1 + (dz / zR) * (dz / zR));
}

// Separable Gaussian blur of a W x H plane (in place via tmp).
function blur(img, W, H, sigmaPx, tmp) {
  const r = Math.min(64, Math.ceil(3 * sigmaPx));
  const k = new Float32Array(2 * r + 1);
  let s = 0;
  for (let i = -r; i <= r; i++) { k[i + r] = Math.exp(-0.5 * (i / sigmaPx) ** 2); s += k[i + r]; }
  for (let i = 0; i < k.length; i++) k[i] /= s;
  for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) {
    let a = 0;
    for (let i = -r; i <= r; i++) { const xx = x + i; if (xx >= 0 && xx < W) a += img[y * W + xx] * k[i + r]; }
    tmp[y * W + x] = a;
  }
  for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) {
    let a = 0;
    for (let i = -r; i <= r; i++) { const yy = y + i; if (yy >= 0 && yy < H) a += tmp[yy * W + x] * k[i + r]; }
    img[y * W + x] = a;
  }
}

// Photon image (no noise) of emitters (x,y,z,weight) through the defocus PSF: bins emitters into 50 nm
// z slabs, one separable blur per slab.
function photonImage(em, rect, o) {
  const [x0, y0] = rect, px = o.pxUm;
  const W = Math.round(o.fovUm / px), H = W;
  const sig = psfModel(o);
  const slab = 0.05, zMax = 4;
  const slabs = new Map();
  for (let i = 0; i < em.n; i++) {
    const dz = em.z[i] - o.focusUm;
    if (Math.abs(dz) > zMax) continue;
    const ix = Math.floor((em.x[i] - x0) / px), iy = Math.floor((em.y[i] - y0) / px);
    if (ix < 0 || iy < 0 || ix >= W || iy >= H) continue;
    const b = Math.round(dz / slab);
    let pl = slabs.get(b);
    if (!pl) { pl = new Float32Array(W * H); slabs.set(b, pl); }
    pl[iy * W + ix] += em.w[i];
  }
  const out = new Float32Array(W * H), tmp = new Float32Array(W * H);
  for (const [b, pl] of slabs) {
    blur(pl, W, H, Math.max(0.3, sig(b * slab) / px), tmp);
    for (let i = 0; i < out.length; i++) out[i] += pl[i];
  }
  return { img: out, W, H };
}

function addNoise(img, o, rng) {
  for (let i = 0; i < img.length; i++) img[i] = poisson(rng, img[i] + o.bgPhotons) + o.readNoise * gauss(rng);
  return img;
}

// dyes: {x, y, z: Float32Array} (world um) already thinned to labelPct. rect: [x0, y0, x1, y1].
export function render(dyes, rect, opt) {
  const o = { ...PREVIEW_DEFAULTS, ...opt };
  const rng = mulberry32(o.seed * 2654435761);
  const n = dyes.x.length;
  if (o.mode === 'recon') {
    const px = o.reconPxUm, W = Math.round(o.fovUm / px), H = W;
    const img = new Float32Array(W * H);
    const s = o.locPrecNm / 1000;
    let nloc = 0;
    for (let i = 0; i < n; i++) {
      const dz = dyes.z[i] - o.focusUm;
      if (Math.abs(dz) > o.reconRangeUm) continue;
      const sp = s * (1 + Math.abs(dz) / o.reconRangeUm);          // worse precision away from focus
      const x = dyes.x[i] + sp * gauss(rng), y = dyes.y[i] + sp * gauss(rng);
      const ix = Math.floor((x - rect[0]) / px), iy = Math.floor((y - rect[1]) / px);
      if (ix < 0 || iy < 0 || ix >= W || iy >= H) continue;
      img[iy * W + ix]++; nloc++;
    }
    blur(img, W, H, 0.8, new Float32Array(W * H));
    return { img, W, H, pxUm: px, info: `${nloc} localisations (|z - focus| < ${o.reconRangeUm} um)` };
  }
  const em = { n: 0, x: [], y: [], z: [], w: [] };
  if (o.mode === 'sr') {
    const p = o.srOnPct / 100;
    for (let i = 0; i < n; i++) if (rng() < p) {
      em.x.push(dyes.x[i]); em.y.push(dyes.y[i]); em.z.push(dyes.z[i]); em.w.push(o.srPhotons * Math.exp(0.47 * gauss(rng) - 0.11));
    }
  } else {
    for (let i = 0; i < n; i++) { em.x.push(dyes.x[i]); em.y.push(dyes.y[i]); em.z.push(dyes.z[i]); em.w.push(o.wfPhotonsPerDye); }
  }
  em.n = em.x.length;
  const { img, W, H } = photonImage(em, rect, o);
  addNoise(img, o, rng);
  return { img, W, H, pxUm: o.pxUm, info: o.mode === 'sr' ? `${em.n} emitters ON` : `${n} dyes` };
}
