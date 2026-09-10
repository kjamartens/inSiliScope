// Standalone dump tool: computes one in-focus PSF plane using webSMLM's OWN
// Zernike-pupil math (computePsfPupilForZPlane/computePsfIntensityPlane,
// psfZernikeValue/psfIndexToNM/psfZernikeRadial/psfBinomial), copied
// verbatim from C:\GitHub\websmlm\webSMLM.html (not imported -- that file
// is not a module, and this project deliberately doesn't add a build step
// to it -- see PARITY.md), so compare.py can diff it against
// DumpJavaPsf.java's dump of the equivalent demoCam_SMLM_MM computation.
// See README.md in this directory.
//
// If webSMLM.html's PSF math changes, this file drifts out of sync --
// that's expected of a copy, not a dependency; re-sync manually when
// re-running this check after a webSMLM PSF change (see PARITY.md's
// staleness warning).
//
// Run: node dump_websmlm.mjs <outfile>

const PSF_N_RHO = 20, PSF_N_PHI = 40;

function psfIndexToNM(j) {
  for (let n = 0; ; n++) for (let l = 0; l <= n; l++) if (n * (n + 1) / 2 + l === j) return [n, -n + 2 * l];
}
function psfBinomial(a, b) {
  if (b < 0 || b > a) return 0;
  let r = 1; for (let i = 0; i < b; i++) r = r * (a - i) / (i + 1);
  return r;
}
function psfZernikeRadial(n, m, rho) {
  let r = 0;
  for (let k = 0; k <= (n - m) / 2; k++) {
    const coeff = (k % 2 === 0 ? 1 : -1) * psfBinomial(n - k, k) * psfBinomial(n - 2 * k, (n - m) / 2 - k);
    r += coeff * Math.pow(rho, n - 2 * k);
  }
  return r;
}
function psfZernikeValue(j, rho, phi) {
  const [n, l] = psfIndexToNM(j), m = Math.abs(l);
  const radial = rho <= 1 ? psfZernikeRadial(n, m, rho) : 0;
  return radial * (l >= 0 ? Math.cos(m * phi) : Math.sin(m * phi));
}

function computePsfPupilForZPlane(z, nz, params) {
  const { NA, lambda, ns, ni, ti0, particleAxialPosition, zStep, zernikeCoeffs } = params;
  const focalShift = particleAxialPosition * (ni / ns);
  const ti = (ti0 - focalShift) + zStep * (z - (nz - 1) / 2);
  const k0 = 2 * Math.PI / lambda;
  const bMax = Math.min(1, ns / NA);
  const dRho = bMax / PSF_N_RHO, dPhi = 2 * Math.PI / PSF_N_PHI;
  const cosPhiAt = new Float64Array(PSF_N_PHI), sinPhiAt = new Float64Array(PSF_N_PHI);
  for (let ip = 0; ip < PSF_N_PHI; ip++) { const phi = (ip + 0.5) * dPhi; cosPhiAt[ip] = Math.cos(phi); sinPhiAt[ip] = Math.sin(phi); }
  const krAt = new Float64Array(PSF_N_RHO);
  const pupilRe = new Float64Array(PSF_N_RHO * PSF_N_PHI), pupilIm = new Float64Array(PSF_N_RHO * PSF_N_PHI);
  for (let ir = 0; ir < PSF_N_RHO; ir++) {
    const rho = (ir + 0.5) * dRho;
    krAt[ir] = k0 * NA * rho;
    const s1 = NA * rho / ns, s3 = NA * rho / ni;
    const opd1 = ns * particleAxialPosition * Math.sqrt(Math.max(0, 1 - s1 * s1));
    const opd3 = ni * (ti - ti0) * Math.sqrt(Math.max(0, 1 - s3 * s3));
    const gibsonLanniPhase = k0 * (opd1 + opd3);
    const rowBase = ir * PSF_N_PHI;
    for (let ip = 0; ip < PSF_N_PHI; ip++) {
      const phi = (ip + 0.5) * dPhi;
      let zernikePhase = 0;
      for (let j = 0; j < zernikeCoeffs.length; j++) { const c = zernikeCoeffs[j]; if (c !== 0) zernikePhase += c * psfZernikeValue(j, rho / bMax, phi); }
      zernikePhase *= 2 * Math.PI;
      const phase = gibsonLanniPhase + zernikePhase;
      pupilRe[rowBase + ip] = rho * Math.cos(phase);
      pupilIm[rowBase + ip] = rho * Math.sin(phase);
    }
  }
  return { pupilRe, pupilIm, krAt, cosPhiAt, sinPhiAt };
}

function computePsfIntensityPlane(nx, ny, resLateralM, pupil) {
  const { pupilRe, pupilIm, krAt, cosPhiAt, sinPhiAt } = pupil;
  const x0 = (nx - 1) / 2, y0 = (ny - 1) / 2;
  const slice = new Float64Array(nx * ny);
  for (let y = 0; y < ny; y++) {
    const dyM = (y - y0) * resLateralM, rowOut = nx * y;
    for (let x = 0; x < nx; x++) {
      const dxM = (x - x0) * resLateralM;
      let sumRe = 0, sumIm = 0;
      for (let ir = 0; ir < PSF_N_RHO; ir++) {
        const kr = krAt[ir], rowBase = ir * PSF_N_PHI;
        for (let ip = 0; ip < PSF_N_PHI; ip++) {
          const sp = kr * (dxM * cosPhiAt[ip] + dyM * sinPhiAt[ip]);
          const cosSp = Math.cos(sp), sinSp = Math.sin(sp);
          const pr = pupilRe[rowBase + ip], pi = pupilIm[rowBase + ip];
          sumRe += pr * cosSp - pi * sinSp; sumIm += pr * sinSp + pi * cosSp;
        }
      }
      slice[rowOut + x] = sumRe * sumRe + sumIm * sumIm;
    }
  }
  return slice;
}

// Shared parameter block -- keep in sync with DumpJavaPsf.java.
const params = {
  NA: 1.4,
  lambda: 660e-9,
  ns: 1.518,
  ni: 1.518,
  ti0: 150e-6,
  particleAxialPosition: 0,
  zStep: 100e-9,
  zernikeCoeffs: (() => { const a = new Array(15).fill(0); a[5] = 0.15; return a; })(),
};
const nx = 65, ny = 65;
const resLateralM = 25e-9; // 100nm camera pixel / 4x oversampling

const pupil = computePsfPupilForZPlane(0, 1, params); // nz=1 -> in-focus, ti=ti0 exactly (particleAxialPosition=0)
const slice = computePsfIntensityPlane(nx, ny, resLateralM, pupil);

const outfile = process.argv[2];
if (!outfile) { console.error("usage: node dump_websmlm.mjs <outfile>"); process.exit(1); }

const buf = Buffer.alloc(nx * ny * 4);
for (let i = 0; i < nx * ny; i++) buf.writeFloatBE(slice[i], i * 4);
await import("node:fs/promises").then(fs => fs.writeFile(outfile, buf));
console.log(`Wrote ${nx * ny} floats (${nx}x${ny}) to ${outfile}`);
