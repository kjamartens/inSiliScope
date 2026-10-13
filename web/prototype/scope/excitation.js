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
  // The free imager's collected column (imagerColumnAt, imagerMap): the light of the dyes at height z reaches the
  // camera pixel at (x, y) defocused, spread over a disc of radius |z - z_p| tan theta (theta: the emission angle in
  // the medium), so a pixel collects the excitation within that reach of it, not the column above it alone. Rings:
  // equal-weight quantiles of the collected emission angles (collectionTans); z: intervals growing away from the focal
  // plane (epi, HILO) or COLLECT_TIRF_STEPS equal steps over the evanescent layer (TIRF), each weighted by its exact
  // depth integral and evaluated at its middle.
  const scaleUm = profile === 1 ? sig : edge;
  const dirsOf = nd => {
    const ds = [];
    for (let k = 0; k < nd; k++) { const a = 2.0 * Math.PI * (k + 0.5) / nd; ds.push([Math.cos(a), Math.sin(a)]); }
    return ds;
  };
  // The z intervals of the chamber [0, Hc]: {zm, w}, w = the sum over the imagers {rate, shares} of rate x the depth
  // integral over the interval.
  const intervals = (Hc, imagers, ratio, nTirf) => {
    const out = [];
    const add = (a, b) => {
      if (!(b > a)) return;
      let w = 0.0;
      for (const L of imagers) w += L.rate * (depthMean(a, b, L.shares) * (b - a));
      out.push({ zm: 0.5 * (a + b), w });
    };
    if (tirf) {
      let dMax = 0.0;
      for (const L of imagers) for (const l of L.shares) dMax = Math.max(dMax, decayUm(l.nm));
      const top = Math.min(Hc, 30.0 * dMax);
      for (let i = 0; i < nTirf; i++) add(top * i / nTirf, top * (i + 1) / nTirf);
      return out;
    }
    const f = Math.min(Hc, Math.max(0.0, zp)), reach = Math.max(f, Hc - f);
    let e0 = 0.0, e1 = COLLECT_DZ0;
    while (e0 < reach) {
      add(Math.max(0.0, f - e1), f - e0);
      add(f + e0, Math.min(Hc, f + e1));
      e0 = e1;
      e1 *= ratio;
    }
    return out;
  };
  // The collected column at (x, y): the intervals' weights x the footprint's mean over the rings (a ring smaller than
  // `small` um: its centre).
  const collectedAt = (x, y, ivs, rings, ds, small) => {
    let s = 0.0;
    const tans = rings.tans, ws = rings.weights, nr = tans.length, nd = ds.length;
    for (const iv of ivs) {
      if (iv.w === 0.0) continue;
      const dz = Math.abs(iv.zm - zp);
      let b = 0.0;
      for (let j = 0; j < nr; j++) {
        const rho = dz * tans[j];
        if (rho < small) { b += ws[j] * lateral(x, y, iv.zm); continue; }
        let r = 0.0;
        for (let k = 0; k < nd; k++) r += lateral(x + rho * ds[k][0], y + rho * ds[k][1], iv.zm);
        b += ws[j] * (r / nd);
      }
      s += iv.w * b;
    }
    return F * s;
  };
  // One pixel at a chosen accuracy (the checks' dense reference): opts {rings (0: Gauss-Legendre), dirs, ratio,
  // tirfSteps, fresnel}.
  const imagerColumnAt = (x, y, Hc, imagers, opts = {}) => {
    if (clipped || !(Hc > 0.0)) return 0.0;
    const rings = collectionRings(nS, nI, o.na, opts.rings ?? 0, opts.fresnel ?? true);
    return collectedAt(x, y, intervals(Hc, imagers, opts.ratio ?? COLLECT_RATIO, opts.tirfSteps ?? COLLECT_TIRF_STEPS),
      rings, dirsOf(opts.dirs ?? COLLECT_DIRS), 0.0);
  };
  // The map over a W x Hpx camera (pixel (i, j) at (i um - axisX, j um - axisY)), per second: evaluated on a node grid
  // (spacing a third of the profile's edge width or sigma, at least a pixel, at most COLLECT_NODES per side) and
  // interpolated bilinearly (the defocused light is smooth; the grid resolves the footprint's edge). A Flat beam lights
  // every plane evenly, so the spread changes nothing there: its column, exactly.
  const imagerMap = (W, Hpx, um, axisX, axisY, Hc, imagers) => {
    const out = new Float64Array(W * Hpx);
    if (clipped || !(Hc > 0.0) || !imagers.length) return out;
    if (profile === 0) {
      for (let j = 0; j < Hpx; j++) for (let i = 0; i < W; i++) {
        let v = 0.0;
        for (const L of imagers) v += L.rate * column(i * um - axisX, j * um - axisY, Hc, L.shares);
        out[j * W + i] = v;
      }
      return out;
    }
    const g = Math.max(um, scaleUm / 3.0);
    const axis = n => n <= 1 ? 1 : Math.min(COLLECT_NODES, Math.max(2, Math.ceil((n - 1) * um / g) + 1));
    const nx = axis(W), ny = axis(Hpx);
    const nodeAt = (k, nn, n) => nn <= 1 ? 0.0 : (n - 1) * um * k / (nn - 1);
    const ivs = intervals(Hc, imagers, COLLECT_RATIO, COLLECT_TIRF_STEPS), small = 0.125 * g;
    const rings = collectionRings(nS, nI, o.na, 0, true), ds = dirsOf(COLLECT_DIRS);
    const node = new Float64Array(nx * ny);
    for (let b = 0; b < ny; b++) for (let a = 0; a < nx; a++)
      node[b * nx + a] = collectedAt(nodeAt(a, nx, W) - axisX, nodeAt(b, ny, Hpx) - axisY, ivs, rings, ds, small);
    // Pixel p of n on nn nodes: the node below it and the fraction towards the next.
    const cellOf = (p, n, nn) => {
      if (nn <= 1) return [0, 0.0];
      const f = p * (nn - 1) / (n - 1), k = Math.min(nn - 2, Math.floor(f));
      return [k, f - k];
    };
    for (let j = 0; j < Hpx; j++) {
      const [b, fy] = cellOf(j, Hpx, ny), b1 = ny > 1 ? b + 1 : b;
      for (let i = 0; i < W; i++) {
        const [a, fx] = cellOf(i, W, nx), a1 = nx > 1 ? a + 1 : a;
        const v00 = node[b * nx + a], v10 = node[b * nx + a1], v01 = node[b1 * nx + a], v11 = node[b1 * nx + a1];
        out[j * W + i] = (1.0 - fy) * ((1.0 - fx) * v00 + fx * v10) + fy * ((1.0 - fx) * v01 + fx * v11);
      }
    }
    return out;
  };
  return { regime, u, uMax, clipped, tirf, F, cosI, tanS, profile, fwhmUm,
    // Exactly 1 everywhere (a Flat beam at the BFP centre): the renderers skip the factor (outputs as before).
    uniform: profile === 0 && u === 0.0,
    profileAt: P, lateral, depth, depthMean, at, column, imagerColumnAt, imagerMap, depthNm, decayUm: tirf ? decayUm : null };
}

// The collection of a defocused dye's light (the free imager's background; imagerMap). A freely rotating imager emits
// isotropically; its light at emission angle theta in the medium (n_s) crosses the focal plane |z - z_p| tan theta
// from the dye (geometric optics in one medium: the coverslip and immersion mismatch is left out) and enters the
// glass (n_i) with the Fresnel power transmittance T = (n_i cos theta_i / n_s cos theta) (t_s^2 + t_p^2) / 2
// (bornwolf1999, section 1.5.3; unpolarised: the mean of s and p). An aplanatic objective (the sine condition)
// collects equal power per solid angle in object space -- its cos^(1/2) apodization only redistributes the pupil's
// amplitude (richards1959) -- up to theta_max = asin(NA / n_s) (all of the medium's half space when NA >= n_s; the
// supercritical near field is left out). So the collected light is distributed over cos theta as T on
// [cos theta_max, 1]: the rings sit at quantiles q of it (as tan theta) -- 16-point Gauss-Legendre in q (n = 0; the
// renderers: the tail of large rings is what an equal-weight midpoint rule gets wrong, 3 % at 16) or n equal-weight
// midpoints (the checks' dense reference). The dye's detected fraction stays the label's (the weights sum to 1).
// fresnel false: T = 1 (isotropic; the checks' closed form).
export const COLLECT_DIRS = 24, COLLECT_RATIO = 1.15, COLLECT_DZ0 = 0.05, COLLECT_TIRF_STEPS = 64, COLLECT_NODES = 64;
// Gauss-Legendre, 16 points on [-1, 1]: the positive nodes and their weights (numpy leggauss).
const GL16_X = [0.09501250983763744, 0.2816035507792589, 0.45801677765722737, 0.6178762444026438, 0.755404408355003,
  0.8656312023878318, 0.9445750230732326, 0.9894009349916499];
const GL16_W = [0.18945061045506864, 0.18260341504492364, 0.16915651939500265, 0.1495959888165767, 0.12462897125553407,
  0.0951585116824926, 0.062253523938647456, 0.027152459411754176];
export function collectionRings(nS, nI, na, n, fresnel = true) {
  const cMin = na >= nS ? 0.0 : Math.sqrt(1.0 - (na / nS) * (na / nS));
  const STEPS = 4096, dc = (1.0 - cMin) / STEPS;
  const T = c => {
    if (!fresnel) return 1.0;
    const s = Math.sqrt(1.0 - c * c), sg = nS * s / nI, cg = Math.sqrt(1.0 - sg * sg);
    const ts = 2.0 * nS * c / (nS * c + nI * cg), tp = 2.0 * nS * c / (nI * c + nS * cg);
    return (nI * cg) / (nS * c) * (ts * ts + tp * tp) / 2.0;
  };
  // The cumulative weight from cos theta = 1 down (theta growing), STEPS midpoints.
  const cum = new Float64Array(STEPS + 1);
  for (let k = 0; k < STEPS; k++) cum[k + 1] = cum[k] + T(1.0 - (k + 0.5) * dc) * dc;
  // The quantiles in increasing order and their weights.
  const qs = [], ws = [];
  if (n === 0) {
    for (let j = 7; j >= 0; j--) { qs.push(0.5 - 0.5 * GL16_X[j]); ws.push(0.5 * GL16_W[j]); }
    for (let j = 0; j < 8; j++) { qs.push(0.5 + 0.5 * GL16_X[j]); ws.push(0.5 * GL16_W[j]); }
  } else for (let j = 0; j < n; j++) { qs.push((j + 0.5) / n); ws.push(1.0 / n); }
  const tans = [];
  let k = 0;
  for (const qq of qs) {
    const q = qq * cum[STEPS];
    while (k < STEPS - 1 && cum[k + 1] < q) k++;
    const c = 1.0 - (k + (q - cum[k]) / (cum[k + 1] - cum[k])) * dc;
    tans.push(Math.sqrt(1.0 - c * c) / c);
  }
  return { tans, weights: ws };
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
