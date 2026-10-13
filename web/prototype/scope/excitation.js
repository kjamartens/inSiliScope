// Excitation geometry (2026-10-13): the beam's in-plane profile and where the laser spot sits in the objective's back
// focal plane -- epi, HILO, TIRF -- as one 3D excitation factor. The JS reference for
// adapter/inSiliScope/Simulation/Excitation.cpp (port pending, PORT_PENDING.md).
//
// The slider u (option illum-bfp) is the spot's distance from the BFP centre in units of the critical angle's
// radius. By the sine condition the BFP radius is proportional to n_i sin(theta_i), so n_i sin(theta_i) = u n_s
// (n_i the immersion index, n_s the medium's): 0 = epi, 1 = the critical angle, > 1 = total internal reflection, up to
// the pupil edge u_max = NA / n_s. A spot beyond the pupil edge is clipped: no light reaches the sample.
//
// The beam's cross-section (in the glass) is the profile P(x', y): Flat, Gaussian or FlatTop (a logistic-edged disc,
// the SMLM Challenge 2016 simulator's; SMLMBackground.cpp), FWHM as % of the FOV width, centred on the optical axis
// (the FOV centre). The incidence plane is the camera x axis. On a plane at height z above the coverslip the beam
// lands as P((x - (z - z_p) tan theta_s) cos theta_i, y): stretched by 1/cos theta_i along x and, below the critical
// angle, moving with z (the HILO sheet; Tokunaga et al. 2008, tokunaga2008). The beam crosses the axis at the
// objective's focal plane z_p (pivotUm: the movie's focus), so the sheet is centred there on the axis; the evanescent
// footprint is centred on the axis.
//
// The factor F is |E|^2 in the sample over its epi value (so the lasers' kW/cm^2 "at the sample" hold at u = 0),
// the mean of s and p polarisation (estimate: unpolarised or circular light):
//   u < 1: Fresnel transmission, F = (t_s^2 + t_p^2) / 2 / t(0)^2 (bornwolf1999, section 1.5.2);
//   u > 1: the evanescent field at the interface, I_s = 4 cos^2 / (1 - n^2), I_p = 4 cos^2 (2 sin^2 - n^2) /
//          (n^4 cos^2 + sin^2 - n^2) (n = n_s / n_i; axelrod2001, eq. 2-3), decaying as exp(-z / d),
//          d = lambda / (4 pi sqrt(n_i^2 sin^2 - n_s^2)) = lambda / (4 pi n_s sqrt(u^2 - 1)) (axelrod2001, eq. 1).
// F is continuous at u = 1 (4 for s, 4 / n^2 for p): the interface's enhancement is a gain. The plane-wave limits
// diverge at u = 1 (d -> infinity above, a grazing sheet below); a real spot has a width in the BFP, so u keeps
// U_GAP from 1 on either side.
//
// Coordinates: um from the optical axis in x, y; z = height above the coverslip, um. shares: a dye state's
// excitation per laser line, [{nm, share}] summing to 1 (statePhotophysics lineShares): only TIRF's depth depends on
// the wavelength.
export const ILLUM_PROFILES = ['Flat', 'Gaussian', 'FlatTop'];
export const ILLUM_REGIMES = ['Epi', 'HILO', 'TIRF', 'Clipped'];
const U_GAP = 1e-3;
const FLATTOP_EDGE = 0.15;   // the logistic edge width, x the disc radius (SMLMBackground.cpp)

export function makeExcitation(o) {
  const profile = Math.trunc(o.profile ?? 0), fwhmUm = Math.max(1e-6, (o.fwhmPct / 100.0) * o.fovWidthUm);
  const nI = o.nImm, nS = o.nMed, zp = o.pivotUm ?? 0.0;
  let u = Math.max(0.0, o.bfp);
  const uMax = o.na / nS;
  const clipped = u > uMax;
  if (Math.abs(u - 1.0) < U_GAP) u = u < 1.0 ? 1.0 - U_GAP : 1.0 + U_GAP;
  const tirf = !clipped && u > 1.0;
  const regime = clipped ? 'Clipped' : (tirf ? 'TIRF' : (u > 0.0 ? 'HILO' : 'Epi'));
  const sinI = u * nS / nI, cosI = Math.sqrt(Math.max(0.0, 1.0 - sinI * sinI));
  const n = nS / nI;
  let F = 0.0, tanS = 0.0;
  if (!clipped) {
    if (tirf) {
      const c2 = cosI * cosI, s2 = sinI * sinI;
      const Is = 4.0 * c2 / (1.0 - n * n);
      const Ip = 4.0 * c2 * (2.0 * s2 - n * n) / (n * n * n * n * c2 + s2 - n * n);
      const t0 = 2.0 * nI / (nI + nS);
      F = (Is + Ip) / 2.0 / (t0 * t0);
    } else {
      const sinS = u, cosS = Math.sqrt(1.0 - sinS * sinS);
      tanS = sinS / cosS;
      const ts = 2.0 * nI * cosI / (nI * cosI + nS * cosS), tp = 2.0 * nI * cosI / (nS * cosI + nI * cosS);
      const t0 = 2.0 * nI / (nI + nS);
      F = (ts * ts + tp * tp) / 2.0 / (t0 * t0);
    }
  }
  // The in-plane profile at (x', y) um from the axis, peak 1.
  const sig = fwhmUm / 2.3548, r0 = fwhmUm / 2.0, edge = Math.max(1e-6, FLATTOP_EDGE * r0);
  const flatTop0 = 1.0 / (1.0 + Math.exp(-r0 / edge));
  const P = (xp, y) => {
    if (profile === 0) return 1.0;
    const r2 = xp * xp + y * y;
    if (profile === 1) return Math.exp(-r2 / (2.0 * sig * sig));
    return 1.0 / (1.0 + Math.exp((Math.sqrt(r2) - r0) / edge)) / flatTop0;
  };
  // Beyond this |x'| or radius the profile is below 1e-12 of its peak (the HILO integral's range).
  const reachUm = profile === 0 ? Infinity : (profile === 1 ? sig * 7.5 : r0 + 28.0 * edge);
  const decayUm = nm => 1e-3 * nm / (4.0 * Math.PI * nS * Math.sqrt(u * u - 1.0));
  const lateral = (x, y, z) => P((x - (tirf ? 0.0 : z - zp) * tanS) * cosI, y);
  const depth = (z, shares) => {
    if (!tirf) return 1.0;
    let s = 0.0;
    for (const l of shares) s += l.share * Math.exp(-Math.max(0.0, z) / decayUm(l.nm));
    return s;
  };
  // Mean of the depth factor over [z0, z1] (a WideField plane): exact for the exponentials.
  const depthMean = (z0, z1, shares) => {
    if (!tirf) return 1.0;
    const a = Math.max(0.0, z0), b = Math.max(a, z1);
    if (!(b > a)) return depth(a, shares);
    let s = 0.0;
    for (const l of shares) { const d = decayUm(l.nm); s += l.share * d * (Math.exp(-a / d) - Math.exp(-b / d)); }
    return s / (b - a);
  };
  const at = (x, y, z, shares) => clipped ? 0.0 : F * lateral(x, y, z) * depth(z, shares);
  // The column integral of the factor over the chamber [0, H] um at (x, y) (the free imager's excitation): epi
  // and TIRF in closed form, HILO by 256 midpoints over the heights the tilted beam crosses (x, y).
  const column = (x, y, H, shares) => {
    if (clipped || !(H > 0.0)) return 0.0;
    if (tirf) {
      let s = 0.0;
      for (const l of shares) { const d = decayUm(l.nm); s += l.share * d * (1.0 - Math.exp(-H / d)); }
      return F * lateral(x, y, 0.0) * s;
    }
    if (tanS === 0.0 || profile === 0) return F * lateral(x, y, 0.0) * H;
    const r = reachUm / cosI;
    const za = Math.max(0.0, zp + (x - r) / tanS), zb = Math.min(H, zp + (x + r) / tanS);
    if (!(zb > za)) return 0.0;
    const n = 256, dz = (zb - za) / n;
    let s = 0.0;
    for (let i = 0; i < n; i++) s += lateral(x, y, za + (i + 0.5) * dz);
    return F * s * dz;
  };
  // How deep the light reaches, nm: TIRF's 1/e depth at lambdaNm, HILO's sheet thickness on the axis (the beam's
  // FWHM / (tan theta_s cos theta_i)); -1 for epi, a Flat beam's HILO and a clipped spot.
  const depthNm = (lambdaNm = 640) => tirf ? decayUm(lambdaNm) * 1000.0
    : (!clipped && tanS > 0.0 && profile !== 0 ? fwhmUm / (tanS * cosI) * 1000.0 : -1.0);
  return { regime, u, uMax, clipped, tirf, F, cosI, tanS, profile, fwhmUm,
    // Exactly 1 everywhere (a Flat beam at the BFP centre): the renderers skip the factor (outputs as before).
    uniform: profile === 0 && u === 0.0,
    profileAt: P, lateral, depth, depthMean, at, column, depthNm, decayUm: tirf ? decayUm : null };
}

// Dose levels of a population whose photons per dye are nonlinear in its excitation factor a (bleaching at a x the
// rate; the mean-field images, fluorescence.js): nodes 0 < a_1 < ... <= aMax, a dye split between its two
// neighbouring nodes by linear interpolation (hatWeight), each node one image whose frame photons are those of a
// dye at exactly a_k. dMax: the largest dose (rate x a x time, in e-folds) of the movie. One node (aMax: photons
// linear in a, exact) while dMax <= DOSE_STEP; else nodes DOSE_STEP apart in dose up to DOSE_CUT e-folds (at most
// DOSE_LEVELS; interpolation error ~ DOSE_STEP^2 / 8 of a dye's photons), and aMax beyond (the dyes there are
// bleached: e^-8).
export const DOSE_STEP = 0.5, DOSE_CUT = 8.0, DOSE_LEVELS = 16;
export function doseNodes(aMax, dMax) {
  if (!(aMax > 0)) return [];
  if (!(dMax > DOSE_STEP)) return [aMax];
  const K = Math.min(DOSE_LEVELS, Math.ceil(Math.min(dMax, DOSE_CUT) / DOSE_STEP));
  const aCut = dMax > DOSE_CUT ? aMax * DOSE_CUT / dMax : aMax;
  const out = [];
  for (let k = 1; k <= K; k++) out.push(aCut * k / K);
  if (aCut < aMax) out.push(aMax);
  return out;
}
// The share of node k for a dye at factor a (the hat function over nodes [0, ...nodes]; the shares of a sum to 1
// for 0 < a <= aMax, the node at 0 has no image: its dyes are dark).
export function hatWeight(nodes, k, a) {
  const hi = nodes[k], lo = k > 0 ? nodes[k - 1] : 0.0;
  if (a >= lo && a <= hi) return hi > lo ? (a - lo) / (hi - lo) : 1.0;
  if (k + 1 < nodes.length) {
    const nx = nodes[k + 1];
    if (a > hi && a < nx) return (nx - a) / (nx - hi);
  }
  return 0.0;
}
