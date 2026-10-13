// Label-model regression (issues 16 and 15): the working tree's world against the base's, and the two targets.
//   node web/lab/label_regression.mjs [--base origin/main]
// 1. The base's scope/dyes.js + world.js (git show) against the working tree's, same prototype geometry: every
//    microtubule label mode gives the same sites, events and continuous windows (positions, times, brightness, id,
//    order) with the nuclear pores off -- and the microtubules' share of a world with the pores on is the same too
//    (the targets do not touch each other's draws).
// 2. The label model's statistics: fluorescent-fraction nesting, dSTORM initial ON, WideField windows, the PALM pre
//    state, orientations; the pores' labelled fraction and nesting, their Fixed orientation about the pore axis.
// 3. The label linkage (dyes.js linkedDye): a preset's dye distances from the microtubule axis and from the pore
//    sites, the three parts (binder, its flexibility, linker), the same dyes (ids) whatever the linkage.
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
// ---- 3. linkage ----
{
  const w = newWorld({ density: 0.7 });
  const c = w.cellsInRect(...win.slice(0, 4))[0], A = w.cellAssets(c);
  const i = A.mts.findIndex((m, j) => m.length > 1 && w.frames(A, j).length > 3);
  const pts = A.mts[i], fr = w.frames(A, i);
  // Per dye: the distance from the axis (nm, perpendicular to the tangent).
  const mtDyes = lk => {
    const raw = [];
    for (let b = 0; b < 3; b++) D.dyesInBlock(seed, c.cx, c.cy, i, pts, fr, b, 0.7, 1, raw, D.normLinkage(lk));
    return raw.map(d => {
      const S = (D.mtProtofilamentOffsetNm(d.k) + d.n * D.MT_DIMER_NM) * 1e-3, seg = D.mtSegmentAt(fr, S);
      const p = D.pointAtArc(pts, fr, S), T = fr.T[seg];
      const dx = d.pos.x - p.x, dy = d.pos.y - p.y, dz = d.pos.z - p.z, along = dx * T.x + dy * T.y + dz * T.z;
      return { id: d.id, r: Math.sqrt(Math.max(0, dx * dx + dy * dy + dz * dz - along * along)) * 1000 };
    });
  };
  const presets = (await import(pathToFileURL(path.join(ROOT, 'web/prototype/scope/dye_library_data.js')).href)).DYE_DATA.linkagePresets;
  const preset = id => presets.find(x => x.id === id);
  const legacy = mtDyes(D.LEGACY_LINKAGE.microtubules), igg = mtDyes(preset('IgG2-STORM'));
  const mean = a => a.reduce((s, x) => s + x, 0) / a.length;
  const rI = igg.map(d => d.r), rL = legacy.map(d => d.r);
  const lo = 12.5 + 10 - 12, hi = 12.5 + 10 + 12;
  report(legacy.length === igg.length && legacy.every((d, j) => d.id === igg[j].id),
    `linkage: the same ${igg.length} dyes (ids) under Legacy and IgG2-STORM`);
  report(Math.min(...rL) >= 12.5 + 12 - 5 - 1e-9 && Math.max(...rL) <= Math.hypot(12.5 + 12 + 5, 0) + 1e-9 &&
    Math.min(...rI) >= lo - 0.5 - 1e-9 && Math.max(...rI) <= hi + 0.5 + 1e-9 && Math.abs(mean(rI) - 22.5) < 1.5,
    `linkage: microtubule dye radius Legacy ${mean(rL).toFixed(2)} nm, IgG2-STORM ${mean(rI).toFixed(2)} nm (shell 22.5, Fruh 2021) in [${Math.min(...rI).toFixed(1)}, ${Math.max(...rI).toFixed(1)}]`);
  // Flexibility 6 nm, no linker: the binder's end uniform in a ball about its rigid end (mean distance 3/4 of the
  // radius, none beyond it), the linker [2, 2]: every dye exactly 2 nm from the binder's end.
  const fx = [];
  for (let b = 0; b < 3; b++) D.dyesInBlock(seed, c.cx, c.cy, i, pts, fr, b, 0.7, 1, fx, D.normLinkage({ binderNm: 10, binderFlexNm: 6, linkerMinNm: 2, linkerMaxNm: 2 }), true);
  const flexD = fx.map(d => Math.hypot(d.parts.tip.x - d.parts.rigid.x, d.parts.tip.y - d.parts.rigid.y, d.parts.tip.z - d.parts.rigid.z) * 1000);
  const linkD = fx.map(d => Math.hypot(d.pos.x - d.parts.tip.x, d.pos.y - d.parts.tip.y, d.pos.z - d.parts.tip.z) * 1000);
  const rigidD = fx.map(d => Math.hypot(d.parts.rigid.x - d.parts.site.x, d.parts.rigid.y - d.parts.site.y, d.parts.rigid.z - d.parts.site.z) * 1000);
  report(Math.abs(mean(flexD) - 4.5) < 0.1 && Math.max(...flexD) <= 6 + 1e-9 && linkD.every(x => Math.abs(x - 2) < 1e-9) &&
    rigidD.every(x => Math.abs(x - 10) < 1e-9) && fx.every((d, j) => d.id === legacy[j].id),
    `linkage parts: binder 10 nm exactly, flexibility mean ${mean(flexD).toFixed(3)} nm (4.5) max ${Math.max(...flexD).toFixed(2)} (<= 6), linker 2 nm exactly, same dyes`);
  // Pores: the dye's offset from its site along the corner's radial direction averages the binder (the linker ball is
  // symmetric), its distance <= binder + linker.
  if (NPC >= 0) {
    const pw = newWorld({ density: 0 }, D.makeLabel({ kinetics: kin, density: 0.6 }));
    const pc = pw.cellsInRect(...win.slice(0, 4))[0], N = pw.npcs(pw.cellAssets(pc));
    const ids = N.pores.map((_, j) => j).slice(0, 200), pl = D.normLinkage(preset('IgG2-STORM'));
    const raw = [];
    D.npcDyesInBlock(seed, pc.cx, pc.cy, N.pores, ids, N.sites, 0.6, 1, raw, pl);
    let sum = 0, far = 0;
    for (const d of raw) {
      const p = N.pores[d.pore], k = d.k, th = ((k & 31) >> 2) * Math.PI / 4, ct = Math.cos(th), st = Math.sin(th);
      const sa = N.sites[3 * k] * 1e-3, sb = N.sites[3 * k + 1] * 1e-3, sc = N.sites[3 * k + 2] * 1e-3;
      const at = [0, 1, 2].map(j => [p.x, p.y, p.z][j] + sa * p.e1[j] + sb * p.e2[j] + sc * p.n[j]);
      const off = [d.pos.x - at[0], d.pos.y - at[1], d.pos.z - at[2]];
      const a = [0, 1, 2].map(j => ct * p.e1[j] + st * p.e2[j]);
      sum += (off[0] * a[0] + off[1] * a[1] + off[2] * a[2]) * 1000;
      far = Math.max(far, Math.hypot(...off) * 1000);
    }
    const m = sum / raw.length;
    report(Math.abs(m - 10) < 0.4 && far <= 10 + 12 + 0.5 + 1e-9,
      `linkage: pore dyes (IgG2-STORM) ${m.toFixed(2)} nm out along the corner direction (10), farthest ${far.toFixed(1)} nm (<= 22.5), ${raw.length} dyes`);
  }
}
for (const [what, l] of [['SPT motion', { motion: 'Diffusive' }], ['off-target', { offTarget: [{ structure: 1, fraction: 0.05 }] }]]) {
  let threw = false;
  try { D.makeLabel(l); } catch { threw = true; }
  report(threw, `${what} rejected (not implemented yet)`);
}
fs.rmSync(tmp, { recursive: true, force: true });
console.log(fails ? `label regression: ${fails} FAILED` : 'label regression: PASS');
process.exit(fails ? 1 : 0);
