// Shape metrics of microtubule centerlines (um), shared by the lab page (worker) and web/lab/check.mjs.
// Paths are resampled at a fixed arc step first, so the numbers do not depend on the generator's mtStepLen.

export const RESAMPLE_UM = 0.1;

export function resample(pts, step = RESAMPLE_UM) {
  if (pts.length < 2) return pts.slice();
  const out = [{ x: pts[0].x, y: pts[0].y, z: pts[0].z }];
  let acc = 0; // arc length since the last sample
  for (let i = 1; i < pts.length; i++) {
    const a = pts[i - 1], b = pts[i];
    const L = Math.hypot(b.x - a.x, b.y - a.y, b.z - a.z);
    let pos = step - acc;
    for (; pos <= L; pos += step) {
      const f = pos / L;
      out.push({ x: a.x + (b.x - a.x) * f, y: a.y + (b.y - a.y) * f, z: a.z + (b.z - a.z) * f });
    }
    acc = L - (pos - step);
  }
  return out;
}

// Per path: length, tortuosity (arc / chord), curvature (rad/um) mean and p95 from the turning angle
// between consecutive resampled segments, min turn radius, z-slope (|dz| / |dxy|) rms and max,
// zSpan, and a 3D "wiggle wavelength" = 2*pi / mean curvature.
export function pathMetrics(pts) {
  const r = resample(pts);
  const n = r.length;
  if (n < 3) return null;
  let len = 0, curvSum = 0, slopeSq = 0, slopeMax = 0, zMin = Infinity, zMax = -Infinity;
  const curv = [];
  for (let i = 1; i < n; i++) {
    const a = r[i - 1], b = r[i];
    const dxy = Math.hypot(b.x - a.x, b.y - a.y), dz = Math.abs(b.z - a.z);
    len += Math.hypot(dxy, dz);
    const s = dz / Math.max(dxy, 1e-9);
    slopeSq += s * s; slopeMax = Math.max(slopeMax, s);
    zMin = Math.min(zMin, b.z); zMax = Math.max(zMax, b.z);
    if (i < n - 1) {
      const c = r[i + 1];
      const ux = b.x - a.x, uy = b.y - a.y, uz = b.z - a.z, vx = c.x - b.x, vy = c.y - b.y, vz = c.z - b.z;
      const lu = Math.hypot(ux, uy, uz), lv = Math.hypot(vx, vy, vz);
      if (lu > 1e-12 && lv > 1e-12) {
        const cos = Math.min(1, Math.max(-1, (ux * vx + uy * vy + uz * vz) / (lu * lv)));
        const k = Math.acos(cos) / (0.5 * (lu + lv));
        curv.push(k); curvSum += k;
      }
    }
  }
  const chord = Math.hypot(r[n - 1].x - r[0].x, r[n - 1].y - r[0].y, r[n - 1].z - r[0].z);
  curv.sort((x, y) => x - y);
  const kMean = curvSum / Math.max(1, curv.length);
  const p95 = curv.length ? curv[Math.min(curv.length - 1, Math.floor(0.95 * curv.length))] : 0;
  const kMax = curv.length ? curv[curv.length - 1] : 0;
  return {
    len, tort: len / Math.max(chord, 1e-9), kMean, kP95: p95, rMin: kMax > 0 ? 1 / kMax : Infinity,
    slopeRms: Math.sqrt(slopeSq / (n - 1)), slopeMax, zSpan: zMax - zMin, curv,
  };
}

const KEYS = [
  ['count', 'MTs', 0], ['lenTotal', 'total length (um)', 1], ['lenMean', 'mean length (um)', 2],
  ['tort', 'tortuosity (arc/chord)', 3], ['kMean', 'curvature mean (rad/um)', 3], ['kP95', 'curvature p95 (rad/um)', 3],
  ['rMin', 'min turn radius, median (um)', 3], ['slopeRms', 'z slope rms', 3], ['slopeMax', 'z slope max', 2],
  ['zSpan', 'z span mean (um)', 2],
];
export const SUMMARY_KEYS = KEYS;

// Aggregate over many paths (length-weighted where it makes sense).
export function summarize(paths) {
  const ms = paths.map(pathMetrics).filter(Boolean);
  const S = { count: ms.length, lenTotal: 0 };
  if (!ms.length) return { ...S, lenMean: 0, tort: 0, kMean: 0, kP95: 0, rMin: 0, slopeRms: 0, slopeMax: 0, zSpan: 0, hist: [] };
  let w = 0, tort = 0, kMean = 0, slopeRms = 0, slopeMax = 0, zSpan = 0;
  const all = [];
  for (const m of ms) {
    S.lenTotal += m.len; w += m.len;
    tort += m.tort * m.len; kMean += m.kMean * m.len; slopeRms += m.slopeRms * m.len;
    slopeMax = Math.max(slopeMax, m.slopeMax); zSpan += m.zSpan;
    for (const k of m.curv) all.push(k);
  }
  all.sort((a, b) => a - b);
  const rMins = ms.map(m => m.rMin).sort((a, b) => a - b);
  // Curvature histogram, 0..8 rad/um in 32 bins (normalized to a density).
  const hist = new Array(32).fill(0);
  for (const k of all) hist[Math.min(31, Math.floor(k / 0.25))]++;
  for (let i = 0; i < 32; i++) hist[i] /= Math.max(1, all.length);
  return {
    ...S, lenMean: S.lenTotal / ms.length, tort: tort / w, kMean: kMean / w,
    kP95: all[Math.min(all.length - 1, Math.floor(0.95 * all.length))] || 0,
    rMin: rMins[rMins.length >> 1], slopeRms: slopeRms / w, slopeMax, zSpan: zSpan / ms.length, hist,
  };
}

export function formatSummary(s, base) {
  const rows = [];
  for (const [k, name, dp] of KEYS) {
    const a = s[k], b = base ? base[k] : undefined;
    let d = '';
    if (b != null && isFinite(a) && isFinite(b) && b !== 0) d = ((a / b - 1) * 100).toFixed(1) + '%';
    rows.push({ key: k, name, a: isFinite(a) ? a.toFixed(dp) : String(a), b: b == null ? '' : (isFinite(b) ? b.toFixed(dp) : String(b)), d });
  }
  return rows;
}
