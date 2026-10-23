// Issue 16 labels: the C++ core (WASM build, C ABI 11) against the JS reference (web/prototype/scope/world.js),
// number for number and in order: fluorescent dyes (sitesInWindow), blinks (eventsInWindow) and continuous windows
// (continuousInWindow) for every label mode, a fluorescent fraction, a pre state and an initial ON, over a query
// history (late window first, then a long one, then early ones); then ABI 11 kinetics histories (rate steps per mode,
// two histories alternating on one world); the same for a DNA-origami nanoruler world (ABI 12, specimen 1: the
// docking strands, structure 2 since ABI 13), and for the cell field's nuclear pores (ABI 13, structure 1) and the
// label linkage (ISC_LABEL_BINDER_NM ..; a label without one sends NaN = the structure's built-in numbers); then ABI 14
// clock maps (isc_events_in_map / isc_continuous_in_map against world.js eventsInMap / continuousInMap): dose clocks
// with shear, a depth table, an unlit band (frozen dyes) and epochs (one giving way), activation clocks with steps,
// DNA-PAINT and dSTORM read on T, a uniform global clock; a moved present on the same worlds; the map removed.
//   node tests/parity/label_parity.mjs [path to build/wasm/core/insiliscope.js] [--section=labels|histories|maps]
// Without --section the three sections run in three processes side by side (each alone takes ~2 min).
import fs from 'fs';
import path from 'path';
import { createRequire } from 'module';
import { fileURLToPath, pathToFileURL } from 'url';
import { loadPrototype } from './load_prototype.mjs';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const SECTIONS = ['labels', 'histories', 'maps'];
const sectionArg = process.argv.find(a => a.startsWith('--section='));
if (!sectionArg) {
  const { spawn } = await import('child_process');
  const t0 = performance.now();
  const runs = await Promise.all(SECTIONS.map(sec => new Promise(done => {
    const c = spawn(process.execPath, [fileURLToPath(import.meta.url), ...process.argv.slice(2), '--section=' + sec]);
    let out = '';
    c.stdout.on('data', d => { out += d; });
    c.stderr.on('data', d => { out += d; });
    c.on('close', code => done({ sec, code, out }));
  })));
  for (const r of runs) process.stdout.write(r.out);
  const bad = runs.filter(r => r.code !== 0).map(r => r.sec);
  console.log(bad.length ? `label parity: FAILED (${bad.join(', ')})`
    : `label parity: PASS (C++ core = JS reference; ${((performance.now() - t0) / 1000).toFixed(1)} s, 3 processes)`);
  process.exit(bad.length ? 1 : 0);
}
const SECTION = sectionArg.slice('--section='.length);
if (!SECTIONS.includes(SECTION)) throw new Error('--section: ' + SECTION);
const require = createRequire(import.meta.url);
const modulePath = process.argv.slice(2).find(a => !a.startsWith('--'));
const M = await require(modulePath || path.join(ROOT, 'build/wasm/core/insiliscope.js'))();
const read = f => fs.readFileSync(path.join(ROOT, f), 'utf8');
const P = loadPrototype(read('web/prototype/index.html'), read('web/prototype/microtubules.js'));
const { World } = await import(pathToFileURL(path.join(ROOT, 'web/prototype/scope/world.js')).href);
const D = await import(pathToFileURL(path.join(ROOT, 'web/prototype/scope/dyes.js')).href);
const { STRUCTURES, STRUCTURE_NPC } = await import(pathToFileURL(path.join(ROOT, 'web/prototype/scope/world.js')).href);
const { STRUCTURE_ORIGAMI } = await import(pathToFileURL(path.join(ROOT, 'web/prototype/scope/origami.js')).href);
const CM = await import(pathToFileURL(path.join(ROOT, 'web/prototype/scope/clockmap.js')).href);
if (M._isc_abi_version() !== 15) throw new Error('core ABI ' + M._isc_abi_version() + ', expected 15');
if (STRUCTURES.length !== 3 || STRUCTURE_NPC !== 1 || STRUCTURE_ORIGAMI !== 2) throw new Error('structure ids moved');

const seed = 1249, geom = P.paramsFrom(P.defaults);
const MODES = { dSTORM: 0, PALM: 1, 'DNA-PAINT': 2, WideField: 3 };
const ORIENT = { Free: 0, Fixed: 1, Random: 2 };
// isc_world_set_label's layout (core/include/insiliscope/insiliscope.h ISC_LABEL_*).
const labelVector = l => [l.density, l.fluorescentFraction, MODES[l.mode], l.kinetics.activationRatePerSec, l.kinetics.onSec,
  l.kinetics.offSec, l.kinetics.bleachProb, l.kinetics.photonCV, l.kinetics.initialOnSec, l.preState ? 1 : 0,
  ORIENT[l.orientation.mode], l.orientation.polarDeg, l.orientation.azimuthDeg, l.orientation.wobbleDeg, 0, 0,
  ...(l.linkage ? [l.linkage.binderNm, l.linkage.binderFlexNm, l.linkage.linkerMinNm, l.linkage.linkerMaxNm] : [NaN, NaN, NaN, NaN])];

let fails = 0;
const report = (ok, msg) => { console.log(`${ok ? 'ok  ' : 'FAIL'} ${msg}`); if (!ok) fails++; };

// A C world of `specimen` with `label` on structure s; the other structures keep the default label, or get `other`.
function cWorld(label, specimen = 0, s = 0, extra = {}, other = null) {
  const p = M._isc_params_new();
  for (const [k, v] of Object.entries({ specimen, ...extra })) {
    const n = M.lengthBytesUTF8(k) + 1, sp = M._malloc(n);
    M.stringToUTF8(k, sp, n);
    if (M._isc_params_set(p, sp, v) !== 0) throw new Error('isc_params_set ' + k);
    M._free(sp);
  }
  const w = M._isc_world_new(seed, p);
  M._isc_params_free(p);
  STRUCTURES.forEach((_, i) => {
    if (i !== s && !other) return;
    const v = labelVector(i === s ? label : other), buf = M._malloc(v.length * 8);
    M.HEAPF64.set(v, buf / 8);
    const r = M._isc_world_set_label(w, i, buf, v.length);
    M._free(buf);
    if (r !== 0) throw new Error('isc_world_set_label: ' + r);
  });
  return w;
}
// The ABI's cap/total convention, rows of `stride` doubles.
function rows(fn, stride) {
  const n = fn(0, 0);
  if (n < 0) throw new Error('query failed');
  const buf = M._malloc(Math.max(1, n) * stride * 8);
  const n2 = fn(buf, n);
  const out = [];
  for (let i = 0; i < n2; i++) out.push(Array.from(M.HEAPF64.subarray(buf / 8 + i * stride, buf / 8 + (i + 1) * stride)));
  M._free(buf);
  return out;
}
const evRow = e => [e.x, e.y, e.z, e.tOn, e.tOff, e.brightness, e.id, e.structure, e.state, e.aux];
function same(what, a, b) {
  let at = -1;
  if (a.length === b.length) {
    for (let i = 0; i < a.length && at < 0; i++) if (!a[i].every((v, j) => Object.is(v, b[i][j]))) at = i;
  }
  const ok = a.length === b.length && at < 0;
  report(ok, `${what}: ${a.length} C++ vs ${b.length} JS` + (ok ? '' : at >= 0 ? ` (first difference at ${at}: ${a[at]} vs ${b[at]})` : ''));
}

const win = [64, 4, 70, 10, -5, 50];
const kin = { activationRatePerSec: 0.05, onSec: 0.05, offSec: 1.0, bleachProb: 0.5, photonCV: 0.5 };
const CASES = [
  ['DNA-PAINT 70 %', { mode: 'DNA-PAINT', density: 0.7, kinetics: { ...kin, activationRatePerSec: 0.00143 } }],
  ['dSTORM 10 %, initial ON 2 s', { mode: 'dSTORM', density: 0.1, kinetics: { ...kin, initialOnSec: 2 } }],
  ['PALM 10 %, pre state, fraction 0.6', { mode: 'PALM', density: 0.1, fluorescentFraction: 0.6, preState: true, kinetics: kin }],
  ['WideField 30 %', { mode: 'WideField', density: 0.3, kinetics: kin }],
];
const jsLabels = (label, s, other = null) => STRUCTURES.map((_, i) => i === s ? label : other ?? D.makeLabel());
// The specimens: the cell field (the label on the microtubules, the pores keeping the default label, so both emit;
// then on the pores alone: the microtubules' 70 % DNA-PAINT would be ~20x their dyes) and the nanorulers (docking
// strands; their own params on top).
const OFF = D.makeLabel({ density: 0 });
const SPECIMENS = [
  { name: '', specimen: 0, s: 0, extra: {} },
  { name: 'pores ', specimen: 0, s: STRUCTURE_NPC, extra: {}, other: OFF },
  { name: 'pores (flexible linkage, Fixed, sparse) ', specimen: 0, s: STRUCTURE_NPC, other: OFF,
    extra: { npcDensity: 5, npcMinDistNm: 200, npcRadiusNm: 60 },
    label: { linkage: { binderNm: 10, binderFlexNm: 4, linkerMinNm: 1, linkerMaxNm: 6 },
             orientation: { mode: 'Fixed', polarDeg: 30, azimuthDeg: 40 } } },
  { name: 'microtubules (IgG-like linkage) ', specimen: 0, s: 0, extra: {},
    label: { linkage: { binderNm: 17.5, binderFlexNm: 5, linkerMinNm: 0, linkerMaxNm: 2 } } },
  { name: 'nanorulers ', specimen: 1, s: STRUCTURE_ORIGAMI, extra: {} },
  { name: 'nanorulers (Lp 273.1, 3 strands, 3D) ', specimen: 1, s: STRUCTURE_ORIGAMI,
    extra: { origamiLpNm: 273.1, origamiStrandsPerSpot: 3, origamiHeights: 1, origamiDensity: 2 } },
  { name: '2D origami (logo, 20 nm) ', specimen: 1, s: STRUCTURE_ORIGAMI, extra: { origamiDesign: 1, origamiSpacingNm: 20, origamiDensity: 2 } },
];
const tl0 = performance.now();
if (SECTION === 'labels') for (const sp of SPECIMENS) for (const [cname, opts] of CASES) {
  const name = sp.name + cname, label = D.makeLabel({ ...opts, ...(sp.label || {}) });
  const js = new World(P, seed, { ...geom, specimen: sp.specimen, ...sp.extra }, jsLabels(label, sp.s, sp.other));
  const w = cWorld(label, sp.specimen, sp.s, sp.extra, sp.other);
  same(`${name}: dyes`, rows((b, c) => M._isc_sites_in_window(w, ...win, b, c), 5),
    js.sitesInWindow(...win).map(d => [d.x, d.y, d.z, d.id, d.structure]));
  // A query history: late, long, early, frame-sized.
  for (const [t0, t1] of [[120, 120.05], [0, 200], [3, 3.05], [60, 62]]) {
    same(`${name}: blinks [${t0}, ${t1})`, rows((b, c) => M._isc_events_in_window(w, ...win, t0, t1, b, c), 10),
      js.eventsInWindow(...win, t0, t1).map(evRow));
  }
  for (const tMin of [-Infinity, 1.0]) {
    same(`${name}: continuous windows ending after ${tMin}`, rows((b, c) => M._isc_continuous_in_window(w, ...win, tMin, b, c), 10),
      js.continuousInWindow(...win, tMin).map(evRow));
  }
  M._isc_world_free(w);
}
// ABI 11: kinetics histories. Each case: segments [tStart, kinetics] (the label's mode, density, pre state stay).
const KIN_KEYS = ['activationRatePerSec', 'onSec', 'offSec', 'bleachProb', 'photonCV', 'initialOnSec'];
function setHistory(w, js, segs) {
  // The same kinetics for every structure (a row is tStart + 6 per structure).
  const tStart = segs.map(x => x[0]), kins = segs.map(x => STRUCTURES.map(() => ({ initialOnSec: 0, ...x[1] })));
  js.setKineticsHistory(tStart, kins);
  const v = segs.flatMap((x, i) => [x[0], ...kins[i].flatMap(q => KIN_KEYS.map(k => q[k]))]);
  const buf = M._malloc(Math.max(1, v.length) * 8);
  M.HEAPF64.set(v, buf / 8);
  const r = M._isc_world_set_kinetics_history(w, buf, segs.length);
  M._free(buf);
  if (r !== 0) throw new Error('isc_world_set_kinetics_history: ' + r);
}
const HCASES = [
  ['dSTORM initial ON 2 s, 405 x10 at 30 s, off at 90 s', { mode: 'dSTORM', density: 0.1, kinetics: { ...kin, initialOnSec: 2 } },
    [[[0, { ...kin, initialOnSec: 2 }], [1.5, { ...kin, initialOnSec: 0.5 }], [30, { ...kin, activationRatePerSec: 0.5, initialOnSec: 0.5 }],
      [90, { ...kin, activationRatePerSec: 0, initialOnSec: 0.5 }]],
     [[0, { ...kin, initialOnSec: 2 }], [30, { ...kin, onSec: 0.005, offSec: 0.1 }]]]],
  ['PALM pre state, UV off at 50 s, high at 100 s', { mode: 'PALM', density: 0.1, fluorescentFraction: 0.6, preState: true, kinetics: kin },
    [[[0, kin], [50, { ...kin, activationRatePerSec: 0 }], [100, { ...kin, activationRatePerSec: 0.5 }]],
     [[0, { ...kin, activationRatePerSec: 0.2 }]]]],
  ['DNA-PAINT, imager x10 mid bin', { mode: 'DNA-PAINT', density: 0.7, kinetics: { ...kin, activationRatePerSec: 0.00143 } },
    [[[0, { ...kin, activationRatePerSec: 0.00143 }], [60.4, { ...kin, activationRatePerSec: 0.0143, onSec: 0.2 }]],
     [[0, { ...kin, activationRatePerSec: 0.00143 }], [10, { ...kin, activationRatePerSec: 0 }]]]],
  ['WideField, rates step', { mode: 'WideField', density: 0.3, kinetics: kin },
    [[[0, kin], [40, { ...kin, onSec: 0.5 }]], [[0, kin]]]],
];
if (SECTION === 'labels') console.log(`labels: ${((performance.now() - tl0) / 1000).toFixed(1)} s`);
const th0 = performance.now();
if (SECTION === 'histories') for (const sp of [SPECIMENS[0], SPECIMENS[1], SPECIMENS[4]]) for (const [cname, opts, hists] of HCASES) {
  const name = sp.name + cname, label = D.makeLabel(opts);
  const js = new World(P, seed, { ...geom, specimen: sp.specimen, ...sp.extra }, jsLabels(label, sp.s, sp.other));
  const w = cWorld(label, sp.specimen, sp.s, sp.extra, sp.other);
  // History A, B, A again on the same worlds (the C++ keeps a schedule per history).
  for (const [hi, h] of [[0, hists[0]], [1, hists[1]], [0, hists[0]]]) {
    setHistory(w, js, h);
    for (const [t0, t1] of [[120, 120.05], [0, 200], [29.9, 30.2], [60, 62]]) {
      same(`${name} (history ${'AB'[hi]}): blinks [${t0}, ${t1})`, rows((b, c) => M._isc_events_in_window(w, ...win, t0, t1, b, c), 10),
        js.eventsInWindow(...win, t0, t1).map(evRow));
    }
    same(`${name} (history ${'AB'[hi]}): continuous windows`, rows((b, c) => M._isc_continuous_in_window(w, ...win, -Infinity, b, c), 10),
      js.continuousInWindow(...win, -Infinity).map(evRow));
  }
  M._isc_world_free(w);
}
// ABI 14: clock maps. Two grids (one ending inside the window: the dyes past x = 68.75 are unlit), a depth table; per
// case a clock for every structure, then the present moved on (tOpen + 10, the past rate folded into the total, the
// beam 0.3 um over; an activation clock gets a step), then the map removed.
const wavy = (nx, ny, f) => Array.from({ length: nx * ny }, (_, k) => f(k % nx, Math.floor(k / nx)));
const GRIDS = [
  { x0: 63, y0: 3, h: 0.25, nx: 23, ny: 30, v: wavy(23, 30, (i, j) => 0.6 + 0.4 * Math.sin(0.7 * i + 0.3 * j)) },
  { x0: 60, y0: 0, h: 0.5, nx: 30, ny: 30, v: wavy(30, 30, (i, j) => 1 + 0.5 * Math.cos(0.4 * i - 0.5 * j)) },
];
const DEPTHS = [{ z0: 0, dz: 0.5, v: [1, 0.7, 0.4, 0.2] }];
const hist = segs => ({ tStart: segs.map(x => x[0]), kin: segs.map(x => ({ initialOnSec: 0, ...x[1] })) });
const RATE = [{ grid: 0, depth: 0, sx: 0.3, zp: 0.2, scale: 1 }];
const dose = (segs, bound = []) => ({ mode: CM.CLOCK_DOSE, hist: hist(segs), bound, total: [{ grid: 1, scale: 50 }], totalC: 0,
  rate: RATE, rateC: 0, tOpen: 50 });
const activation = segs => ({ mode: CM.CLOCK_ACTIVATION, hist: hist(segs), total: [], rate: [{ grid: 0, depth: 0, scale: 0.02 }],
  rateC: 0, tOpen: 30, steps: [{ t0: 0, t1: 10, c: 0, field: [{ grid: 1, scale: 0.05 }] }, { t0: 10, t1: 25, c: 0.01, field: [] },
    { t0: 25, t1: 30, c: 0, field: [{ grid: 0, scale: 0.3 }] }] });
const global = segs => ({ mode: CM.CLOCK_GLOBAL, hist: hist(segs), total: [], rate: [], tOpen: 50 });
// The present 10 s on: a dose clock's last 10 s of rate folded into its total, the beam moved; an activation clock's
// open rate becomes a step.
function moved(c) {
  const n = structuredClone(c);
  if (c.mode === CM.CLOCK_DOSE) {
    n.total = [...c.total, ...c.rate.map(t => ({ ...t, scale: t.scale * 10 }))];
    n.totalC = c.totalC + 10 * c.rateC;
    n.rate = c.rate.map(t => ({ ...t, ox: 0.3 }));
  } else if (c.mode === CM.CLOCK_ACTIVATION) {
    n.steps = [...c.steps, { t0: c.tOpen, t1: c.tOpen + 10, c: 10 * c.rateC, field: c.rate.map(t => ({ ...t, scale: t.scale * 10 })) }];
    n.rate = c.rate.map(t => ({ ...t, ox: 0.3 }));
  }
  if (c.bleachRate) n.bleachTotal = [...c.bleachTotal, ...c.bleachRate.map(t => ({ ...t, scale: t.scale * 10 }))];
  n.tOpen = c.tOpen + 10;
  return n;
}
// A photon-budget bleach (the hazard by tOpen, then per second): a budgeted window ends where it reaches its draw.
const bleach = c => ({ ...c, bleachTotal: [{ grid: 1, scale: 0.6 }], bleachRate: [{ grid: 0, depth: 0, sx: 0.3, zp: 0.2, scale: 0.05 }] });
const palmKin = kin, paintKin = { ...kin, activationRatePerSec: 0.00143 };
if (SECTION === 'histories') console.log(`kinetics histories: ${((performance.now() - th0) / 1000).toFixed(1)} s`);
const MCASES = [
  ['dSTORM initial ON 2 s, dose (shear, depth, unlit band)', { mode: 'dSTORM', density: 0.05, kinetics: { ...kin, initialOnSec: 2 } },
    dose([[0, { ...kin, initialOnSec: 2 }]])],
  ['dSTORM, dose with three epochs (one giving way)', { mode: 'dSTORM', density: 0.05, kinetics: { ...kin, initialOnSec: 2 } },
    dose([[0, { ...kin, initialOnSec: 2 }], [20, { ...kin, activationRatePerSec: 0.5, initialOnSec: 2 }],
      [40, { ...kin, activationRatePerSec: 0.01, onSec: 0.02, initialOnSec: 2 }]],
    [[{ grid: 1, scale: 20 }], [{ grid: 0, scale: 45 }]])],
  ['PALM pre state, activation steps then the open rate', { mode: 'PALM', density: 0.05, fluorescentFraction: 0.6, preState: true, kinetics: palmKin },
    activation([[0, palmKin], [60, { ...palmKin, onSec: 0.1 }]])],
  ['PALM pre state, dose', { mode: 'PALM', density: 0.05, fluorescentFraction: 0.6, preState: true, kinetics: palmKin },
    dose([[0, palmKin]])],
  ['PALM pre state with a bleach, activation', { mode: 'PALM', density: 0.05, fluorescentFraction: 0.6, preState: true, kinetics: palmKin },
    bleach(activation([[0, palmKin]]))],
  ['WideField, dose (unlit band)', { mode: 'WideField', density: 0.1, kinetics: kin }, dose([[0, kin]])],
  ['WideField with a bleach, dose', { mode: 'WideField', density: 0.1, kinetics: kin }, bleach(dose([[0, kin]]))],
  ['DNA-PAINT under a dose clock (on T, two epochs)', { mode: 'DNA-PAINT', density: 0.2, kinetics: paintKin },
    dose([[0, paintKin], [60.4, { ...paintKin, activationRatePerSec: 0.0143, onSec: 0.2 }]], [[{ grid: 1, scale: 20 }]])],
  ['dSTORM under an activation clock (on T)', { mode: 'dSTORM', density: 0.05, kinetics: kin },
    { ...activation([[0, kin], [30, { ...kin, activationRatePerSec: 0.2 }]]) }],
  ['dSTORM, global clock with two segments', { mode: 'dSTORM', density: 0.05, kinetics: kin },
    global([[0, kin], [30, { ...kin, activationRatePerSec: 0.2 }]])],
];
function setMap(w, js, m) {
  const v = m ? CM.encodeClockMap(m) : [];
  js.setClockMap(v);
  const buf = M._malloc(Math.max(1, v.length) * 8);
  M.HEAPF64.set(v, buf / 8);
  const r = M._isc_world_set_clock_map(w, buf, v.length);
  M._free(buf);
  if (r !== 0) throw new Error('isc_world_set_clock_map: ' + r);
}
const mapRow = e => [...evRow(e), e.clock, e.rate];
const MFRAMES = [[50, 50.05], [52, 54], [120, 120.05], [0, 200], [30, 31], [3, 3.05], [60, 62], [61, 61.05]];
const tm0 = performance.now();
if (SECTION === 'maps') for (const sp of [SPECIMENS[0], SPECIMENS[1], SPECIMENS[4]]) for (const [ci, [cname, opts, clock]] of MCASES.entries()) {
  if (clock.bleachRate && sp !== SPECIMENS[0]) continue;   // (the bleach on the first specimen: the time budget)
  const name = sp.name + cname, label = D.makeLabel(opts);
  const js = new World(P, seed, { ...geom, specimen: sp.specimen, ...sp.extra }, jsLabels(label, sp.s, sp.other));
  const w = cWorld(label, sp.specimen, sp.s, sp.extra, sp.other);
  // (the map removed: once per specimen)
  for (const [mi, c] of [[0, clock], [1, moved(clock)], ...(ci === 0 ? [[2, null]] : [])]) {
    setMap(w, js, c && { grids: GRIDS, depths: DEPTHS, s: STRUCTURES.map(() => c) });
    const tag = ['map', 'moved map', 'no map'][mi];
    // (a bleach changes the windows only; the whole 200 s, ~100x a frame's blinks, on the first specimen)
    for (const [t0, t1] of clock.bleachRate ? [] : MFRAMES.filter(f => sp === SPECIMENS[0] || f[1] - f[0] < 100)) {
      same(`${name} (${tag}): blinks [${t0}, ${t1})`, rows((b, n) => M._isc_events_in_map(w, ...win, t0, t1, b, n), 10),
        js.eventsInMap(...win, t0, t1).map(evRow));
    }
    for (const tMin of [-Infinity, 1.0, 55]) {
      same(`${name} (${tag}): continuous windows ending after ${tMin}`,
        rows((b, n) => M._isc_continuous_in_map(w, ...win, tMin, b, n), 12), js.continuousInMap(...win, tMin).map(mapRow));
    }
  }
  M._isc_world_free(w);
}
if (SECTION === 'maps') console.log(`clock maps: ${((performance.now() - tm0) / 1000).toFixed(1)} s`);
console.log(fails ? `${SECTION}: ${fails} FAILED` : `${SECTION}: PASS`);
process.exit(fails ? 1 : 0);
