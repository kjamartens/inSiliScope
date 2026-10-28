// Writes the parity case list (deterministic; no randomness outside pcg4d).
//   node make_cases.mjs <out/cases.txt> [mathSamples=40000]
import fs from 'fs';
import { pcg4d } from './pcg4d.mjs';

const out = [];

// Parameter sets. "builtin" sets nothing: C++ uses its compiled-in defaults
// (frozen from the retired JS prototype's HTML defaults, so they also check the
// defaults did not move).
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
  // Nuclear pores (issue 15): no caps and no spacing (every candidate kept), and a jammed envelope (fewer pores than
  // density x area: the 50 x N candidates run out).
  npcvar: { nucAsym: 0, npcDensity: 25, npcMinDistNm: 0, npcRadiusNm: 60, npcRingSepNm: 40, npcCornerSpreadNm: 20,
            mtDensity: 0.1 },
  npcjam: { npcDensity: 30, npcMinDistNm: 250, mtDensity: 0.1 },
  // The nuclear lamina (2026-10-27): the engine's density with a wide pore exclusion, and a crowded envelope (dense
  // pores, no pore spacing, a 300 nm exclusion: many candidates rejected).
  lamin: { laminDensity: 50, laminNpcExclNm: 120, mtDensity: 0.1 },
  laminjam: { nucAsym: 0, npcDensity: 25, npcMinDistNm: 0, laminDensity: 200, laminNpcExclNm: 300, mtDensity: 0.1 },
};
for (const [name, kv] of Object.entries(PARAM_SETS))
  out.push(['params', name, ...Object.entries(kv).map(([k, v]) => `${k}=${v}`)].join(' '));

// RNG addresses: int32 edges, JS `|0` wrap of values beyond 2^32, negatives -- each edge once (the RNG is integer
// arithmetic: one wrong operation changes every address; 2026-10-27, was every combination, 252).
const SEEDS = [0, 1249, -1, 2147483647, -2147483648, 4294967295];
const COORDS = [0, 1, -1, -123456, 2147483647, -2147483648, 5000000000];
const KS = [0, 1, 44, 8000000, 4294967295, 9500000 * 4096 + 17];
for (let i = 0; i < COORDS.length; i++) {
  const s = SEEDS[i % SEEDS.length], c = COORDS[i], k = KS[(i * 5) % KS.length];
  out.push(`rng ${s} ${c} ${-c} ${k}`);
}
out.push(`rng ${SEEDS[5]} ${COORDS[6]} ${COORDS[5]} ${KS[4]}`);
for (const base of [1000, 9500000]) out.push(`stream 1249 -3 5 ${base} 20`);

out.push(`math ${+(process.argv[3] ?? 40000)}`);

// 8 packing cases, 10x10-chunk windows (2026-10-27, was 50 of 14 x 14: packing is chaotic, so one changed operation
// moves the cells of every case): two around the origin, two ~1e3 and one ~5e7 chunks away (world coords ~1e9 um),
// one per other parameter set.
const W = 10;
const PACK = [[0, 'builtin', 'near'], [1, 'builtin', 'near'], [12, 'builtin', 1000], [13, 'builtin', 1000],
              [30, 'builtin', 50000000], [35, 'blobby', 'near'], [40, 'norot', 1000], [45, 'dense', 'near']];
for (const [i, pset, scale] of PACK) {
  const [h0, h1, h2] = pcg4d(0xC0FFEE, i, 0, 0);
  const seed = i === 0 ? 1249 : i === 1 ? -7 : (h0 & 0x7fffffff);
  let ox, oy;
  if (scale === 'near') { ox = (h1 % 7) - 10; oy = (h2 % 7) - 10; }
  else {
    ox = Math.floor((h1 / 4294967296 - 0.5) * 2 * scale);
    oy = Math.floor((h2 / 4294967296 - 0.5) * 2 * scale);
  }
  out.push(`case c${String(i).padStart(2, '0')} ${seed} ${pset} ${ox} ${oy} ${ox + W - 1} ${oy + W - 1}`);
}

// Cytoplasm mesh + microtubules: the first present cell of a window (one per parameter set).
out.push('cells m00 1249 builtin -2 -2 1 1 1');
out.push('cells m01 -7 mtsparse -3 -3 3 3 1');
out.push('cells m02 90210 mtvar 400 -800 403 -797 1');
out.push('cells m03 5 mtflat -1000000 1000000 -999997 1000003 1');
out.push('cells m05 77 rough -4 -4 0 0 1');
out.push('cells m06 2024 nucplain -2 -2 2 2 1');
out.push('cells m07 9001 nucwild -2 -2 2 2 1');
out.push('case n00 4711 nucwild -4 -4 5 5');
out.push('case r00 31337 rough -4 -4 5 5');

// DNA-origami nanorulers (specimen 1, ABI 12): the molecules of a window (origami.js / origami.cpp), their bent
// outline and every designed strand. origami <id> <seed> <x0> <y0> <x1> <y1> [origami param=value ...]
// Nuclear pores (structure 1, ABI 13): the pores of the first present cells (npcs.cpp / the prototype's
// buildNpcsForCell) and the dyes of their first 40 pores under the pores' built-in linkage and a flexible one.
// npcs <id> <seed> <params> <cx0> <cy0> <cx1> <cy1> <maxCells>: builtin = shaped with a bottom cap, nucplain = the
// plain ellipsoid, nucwild = a top cap.
out.push('npcs p00 1249 builtin -2 -2 1 1 1');
out.push('npcs p01 2024 nucplain -2 -2 2 2 1');
out.push('npcs p02 9001 nucwild -2 -2 2 2 1');
out.push('npcs p03 77 npcvar -3 -3 3 3 1');
out.push('npcs p04 4711 npcjam -3 -3 3 3 1');
// The nuclear lamina (structure 2, ABI 16): lamin <id> <seed> <params> <cx0> <cy0> <cx1> <cy1> <maxCells>.
out.push('lamin l00 1249 lamin -2 -2 1 1 1');
out.push('lamin l01 77 laminjam -3 -3 3 3 1');
out.push('origami o00 1249 0 0 3 3');
out.push('origami o01 -7 -3 -3 3 3 origamiLpNm=273.1 origamiHeights=1 origamiLinkerNm=0 origamiDensity=3');
out.push('origami o02 90210 100000 -100000 100004 -99996 origamiBend=0 origamiRoll=1 origamiRollDeg=30 origamiStrandsPerSpot=3 origamiDensity=2');
out.push('origami o03 5 12345678 -9876543 12345681 -9876540 origamiDensity=8 origamiMinSpacingNm=0 origamiLpNm=50 origamiAttachNm=20');
out.push('origami o04 31 -2 -2 4 4 origamiSpacingNm=40 origamiLpNm=600 origamiDensity=2');
out.push('origami o05 77 -1 -1 5 5 origamiDesign=1 origamiSpacingNm=20 origamiHeights=1 origamiDensity=2');
out.push('origami o06 78 0 0 5 5 origamiDesign=1 origamiSpacingNm=160 origamiRoll=1 origamiRollDeg=40');
// Fiducial beads (ABI 15): the beads of a window (fiducials.cpp) in each height mode, on a step surface
// (1.5 um where x + y > 0; the World's own surface: world_checks.fiducials).
// fiducials <id> <seed> <x0> <y0> <x1> <y1> [fid param=value ...]
out.push('fiducials f00 1249 0 0 10 10 fidDensity=0.5 fidHeight=0');
out.push('fiducials f01 -7 -5 -5 5 5 fidDensity=2 fidDiameterNm=150 fidDiameterCvPct=30 fidHeight=1');
out.push('fiducials f02 90210 100000 -100000 100006 -99994 fidDensity=1 fidHeight=2 fidLayerUm=4');
out.push('fiducials f03 5 -3 -3 3 3 fidDensity=20 fidDiameterNm=2000 fidDiameterCvPct=50 fidHeight=2 fidLayerUm=1');

fs.writeFileSync(process.argv[2], out.join('\n') + '\n');
console.log(`wrote ${out.length} lines to ${process.argv[2]}`);
