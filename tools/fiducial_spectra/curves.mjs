// The fiducial-marker spectra read from Balinovic et al. 2019 (data/fiducials/paper_spectra.json) on the spectra grid,
// and the paper's quoted efficiencies computed from them. Shared by tools/gen_dye_library.mjs (spectra 'cs:<id>'),
// fit_tails.mjs and check.mjs. BSD-3-Clause.
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { GRID_N, gridNm, sampleAt, idealTransmission, detection } from '../../web/prototype/scope/spectra.js';

export const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..', '..');
export const PAPER_FILE = path.join(ROOT, 'data', 'fiducials', 'paper_spectra.json');
export const readPaper = () => JSON.parse(fs.readFileSync(PAPER_FILE, 'utf8'));

// A curve on the grid, peak 1: linear between its points (% of the peak), 0 outside; with a tail, below its first
// non-zero point [nm0, v0] the exponential v0 exp(-(nm0 - nm) / decayNm). Six significant digits (the tails reach
// 1e-5 of the peak, below FPbase's 1e-4 rounding).
export function curveValues(curve, decayNm = curve.tail?.decayNm) {
  const pts = curve.points, peak = Math.max(...pts.map(p => p[1]));
  const first = pts.findIndex(p => p[1] > 0);
  const out = new Array(GRID_N).fill(0);
  for (let i = 0; i < GRID_N; ++i) {
    const nm = gridNm(i);
    let v = 0;
    if (curve.tail && nm < pts[first][0]) {
      if (!(decayNm > 0)) throw new Error('tail without decayNm: run tools/fiducial_spectra/fit_tails.mjs');
      v = pts[first][1] * Math.exp(-(pts[first][0] - nm) / decayNm);
    } else if (nm >= pts[0][0] && nm <= pts[pts.length - 1][0]) {
      let k = 0;
      while (k < pts.length - 2 && pts[k + 1][0] < nm) ++k;
      const [a, va] = pts[k], [b, vb] = pts[k + 1];
      v = b > a ? va + (vb - va) * (nm - a) / (b - a) : va;
    }
    v /= peak;
    out[i] = v < 1e-9 ? 0 : Number(v.toPrecision(6));
  }
  return out;
}

// One quoted efficiency as computed from a curve: [{ what, quoted, computed }].
export function quotedEfficiency(q, values) {
  if (q.laserNm) return [{ what: `excitation at ${q.laserNm} nm`, quoted: q.value, computed: sampleAt(values, q.laserNm) }];
  return q.bands.map(([lo, hi, v]) => ({ what: `emission in ${lo}-${hi} nm`, quoted: v,
    computed: detection(values, [idealTransmission({ type: 'bandpass', loNm: lo, hiNm: hi })]).fraction }));
}

// JSON with number arrays (and arrays of number pairs) on one line.
export const formatJson = obj => JSON.stringify(obj, null, 2)
  .replace(/\[\s*(-?[\d.eE+-]+)\s*,\s*(-?[\d.eE+-]+)\s*(?:,\s*(-?[\d.eE+-]+)\s*)?\]/g, (m, ...g) => `[${g.slice(0, 3).filter(x => x !== undefined).join(',')}]`)
  .replace(/\[\s*((?:\[[\d.,eE+-]+\]\s*,?\s*)+)\]/g, (m, inner) => `[${inner.replace(/\s+/g, '')}]`) + '\n';
