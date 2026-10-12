// Label-model regression (issues 16 and 15): the working tree's world against the base's, and the two targets.
//   node web/lab/label_regression.mjs [--base origin/main]
// 1. The base's scope/dyes.js + world.js (git show) against the working tree's, same prototype geometry: every
//    microtubule label mode gives the same sites, events and continuous windows (positions, times, brightness, id,
//    order) with the nuclear pores off -- and the microtubules' share of a world with the pores on is the same too
//    (the targets do not touch each other's draws).
// 2. The label model's statistics: fluorescent-fraction nesting, dSTORM initial ON, WideField windows, the PALM pre
//    state, orientations; the pores' labelled fraction and nesting, their Fixed orientation about the pore axis.
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { execFileSync } from 'node:child_process';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { loadPrototype } from '../../tests/parity/load_prototype.mjs';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const arg = (k, d) => { const i = process.argv.indexOf(k); return i > 0 ? process.argv[i + 1] : d; };
const base = arg('--base', 'origin/main');
const read = f => fs.readFileSync(path.join(ROOT, f), 'utf8');
const P = loadPrototype(read('web/prototype/index.html'), read('web/prototype/microtubules.js'));

// The base's world modules into a temp dir (they import each other relatively).
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'isc-label-'));
for (const f of ['rng.js', 'dyes.js', 'world.js', 'dye_library_data.js'])
  fs.writeFileSync(path.join(tmp, f), execFileSync('git', ['show', `${base}:web/prototype/scope/${f}`], { cwd: ROOT, maxBuffer: 1 << 26 }));
const Old = await import(pathToFileURL(path.join(tmp, 'world.js')).href);
const OldD = await import(pathToFileURL(path.join(tmp, 'dyes.js')).href);
const New = await import(pathToFileURL(path.join(ROOT, 'web/prototype/scope/world.js')).href);
const D = await import(pathToFileURL(path.join(ROOT, 'web/prototype/scope/dyes.js')).href);
const MT = New.STRUCTURE_MT, NPC = New.STRUCTURE_NPC;

const seed = 1249, geom = P.paramsFrom(P.defaults);
const win = [64, 4, 68, 8, -5, 50];
const kin = { activationRatePerSec: 0.05, onSec: 0.05, offSec: 1.0, bleachProb: 0.5, photonCV: 0.5 };
let fails = 0;
const report = (ok, msg) => { console.log(`${ok ? 'ok  ' : 'FAIL'} ${msg}`); if (!ok) fails++; };
const strip = e => [e.x, e.y, e.z, e.tOn, e.tOff, e.brightness, e.id, e.state, e.aux];
const sameList = (a, b) => a.length === b.length && a.every((e, i) => strip(e).every((v, j) => Object.is(v, strip(b[i])[j])));
const OFF = D.makeLabel({ density: 0 });
// A working-tree world: the microtubule label, the pores' (default: off).
const newWorld = (label, npc = OFF) => {
  const ls = New.STRUCTURES.map(() => OFF);
  ls[MT] = D.makeLabel({ kinetics: kin, ...label });
  if (NPC >= 0) ls[NPC] = npc;
  return new New.World(P, seed, geom, ls);
};
const oldWorld = label => new Old.World(P, seed, geom, Old.STRUCTURES.map((s, i) => i === 0 ? OldD.makeLabel({ kinetics: kin, ...label }) : OldD.makeLabel({ density: 0 })));

// ---- 1. the base's microtubules ----
for (const [mode, extra, t0, t1] of [['PALM', { density: 0.1, preState: true }, 0, 60], ['DNA-PAINT', { density: 0.7 }, 10, 12],
  ['dSTORM', { density: 0.1, kinetics: { ...kin, initialOnSec: 2 } }, 0, 30], ['WideField', { density: 0.1 }, 0, 1]]) {
  const label = { mode, ...extra };
  const o = oldWorld(label), n = newWorld(label);
  const sites = (w) => w.sitesInWindow(...win);
  const so = sites(o), sn = sites(n);
  const okSites = so.length === sn.length && so.every((d, i) => d.id === sn[i].id && d.x === sn[i].x && d.y === sn[i].y && d.z === sn[i].z);
  const okEv = sameList(o.eventsInWindow(...win, t0, t1), n.eventsInWindow(...win, t0, t1));
  const okCont = sameList(o.continuousInWindow(...win), n.continuousInWindow(...win));
  report(okSites && okEv && okCont, `${mode}: microtubule sites (${sn.length}), events and continuous windows == ${base}`);
  if (NPC >= 0) {
    // With the pores labelled too: the microtubules' sites and events are unchanged.
    const both = newWorld(label, D.makeLabel({ kinetics: kin, ...label, density: 0.6 }));
    const mtOnly = l => l.filter(e => e.structure === MT);
    const okB = sameList(mtOnly(both.eventsInWindow(...win, t0, t1)), n.eventsInWindow(...win, t0, t1)) &&
      sameList(mtOnly(both.continuousInWindow(...win)), n.continuousInWindow(...win));
    report(okB, `${mode}: the microtubules unchanged by labelled pores`);
  }
}

// ---- 2. statistics ----
// Nesting: a fluorescent fraction keeps a subset of the same dyes.
{
  const all = newWorld({ density: 0.7 }).sitesInWindow(...win), half = newWorld({ density: 0.7, fluorescentFraction: 0.5 }).sitesInWindow(...win);
  const ids = new Set(all.map(d => d.id)), frac = half.length / all.length;
  report(half.every(d => ids.has(d.id)) && Math.abs(frac - 0.5) < 0.02, `fluorescent fraction 0.5: ${half.length}/${all.length} = ${frac.toFixed(3)}, a subset`);
}
// dSTORM: initial ON windows Exp(initialOnSec), blinks after; WideField: one always-on window per dye.
{
  const w = newWorld({ mode: 'dSTORM', density: 0.1, kinetics: { ...kin, initialOnSec: 2 } });
  const c = w.continuousInWindow(...win), m = c.reduce((s, e) => s + e.tOff, 0) / c.length;
  const n = w.sitesInWindow(...win).length, ev = w.eventsInWindow(...win, 0, 1e9);
  report(c.length === n && Math.abs(m - 2) < 0.1 && c.every(e => e.state === D.EVENT_STATE.INITIAL_ON),
    `dSTORM initial ON: ${c.length} windows for ${n} dyes, mean ${m.toFixed(3)} s (2), ${ev.length} blinks`);
  const wf = newWorld({ mode: 'WideField', density: 0.1 }).continuousInWindow(...win);
  const ma = wf.reduce((s, e) => s + e.aux, 0) / wf.length;
  report(wf.length === n && wf.every(e => e.tOff === Infinity && e.state === D.EVENT_STATE.ALWAYS_ON) && Math.abs(ma - 1) < 0.05,
    `WideField: ${wf.length} always-on windows, mean aux ${ma.toFixed(3)} (1)`);
  const pw = newWorld({ mode: 'PALM', density: 0.1, preState: true });
  // Sites and continuous windows come in the same dye order (ids collide: compare by address).
  const pre = pw.continuousInWindow(...win), ps = pw.sitesInWindow(...win), lab = pw.labels[MT];
  report(pre.length === n && ps.length === n && pre.every((e, i) => {
    const d = ps[i], bl = D.dyeSchedule(D.dyeH1(seed, d.cx, d.cy, d.mtIndex), d.k, d.n, lab.kinetics);
    return e.state === D.EVENT_STATE.PRE && e.x === d.x && e.tOff === (bl.length ? bl[0].tOn : Infinity);
  }), `PALM pre state ends at the first blink: ${pre.length} windows`);
}
// Orientation: Free = no dipole; Random isotropic (<z^2> = 1/3); Fixed: unit vectors.
{
  const sites = w0 => w0.sitesInWindow(...win).slice(0, 4000);
  const fr = newWorld({ density: 0.7 });
  const ok0 = sites(fr).every(d => fr.dyeOrientation(d) === null);
  const rw = newWorld({ density: 0.7, orientation: { mode: 'Random', wobbleDeg: 20 } });
  const zs = sites(rw).map(d => rw.dyeOrientation(d)), z2 = zs.reduce((s, o) => s + o.z * o.z, 0) / zs.length;
  const mx = zs.reduce((s, o) => s + o.x, 0) / zs.length;
  const fw = newWorld({ density: 0.7, orientation: { mode: 'Fixed', polarDeg: 90, azimuthDeg: 0 } });
  const fo = sites(fw).map(d => fw.dyeOrientation(d)), norms = fo.every(o => Math.abs(Math.hypot(o.x, o.y, o.z) - 1) < 1e-12);
  report(ok0 && Math.abs(z2 - 1 / 3) < 0.02 && Math.abs(mx) < 0.03 && norms && zs.every(o => o.wobbleDeg === 20),
    `orientation: Free null, Random <z^2> ${z2.toFixed(3)} (1/3) <x> ${mx.toFixed(3)}, Fixed unit vectors`);
}
// Nuclear pores: over one cell's nucleus, the labelled fraction of the 64 x pores sites, nesting, the block walk
// (every dye once), Fixed orientation perpendicular to the pore axis at polar 90 deg.
if (NPC >= 0) {
  const npcWorld = (o = {}) => newWorld({ density: 0 }, D.makeLabel({ kinetics: kin, mode: 'DNA-PAINT', density: 0.6, ...o }));
  const w = npcWorld();
  const c = w.cellsInRect(...win.slice(0, 4))[0], A = w.cellAssets(c), pores = w.npcs(A).pores;
  const pts = pores.map(q => New.localToWorld(c, q.x, q.y));
  const pad = 0.2, xs = pts.map(p => p[0]), ys = pts.map(p => p[1]), zsP = pores.map(q => q.z);
  const box = [Math.min(...xs) - pad, Math.min(...ys) - pad, Math.max(...xs) + pad, Math.max(...ys) + pad, Math.min(...zsP) - pad, Math.max(...zsP) + pad];
  const own = l => l.filter(d => d.cx === c.cx && d.cy === c.cy && d.structure === NPC);
  const s6 = own(w.sitesInWindow(...box)), frac = s6.length / (64 * pores.length);
  const keys = new Set(s6.map(d => d.k + ',' + d.n));
  report(Math.abs(frac - 0.6) < 0.01 && keys.size === s6.length,
    `pores: ${s6.length} dyes on ${pores.length} pores = ${frac.toFixed(4)} of the sites (0.6), each site once`);
  const s3 = own(npcWorld({ density: 0.3 }).sitesInWindow(...box)), ids6 = new Set(s6.map(d => d.k + ',' + d.n + ',' + d.id));
  report(s3.every(d => ids6.has(d.k + ',' + d.n + ',' + d.id)) && Math.abs(s3.length / s6.length - 0.5) < 0.02,
    `pores: density 0.3 keeps a subset (${s3.length} of ${s6.length})`);
  const fw = npcWorld({ orientation: { mode: 'Fixed', polarDeg: 90, azimuthDeg: 0 } });
  const byCand = new Map(pores.map(q => [q.cand, q]));
  const fo = own(fw.sitesInWindow(...box)).slice(0, 3000);
  const worst = fo.reduce((m, d) => { const o = fw.dyeOrientation(d), q = byCand.get(d.n); return Math.max(m, Math.abs(o.x * q.n[0] + o.y * q.n[1] + o.z * q.n[2])); }, 0);
  report(worst < 1e-12, `pores: Fixed polar 90 deg dipoles perpendicular to the pore axis (max |cos| ${worst.toExponential(1)})`);
}
for (const [what, l] of [['SPT motion', { motion: 'Diffusive' }], ['off-target', { offTarget: [{ structure: 1, fraction: 0.05 }] }]]) {
  let threw = false;
  try { D.makeLabel(l); } catch { threw = true; }
  report(threw, `${what} rejected (not implemented yet)`);
}
fs.rmSync(tmp, { recursive: true, force: true });
console.log(fails ? `label regression: ${fails} FAILED` : 'label regression: PASS');
process.exit(fails ? 1 : 0);
