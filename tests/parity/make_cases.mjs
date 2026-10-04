// Writes the parity case list (deterministic; no randomness outside pcg4d).
//   node make_cases.mjs <out/cases.txt> [mathSamples=40000]
import fs from 'fs';
import { pcg4d } from './pcg4d.mjs';

const out = [];

// Parameter sets. "builtin" sets nothing: C++ uses its compiled-in defaults,
// the JS reference the prototype's HTML defaults -- so the default cases also
// check that the two agree.
const PARAM_SETS = {
  builtin: {},
  blobby: { cellBlob: 3.2, density: 0.6 },
  // Fractal edge tail: strong and rough (the soft clamp binds), relaxation short.
  rough: { cellRough: 0.6, cellFractalDim: 1.8, cellBlob: 2.5, cytoRelaxUm: 0.4 },
  norot: { allowPackRotation: 0, packFrac: 0.9, relaxIters: 40 },
  dense: { density: 0.9, chunkSize: 20, jitter: 1, cellElongMin: 0.3 },
  // Microtubules: sparse enough that the collision pass runs (it is skipped
  // above 8000 points per cell), with a wide separation so it nudges and resamples.
  mtsparse: { mtDensity: 0.03, mtMinSeparation: 0.25, mtStartDecayPct: 4 },
  mtvar: { mtDensity: 0.3, mtStepLen: 0.1, cellBlob: 2.5, mtDirKappa: 6, mtEndDecayPct: 5, mtWobbleTurn: 1.6, mtSmoothLen: 4,
           nucMargin: 1.2, cytoMaxSlope: 0.5, cytoDomeSlope: 1.5, mtMaxZSlope: 2 },
  mtflat: { mtDensity: 0.1, mtWobbleTurn: 0, mtMinTurnRadius: 0, mtSmoothLen: 0, cytoRelaxUm: 0, cellRough: 0,
            cytoMaxSlope: 0, cytoDomeSlope: 0, cytoRings: 20, cytoTheta: 48, mtMinSeparation: 0.3 },
  // Nucleus: the plain ellipsoid (every shape term off), and the shape at its slider extremes.
  nucplain: { nucIrregMin: 0, nucIrregMax: 0, nucBendMin: 0, nucBendMax: 0, nucThickIrreg: 0, nucAsym: 0,
              nucWidestMin: 0.5, nucWidestMax: 0.5, mtDensity: 0.1 },
  nucwild: { nucIrregMin: 0.3, nucIrregMax: 0.3, nucBendMin: 1, nucBendMax: 1, nucSmooth: 0, nucThickIrreg: 0.4,
             nucAsym: -0.9, nucWidestMin: 1, nucWidestMax: 1, nucHeightMin: 0.4, nucHeightMax: 0.4, mtDensity: 0.1,
             mtDirKappa: 0 },
};
for (const [name, kv] of Object.entries(PARAM_SETS))
  out.push(['params', name, ...Object.entries(kv).map(([k, v]) => `${k}=${v}`)].join(' '));

// RNG addresses: int32 edges, JS `|0` wrap of values beyond 2^32, negatives.
const SEEDS = [0, 1249, -1, 2147483647, -2147483648, 4294967295];
const COORDS = [0, 1, -1, -123456, 2147483647, -2147483648, 5000000000];
const KS = [0, 1, 44, 8000000, 4294967295, 9500000 * 4096 + 17];
for (const s of SEEDS) for (const c of COORDS) for (const k of KS) out.push(`rng ${s} ${c} ${-c} ${k}`);
for (const base of [1000, 100000, 8000000, 9500000]) out.push(`stream 1249 -3 5 ${base} 20`);

out.push(`math ${+(process.argv[3] ?? 40000)}`);

// 50 packing cases, 14x14-chunk windows. First 10 around the origin, the
// rest scattered up to ~5e7 chunks away (world coords ~1e9 um).
const W = 14;
for (let i = 0; i < 50; i++) {
  const [h0, h1, h2] = pcg4d(0xC0FFEE, i, 0, 0);
  const seed = i === 0 ? 1249 : i === 1 ? -7 : (h0 & 0x7fffffff);
  let ox, oy;
  if (i < 10) { ox = (h1 % 7) - 10; oy = (h2 % 7) - 10; }
  else {
    const scale = i < 30 ? 1000 : 50000000;
    ox = Math.floor((h1 / 4294967296 - 0.5) * 2 * scale);
    oy = Math.floor((h2 / 4294967296 - 0.5) * 2 * scale);
  }
  const pset = i < 35 ? 'builtin' : i < 40 ? 'blobby' : i < 45 ? 'norot' : 'dense';
  out.push(`case c${String(i).padStart(2, '0')} ${seed} ${pset} ${ox} ${oy} ${ox + W - 1} ${oy + W - 1}`);
}

// Cytoplasm mesh + microtubules: first N present cells of a window.
out.push('cells m00 1249 builtin -2 -2 1 1 2');
out.push('cells m01 -7 mtsparse -3 -3 3 3 4');
out.push('cells m02 90210 mtvar 400 -800 403 -797 2');
out.push('cells m03 5 mtflat -1000000 1000000 -999997 1000003 2');
out.push('cells m04 424242 mtsparse 12 12 20 20 3');
out.push('cells m05 77 rough -4 -4 0 0 2');
out.push('cells m06 2024 nucplain -2 -2 2 2 2');
out.push('cells m07 9001 nucwild -2 -2 2 2 2');
out.push('case n00 4711 nucwild -6 -6 7 7');
out.push('case r00 31337 rough -6 -6 7 7');

fs.writeFileSync(process.argv[2], out.join('\n') + '\n');
console.log(`wrote ${out.length} lines to ${process.argv[2]}`);
