#!/usr/bin/env node
// Checks dist/cellfield_block.js (tools/make_cellfield_block.mjs) the way webSMLM calls it:
// CellField.buildWindow(w, h, opts) with webSMLM's default PARAMS. Runs in Node with a fake
// worker global (the module is built for web/worker only).
//
//   node tests/block/check_cellfield_block.mjs [block.js]
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const file = process.argv[2] || path.join(ROOT, 'dist/cellfield_block.js');
globalThis.WorkerGlobalScope = function () {};
globalThis.self = globalThis;
globalThis.location = { href: 'file:///cellfield_block.js' };

const text = fs.readFileSync(file, 'utf8');
if (!text.startsWith('// ==== BEGIN insiliscope CellField block ====') || !text.trimEnd().endsWith('// ==== END insiliscope CellField block ===='))
  throw new Error('block markers missing');
const CellField = new Function(text + '\nreturn CellField;')();

let fails = 0;
const ok = (cond, msg) => { console.log(`${cond ? 'ok  ' : 'FAIL'} ${msg}`); if (!cond) fails++; };

// webSMLM defaults: simulation_mt_* PARAMS, 100 nm px, zRange 500 nm.
const base = { seed: 1249, xUm: 0, yUm: 0, pxnm: 100, mtDensity: 0.9, cellDensity: 0.33, focusUm: 0.25, slabNm: 500 };
const W = 256, H = 256;
const t0 = performance.now();
const a = CellField.buildWindow(W, H, base);
const t1 = performance.now();
const again = CellField.buildWindow(W, H, base);
const t2 = performance.now();
console.log(`window 25.6 x 25.6 um: ${a.sites.length} sites, ${a.nCells} cells, ${a.nMt} MTs; ` +
            `first ${(t1 - t0).toFixed(0)} ms, cached ${(t2 - t1).toFixed(0)} ms`);
ok(CellField.abiVersion >= 3, `ABI ${CellField.abiVersion}`);
ok(a.sites.length > 1000 && a.nCells > 0 && a.nMt > 0, 'sites, cells and microtubules in the default window');
ok(a.sites.every(s => s.length === 3 && s[0] >= 0 && s[0] < W && s[1] >= 0 && s[1] < H && Math.abs(s[2]) <= 500),
   'every site inside the canvas and the +-slab');
ok(a.removed === null && a.packed === true, 'removed = null, packed = true');

const key = s => s.map(v => v.toFixed(6)).join(',');
const same = (p, q) => p.length === q.length && p.every((s, i) => key(s) === key(q[i]));
ok(same(a.sites, again.sites), 'same call twice: identical');
CellField.buildWindow(W, H, { ...base, xUm: 1000, yUm: -700 });
CellField.buildWindow(W, H, { ...base, seed: 7 });
ok(same(a.sites, CellField.buildWindow(W, H, base).sites), 'identical after a 1.2 mm excursion and a seed change');
CellField.dispose();
ok(same(a.sites, CellField.buildWindow(W, H, base).sites), 'identical after dispose()');

// Tiling: the two halves of the window give exactly the full window's sites (in world um).
const world = (r, o, w, h) => r.sites.map(s => [(s[0] - w / 2) * o.pxnm / 1000 + o.xUm, (s[1] - h / 2) * o.pxnm / 1000 + o.yUm, s[2]]);
const setOf = pts => new Set(pts.map(p => p.map(v => v.toFixed(6)).join(',')));
const L = { ...base, xUm: -6.4 }, R = { ...base, xUm: 6.4 };
const halves = setOf([...world(CellField.buildWindow(W / 2, H, L), L, W / 2, H), ...world(CellField.buildWindow(W / 2, H, R), R, W / 2, H)]);
const full = setOf(world(a, base, W, H));
ok(halves.size === full.size && [...full].every(k => halves.has(k)), `two half windows = the full window (${halves.size} sites)`);

// Parameters reach the world: fewer microtubules, fewer sites.
const sparse = CellField.buildWindow(W, H, { ...base, mtDensity: 0.1 });
ok(sparse.sites.length < a.sites.length / 2, `mtDensity 0.1: ${sparse.sites.length} sites (< half)`);
const thin = CellField.buildWindow(W, H, { ...base, slabNm: 100 });
ok(thin.sites.length < a.sites.length && thin.sites.every(s => Math.abs(s[2]) <= 100), `slab 100 nm: ${thin.sites.length} sites`);

// Worker source: an independent CellField with the same answer.
const CF2 = new Function('return ' + CellField.workerSource())();
ok(same(a.sites, CF2.buildWindow(W, H, base).sites), 'workerSource() CellField gives the same sites');

// Sample drift: the JS reference's path (= the C++'s, tests/parity/scope_parity.mjs), also from a worker's CellField.
const { driftTrajectory } = await import('../../web/prototype/scope/drift.js');
const dt = CellField.driftTrajectory(42, 200, 0.05, 20, 30);
const ref = driftTrajectory(42, 200, 0.05, { xyNmPerSqrtSec: 20, zNmPerSqrtSec: 30 });
ok(dt.length === 200 && dt.every((d, i) => d.x === ref[i].x && d.y === ref[i].y && d.z === ref[i].z) &&
   dt[0].x === 0 && Math.abs(dt[199].x) > 0, `driftTrajectory = the JS reference (frame 199: ${dt[199].x.toFixed(1)}, ${dt[199].y.toFixed(1)}, ${dt[199].z.toFixed(1)} nm)`);
const opts = { xySpeedNmPerSec: 15, zSpeedNmPerSec: 4, zDirection: -1, angleWanderDeg: 20, zAngleWanderDeg: 120, speedWanderPct: 30, wanderTimeSec: 5 };
const dd = CellField.driftTrajectory(42, 200, 0.05, 20, 30, opts);
const rd = driftTrajectory(42, 200, 0.05, { xyNmPerSqrtSec: 20, zNmPerSqrtSec: 30, ...opts });
ok(dd.every((d, i) => d.x === rd[i].x && d.y === rd[i].y && d.z === rd[i].z) && dd[199].x !== dt[199].x,
   `driftTrajectory with a directed part = the JS reference (frame 199: ${dd[199].x.toFixed(1)}, ${dd[199].y.toFixed(1)}, ${dd[199].z.toFixed(1)} nm)`);
const dw = CF2.driftTrajectory(42, 200, 0.05, 20, 30);
ok(dw.every((d, i) => d.x === dt[i].x && d.z === dt[i].z), 'workerSource() CellField: the same drift');

if (fails) { console.error(`${fails} check(s) failed`); process.exit(1); }
console.log('all checks passed');
