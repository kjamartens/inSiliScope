// The excitation geometry's checks (Node, seconds; web/prototype/scope/excitation.js): the BFP slider's physics and
// its effect on movies. Part of web/lab/check.mjs.
//   node web/lab/excitation_check.mjs
// Checks: epi = the in-plane profile; the field factor continuous at the critical angle; the decay depth against
// Axelrod 1981's example (120 nm); the plane means and column integrals against quadrature; HILO's sheet; a clipped
// spot is dark; the free imager's collected column (isotropic: against an independent quadrature in the focal plane;
// the renderers' map against a dense one; epi well below the chamber's height); in movies, TIRF keeps the coverslip's
// dyes and loses the cell's, and cuts DNA-PAINT's imager background.
import fs from 'fs';
import path from 'path';
import { fileURLToPath } from 'url';
import { makeExcitation } from '../prototype/scope/excitation.js';
import { renderScopeMovie } from '../prototype/scope/scope_movie.js';
import { loadPrototype } from '../../tests/parity/load_prototype.mjs';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
let fail = 0;
const bad = m => { console.log('FAIL ' + m); fail++; };
const near = (a, b, tol, what) => { if (!(Math.abs(a - b) <= tol * Math.max(1, Math.abs(b)))) bad(`${what}: ${a} vs ${b}`); };
const t0 = performance.now();
const base = { profile: 2, fwhmPct: 100, fovWidthUm: 12.8, na: 1.49, nImm: 1.518, nMed: 1.337, pivotUm: 0 };
const red = [{ nm: 640, share: 1 }];

// Epi: F = 1, no depth dependence, the footprint is the profile.
{
  const e = makeExcitation({ ...base, bfp: 0 });
  if (e.regime !== 'Epi') bad(`bfp 0: regime ${e.regime}`);
  near(e.F, 1, 1e-15, 'epi field factor');
  near(e.at(3, 1, 2.5, red), e.profileAt(3, 1), 1e-15, 'epi = the profile');
  near(e.at(0, 0, 0, red), 1, 1e-12, 'epi peak');
  near(e.profileAt(6.4, 0), 0.5, 2e-3, 'FlatTop half maximum at FWHM / 2 (peak-normalised)');
  if (!makeExcitation({ ...base, profile: 0, bfp: 0 }).uniform) bad('Flat epi is not uniform');
}
// Continuity at the critical angle (U_GAP either side) and the TIRF maximum about 4x.
{
  const lo = makeExcitation({ ...base, bfp: 0.999 }), hi = makeExcitation({ ...base, bfp: 1.001 });
  if (lo.regime !== 'HILO' || hi.regime !== 'TIRF') bad(`regimes ${lo.regime} / ${hi.regime}`);
  // The transmitted side approaches the common limit like sqrt(1 - u) (cos theta_s), the evanescent side linearly:
  // both move towards it as the gap closes, and the limit is the mean of 4 (s) and 4 / n^2 (p) over t(0)^2.
  const n = base.nMed / base.nImm, t0 = 2 * base.nImm / (base.nImm + base.nMed), lim = (4 + 4 / (n * n)) / 2 / (t0 * t0);
  const far = makeExcitation({ ...base, bfp: 0.99 }), farHi = makeExcitation({ ...base, bfp: 1.01 });
  if (!(far.F < lo.F && lo.F < lim && farHi.F < hi.F && Math.abs(hi.F - lim) < 0.02 * lim))
    bad(`field factor near the critical angle: ${far.F} ${lo.F} | limit ${lim} | ${hi.F} ${farHi.F}`);
}
// Axelrod 1981 (Fig. 2, Methods): theta 72.5 deg in n1 = 1.44, theta_c 67.5 deg, 514.5 nm: d = 120 nm.
{
  const n1 = 1.44, n2 = n1 * Math.sin(67.5 * Math.PI / 180), u = n1 * Math.sin(72.5 * Math.PI / 180) / n2;
  const e = makeExcitation({ ...base, nImm: n1, nMed: n2, bfp: u });
  near(e.depthNm(514.5), 120, 0.01, 'Axelrod 1981 decay depth');
}
// Plane means and column integrals against quadrature (TIRF, two lines; HILO).
{
  const two = [{ nm: 640, share: 0.7 }, { nm: 405, share: 0.3 }];
  const e = makeExcitation({ ...base, bfp: 1.05 });
  let q = 0;
  const n = 20000, z0 = 0.05, z1 = 0.075;
  for (let i = 0; i < n; i++) q += e.depth(z0 + (i + 0.5) * (z1 - z0) / n, two) / n;
  near(e.depthMean(z0, z1, two), q, 1e-9, 'TIRF plane mean');
  let c = 0;
  for (let i = 0; i < n; i++) c += e.at(1, 2, (i + 0.5) * 5 / n, two) * 5 / n;
  near(e.column(1, 2, 5, two), c, 1e-6, 'TIRF column integral');
  const h = makeExcitation({ ...base, bfp: 0.9, pivotUm: 2 });
  let ch = 0;
  for (let i = 0; i < 200000; i++) ch += h.at(1, 2, (i + 0.5) * 100 / 200000, red) * 100 / 200000;
  near(h.column(1, 2, 100, red), ch, 1e-3, 'HILO column integral');
  // The sheet: on the axis, lit around the pivot, dark a sheet thickness above it.
  const t = h.depthNm() / 1000;
  if (!(h.at(0, 0, 2, red) > 0.9 * h.F) || !(h.at(0, 0, 2 + t, red) < 0.01 * h.F)) bad(`HILO sheet: thickness ${t} um`);
  const ep = makeExcitation({ ...base, bfp: 0 });
  if (!(h.column(0, 0, 100, red) < 0.2 * ep.column(0, 0, 100, red))) bad('HILO column not well below epi');
}
// A spot beyond the pupil edge: no light.
{
  const e = makeExcitation({ ...base, na: 1.4, bfp: 1.1 });
  if (e.regime !== 'Clipped' || e.at(0, 0, 0, red) !== 0 || e.column(0, 0, 10, red) !== 0) bad('clipped spot is not dark');
}
// The collected column of the free imager (excitation.js imagerMap).
{
  const cb = { ...base, fovWidthUm: 25.6, na: 1.4, pivotUm: 0.5 }, im = [{ rate: 1, shares: red }];
  // Isotropic emission (T = 1), epi, on the axis: the focal-plane irradiance of a dye at defocus dz is
  // dz / (2 pi (dz^2 + rho^2)^(3/2)), so the column is the integral over z of
  // int_0^inf P(dz t) t / (1 + t^2)^(3/2) dt (t = rho / dz; P radial and 1 at the axis).
  const e = makeExcitation({ ...cb, bfp: 0 });
  let ref = 0;
  const nz = 4000, Hc = 100;
  for (let i = 0; i < nz; i++) {
    const dz = Math.abs((i + 0.5) * Hc / nz - cb.pivotUm), sMax = Math.asinh(200 / Math.max(dz, 1e-3)), ns = 3000;
    let inner = 0;
    for (let k = 0; k < ns; k++) {
      const sk = (k + 0.5) * sMax / ns, t = Math.sinh(sk);
      inner += e.profileAt(dz * t, 0) * t / Math.pow(1 + t * t, 1.5) * Math.cosh(sk) * sMax / ns;
    }
    ref += inner * Hc / nz;
  }
  const iso = e.imagerColumnAt(0, 0, Hc, im, { fresnel: false });
  near(iso, ref, 5e-3, 'isotropic collected column vs the focal-plane quadrature');
  const fr = e.imagerColumnAt(0, 0, Hc, im);
  console.log(`epi collected column on the axis (100 um chamber, 25.6 um FlatTop): isotropic ${iso.toFixed(2)} um, ` +
    `Fresnel-weighted ${fr.toFixed(2)} um (the old column: ${e.column(0, 0, Hc, red).toFixed(0)} um)`);
  if (!(fr > iso && fr < 0.2 * Hc)) bad('Fresnel-weighted collected column not between the isotropic one and a fifth of the chamber');
  // The renderers' map (Gauss-Legendre rings, a node grid) against a dense reference, per regime.
  const W = 64, um = 0.4, ax = 12.8;
  for (const [bfp, tol] of [[0, 5e-3], [0.95, 3e-2], [1.03, 5e-3]]) {
    const x = makeExcitation({ ...cb, bfp }), m = x.imagerMap(W, W, um, ax, ax, Hc, im);
    for (const [i, j] of [[32, 32], [0, 32], [10, 50]]) {
      const d = x.imagerColumnAt(i * um - ax, j * um - ax, Hc, im, { rings: 128, dirs: 64, ratio: 1.02, tirfSteps: 512 });
      near(m[j * W + i], d, tol, `${x.regime} collected column at pixel (${i}, ${j})`);
    }
  }
  // A Flat beam lights every plane alike: the map is the plain column.
  const f = makeExcitation({ ...cb, profile: 0, bfp: 0.9 }), fm = f.imagerMap(8, 8, 1, 4, 4, Hc, im);
  if (fm[3 * 8 + 5] !== f.column(1, -1, Hc, red)) bad('Flat beam: map != column');
}
const unitMs = performance.now() - t0;

// Movies: TIRF vs epi on the cells (microtubules in WideField mode: every dye shows), DNA-PAINT background.
const P = loadPrototype(fs.readFileSync(path.join(ROOT, 'web/prototype/index.html'), 'utf8'),
  fs.readFileSync(path.join(ROOT, 'web/prototype/microtubules.js'), 'utf8'));
const sum = spec => {
  let s = 0, info = null;
  info = renderScopeMovie(P, spec, (f, adu, photons) => { for (let i = 0; i < photons.length; i++) s += photons[i]; });
  return { s, info };
};
const t1 = performance.now();
const K = 'psf-kernel-half-width-nm=1500 size=32 frames=1 npc-dye=None';
const wf = `${K} mt-dye=ATTO655 mt-mode=WideField z=0.1`;
const epi = sum(`${wf} illum-bfp=0`), tirf = sum(`${wf} illum-bfp=1.03`), hilo = sum(`${wf} illum-bfp=0.95`);
console.log(`WideField microtubules, photons: epi ${epi.s.toExponential(3)}, HILO ${hilo.s.toExponential(3)}, TIRF ${tirf.s.toExponential(3)} (d ${tirf.info.excitation.depthNm.toFixed(0)} nm)`);
// Per unit of the interface's intensity (the field factor), TIRF keeps only the dyes near the coverslip.
if (!(tirf.s / tirf.info.excitation.fieldFactor < 0.5 * epi.s)) bad('TIRF does not dim the cell (microtubules above the evanescent layer)');
const pt = 'specimen=Nanorulers psf-kernel-half-width-nm=1500 size=128 frames=1 ruler-label-pct=100';
const pe = sum(`${pt} illum-bfp=0`), ptirf = sum(`${pt} illum-bfp=1.03`), pdef = sum(pt);
const bgE = pe.info.imagerBackgroundPerPxPerFrame, bgT = ptirf.info.imagerBackgroundPerPxPerFrame;
console.log(`Nanorulers DNA-PAINT: imager background per px per frame epi ${bgE.toFixed(1)}, TIRF ${bgT.toFixed(2)}; default ${pdef.info.excitation.regime} (bfp ${pdef.info.excitation.bfp})`);
if (!(bgT < 0.1 * bgE)) bad('TIRF does not cut the free-imager background tenfold');
if (pdef.info.excitation.regime !== 'TIRF') bad(`nanorulers' default illumination ${pdef.info.excitation.regime}`);
const clipped = sum(`${pt} na=1.4 illum-bfp=1.1 background-per-sec=0`);
console.log(`clipped (NA 1.40, bfp 1.1): photons ${clipped.s.toExponential(3)}`);
if (clipped.s !== 0) bad('a clipped spot lights the sample');
// The kinetics at the local light: a Flat beam in HILO has the same factor F everywhere, so its movie equals epi with
// every laser line x F (dSTORM's clock at F x time = its rates at F x the light; PALM's activation; WideField's
// bleaching; DNA-PAINT's photons and imager background).
{
  const F = makeExcitation({ ...base, profile: 0, na: 1.4, bfp: 0.6 }).F;
  const B = 'world-seed=1249 x=63 y=3 size=32 psf-model=0 npc-dye=None illum-profile=Flat';
  for (const [name, spec, lasers] of [
    ['dSTORM AF647 at 60 s', 'frames=8 mt-dye=AF647 light-preset=auto', { 640: 2.0 }],
    ['dSTORM AF647 from 0 (initial ON)', 'frames=6 mt-dye=AF647 light-preset=auto start-sec=0', { 640: 2.0 }],
    ['PALM mEos3.2 + 405 nm, pre state', 'frames=6 mt-dye=mEos3.2 light-preset=auto laser-405=0.05', { 561: 1, 405: 0.05 }],
    ['WideField mEGFP, mean field', 'frames=3 mt-dye=mEGFP light-preset=auto', { 488: 0.01 }],
    ['WideField mEGFP, per dye', 'frames=4 mt-dye=mEGFP light-preset=auto mt-label-pct=3 mean-field-density-per-um2=1e9 mean-field-max-emitters=1e9 start-sec=0 mt-dye.photon-budget=2000', { 488: 0.01 }],
    ['DNA-PAINT ATTO 655 + imager', 'frames=6 chamber-height-um=5', { 640: 1.0 }],
  ]) {
    const t = performance.now();
    // The preset's power of each line (data/dyes/light_path.json), x F.
    const scaled = Object.entries(lasers).map(([nm, kw]) => `laser-${nm}=${kw * F}`).join(' ');
    const a = [], b = [];
    renderScopeMovie(P, `${B} ${spec} illum-bfp=0.6 na=1.4`, (f, adu) => a.push(...adu));
    renderScopeMovie(P, `${B} ${spec} illum-bfp=0 na=1.4 ${scaled}`, (f, adu) => b.push(...adu));
    let same = 0;
    for (let i = 0; i < a.length; i++) if (a[i] === b[i]) same++;
    const frac = a.length ? same / a.length : 0;
    console.log(`uniform F = ${F.toFixed(3)} vs lasers x F, ${name}: ${(100 * frac).toFixed(2)}% identical ADU (${(performance.now() - t).toFixed(0)} ms)`);
    if (!(frac >= 0.995)) bad(`${name}: local kinetics at a uniform factor differ from the lasers x F`);
  }
}
console.log(`excitation check: units ${unitMs.toFixed(0)} ms, movies ${(performance.now() - t1).toFixed(0)} ms: ${fail ? fail + ' failure(s)' : 'PASS'}`);
process.exit(fail ? 1 : 0);
