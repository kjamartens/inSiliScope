#!/usr/bin/env node
// Builds the DNA-origami design table the Nanorulers specimen reads, from data/origami/designs.json:
//   node tools/gen_origami_designs.mjs [--check]
// Output (never edit by hand; --check fails when it is stale):
//   web/prototype/scope/origami_designs_data.js  the JS reference's table (origami.js)
//   core/src/origami_designs.inc               the core's table (origami.cpp)
//   web/origami_designs.js                     the viewer's panel (its Design select)
// Validates every design and expands it into spots in lattice units (positions = units x origamiSpacingNm): bodies
// 'cylinder' (ring spots at lattice steps 'at') and 'plate' (a hex map of CCRR cells, one strand at each centre),
// deformation 'wlc2d' / 'rigid', roll 'uniform' / 'fixed' (cylinders) and 'flat' (plates); the reserved values of the
// schema (other bodies and deformations, stiffness patches, built-in curvature, explicit strand lists) are refused with
// a message. BSD-3-Clause.
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const CHECK = process.argv.includes('--check');
const read = f => JSON.parse(fs.readFileSync(path.join(ROOT, f), 'utf8'));
const src = read('data/origami/designs.json');
const references = read('data/references.json').references;

const fail = (d, msg) => { throw new Error(`data/origami/designs.json ${d.id ?? '?'}: ${msg}`); };
const num = (d, v, what, lo = 0) => { if (typeof v !== 'number' || !Number.isFinite(v) || v < lo) fail(d, `${what} must be a number >= ${lo}`); return v; };
const BODIES = { cylinder: true, plate: true, cuboid: false, mesh: false };
const DEFORMATIONS = { wlc2d: true, rigid: true, wlc3d: false, plate: false };
const ROLLS = { cylinder: ['uniform', 'fixed'], plate: ['flat'] };
const HEX_COL = Math.sqrt(3) / 2, HEX_R = 1 / Math.sqrt(3);   // flat-topped hexagons of unit neighbour distance

// A design's spots in lattice units (u along the body, v across, both from the body's centre; positions = units x
// origamiSpacingNm) and the body's extent: length = lengthUnits x spacing + 2 endNm, in-plane width = widthUnits x
// spacing + widthNm, height heightNm.
function cylinderSpots(d, b) {
  if (!Array.isArray(d.spots) || !d.spots.length) fail(d, 'needs spots');
  for (const s of d.spots) if (s.atNm !== undefined) fail(d, `spot ${s.id}: 'atNm' is gone, give 'at' in lattice steps (origamiSpacingNm)`);
  const at = d.spots.map(s => num(d, s.at, `spot ${s.id} at (lattice steps)`));
  const lo = Math.min(...at), hi = Math.max(...at), mid = (lo + hi) / 2;
  const spots = d.spots.map((s, k) => {
    if (s.strands) fail(d, `spot ${s.id}: explicit strand lists are reserved, not implemented yet (use a ring)`);
    const r = s.ring ?? {};
    const n = num(d, r.n, `spot ${s.id} ring.n`, 1);
    if (!Number.isInteger(n) || n > 64) fail(d, `spot ${s.id}: ring.n must be an integer 1-64`);
    return { id: String(s.id), u: at[k] - mid, v: 0, ringN: n, radiusNm: num(d, r.radiusNm, `spot ${s.id} ring.radiusNm`),
      phaseDeg: r.phaseDeg ?? 0 };
  });
  const diameterNm = num(d, b.diameterNm, 'body.diameterNm', 0.1);
  return { spots, lengthUnits: hi - lo, endNm: num(d, b.endNm, 'body.endNm'), widthUnits: 0, widthNm: diameterNm,
    heightNm: diameterNm };
}

function plateSpots(d, b) {
  const g = d.lattice ?? {};
  if (g.type !== 'hex') fail(d, `lattice.type '${g.type}' is not implemented (hex)`);
  if ((g.orientation ?? 'flat-top') !== 'flat-top' || (g.offset ?? 'odd-q-down') !== 'odd-q-down')
    fail(d, 'lattice: only orientation flat-top with offset odd-q-down is implemented');
  const cols = num(d, g.cols, 'lattice.cols', 1), rows = num(d, g.rows, 'lattice.rows', 1);
  if (!Number.isInteger(cols) || !Number.isInteger(rows) || cols > 100 || rows > 100) fail(d, 'lattice cols/rows: integers 1-100');
  if (!Array.isArray(g.cells) || !g.cells.length) fail(d, 'lattice needs cells');
  // The plate: the bounding box of every hexagon of the map. Image rows run down = +y (as on the camera and the
  // viewer's screen), so the map reads unmirrored.
  const u0 = -HEX_R, u1 = (cols - 1) * HEX_COL + HEX_R;
  const v0 = -0.5, v1 = (rows - 1) + 0.5 + (cols > 1 ? 0.5 : 0);
  const uc = (u0 + u1) / 2, vc = (v0 + v1) / 2;
  const seen = new Set();
  const spots = g.cells.map(cell => {
    const m = /^(\d{2})(\d{2})$/.exec(String(cell));
    if (!m) fail(d, `lattice cell '${cell}' is not CCRR (column, row)`);
    const c = +m[1], r = +m[2];
    if (c >= cols || r >= rows) fail(d, `lattice cell '${cell}' lies outside the ${cols} x ${rows} map`);
    if (seen.has(cell)) fail(d, `lattice cell '${cell}' listed twice`);
    seen.add(cell);
    return { id: String(cell), u: c * HEX_COL - uc, v: r + (c % 2) * 0.5 - vc, ringN: 1, radiusNm: 0, phaseDeg: 0 };
  });
  const marginNm = num(d, b.marginNm ?? 0, 'body.marginNm');
  return { spots, lengthUnits: u1 - u0, endNm: marginNm, widthUnits: v1 - v0, widthNm: 2 * marginNm,
    heightNm: num(d, b.thicknessNm, 'body.thicknessNm', 0.1) };
}

const designs = src.designs.map(d => {
  if (!d.id || !d.name) fail(d, 'needs id and name');
  if (!/^[A-Za-z0-9_]+$/.test(d.id)) fail(d, 'id: letters, digits and _ only (a Micro-Manager value)');
  for (const r of d.refs ?? []) if (!references[r]) fail(d, `unknown reference '${r}'`);
  const b = d.body ?? {};
  if (!(b.shape in BODIES)) fail(d, `body '${b.shape}' is not one of ${Object.keys(BODIES).join(', ')}`);
  if (!BODIES[b.shape]) fail(d, `body '${b.shape}' is reserved, not implemented yet`);
  const f = d.deformation ?? {};
  if (!(f.model in DEFORMATIONS)) fail(d, `deformation '${f.model}' is not one of ${Object.keys(DEFORMATIONS).join(', ')}`);
  if (!DEFORMATIONS[f.model]) fail(d, `deformation '${f.model}' is reserved, not implemented yet`);
  if (b.shape === 'plate' && f.model !== 'rigid') fail(d, 'a plate is rigid (deformation rigid)');
  if ((f.stiffnessVariation ?? 0) !== 0 || (f.curvaturePerUm ?? 0) !== 0)
    fail(d, 'stiffnessVariation and curvaturePerUm are reserved (must be 0)');
  const l = d.landing ?? {};
  if (!ROLLS[b.shape].includes(l.roll)) fail(d, `landing.roll '${l.roll}' is not one of ${ROLLS[b.shape].join(', ')} for a ${b.shape}`);
  const geo = b.shape === 'plate' ? plateSpots(d, b) : cylinderSpots(d, b);
  if (geo.spots.length > 255) fail(d, 'at most 255 spots');
  return {
    id: d.id, name: d.name, shape: b.shape, lengthUnits: geo.lengthUnits, endNm: geo.endNm, widthUnits: geo.widthUnits,
    widthNm: geo.widthNm, heightNm: geo.heightNm, deformation: f.model,
    persistenceLengthNm: f.model === 'wlc2d' ? num(d, f.persistenceLengthNm, 'deformation.persistenceLengthNm', 1) : 0,
    segmentNm: f.model === 'wlc2d' ? num(d, f.segmentNm ?? geo.heightNm / 2, 'deformation.segmentNm', 0.01) : 0,
    roll: l.roll, rollDeg: l.rollDeg ?? 0, attachNm: num(d, l.attachNm, 'landing.attachNm'),
    spots: geo.spots, maxRingN: Math.max(...geo.spots.map(s => s.ringN)), refs: d.refs ?? [],
  };
});
designs.forEach((d, i) => { if (designs.findIndex(e => e.id === d.id) !== i) throw new Error(`designs.json: id '${d.id}' twice`); });

const header = '// Generated by tools/gen_origami_designs.mjs from data/origami/designs.json -- do not edit (BSD-3-Clause).\n';
const js = `${header}export const ORIGAMI_DESIGNS = ${JSON.stringify(designs, null, 1)};\n`;

// The core's table (core/src/origami.h OrigamiDesign): numbers as JS prints them (shortest round trip), which a C++
// compiler reads back to the same doubles.
const cstr = s => JSON.stringify(String(s));
const SHAPE = { cylinder: 'ORIGAMI_CYLINDER', plate: 'ORIGAMI_PLATE' };
const DEF = { rigid: 'ORIGAMI_RIGID', wlc2d: 'ORIGAMI_WLC2D' };
const ROLL = { uniform: 'ORIGAMI_ROLL_UNIFORM', fixed: 'ORIGAMI_ROLL_FIXED', flat: 'ORIGAMI_ROLL_FLAT' };
const inc = header + designs.map((d, i) =>
  `static const OrigamiSpot kOrigamiSpots${i}[] = {\n` +
  d.spots.map(s => `   { ${cstr(s.id)}, ${s.u}, ${s.v}, ${s.ringN}, ${s.radiusNm}, ${s.phaseDeg} },\n`).join('') + '};\n').join('') +
  'static const OrigamiDesign kOrigamiDesigns[] = {\n' + designs.map((d, i) =>
    `   { ${cstr(d.id)}, ${cstr(d.name)}, ${SHAPE[d.shape]}, ${d.lengthUnits}, ${d.endNm}, ${d.widthUnits}, ${d.widthNm}, ` +
    `${d.heightNm}, ${DEF[d.deformation]}, ${d.persistenceLengthNm}, ${d.segmentNm}, ${ROLL[d.roll]}, ${d.rollDeg}, ` +
    `${d.attachNm}, kOrigamiSpots${i}, ${d.spots.length}, ${d.maxRingN} },\n`).join('') +
  '};\n';

// The viewer's panel (a classic script, before any module loads): the design list for its Design select.
const viewer = `${header}self.ISC_ORIGAMI_DESIGNS = ${JSON.stringify(designs)};\n`;

const outputs = [['web/prototype/scope/origami_designs_data.js', js], ['core/src/origami_designs.inc', inc],
  ['web/origami_designs.js', viewer]];
let stale = 0;
for (const [rel, text] of outputs) {
  const file = path.join(ROOT, rel);
  const old = fs.existsSync(file) ? fs.readFileSync(file, 'utf8').replace(/\r\n/g, '\n') : null;
  if (old === text) continue;
  if (CHECK) { console.error(`stale: ${rel} (run node tools/gen_origami_designs.mjs)`); stale++; continue; }
  fs.writeFileSync(file, text);
  console.log(`wrote ${rel}`);
}
if (stale) process.exit(1);
if (CHECK) console.log('origami designs up to date');
