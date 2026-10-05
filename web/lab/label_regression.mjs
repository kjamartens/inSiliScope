// Issue 16 regression: the label model reproduces the old two-population world exactly where they overlap.
//   node web/lab/label_regression.mjs [--base origin/main]
// The base's scope/dyes.js + world.js (git show) against the working tree's, same prototype geometry:
//   old (labelEfficiency e, labelNonBleaching 0)  == new PALM, density e, no pre state  (bleaching dyes)
//   old (labelEfficiency 0, labelNonBleaching p)  == new DNA-PAINT, density p            (persistent sites)
//   old (e, p) dyes == new density e + p (the dye set; the old split has no single-label equivalent)
// Same events (positions, times, brightness, id, order). Also checks the new parts' basic statistics.
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

// The base's scope modules into a temp dir (they import each other relatively).
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'isc-label-'));
for (const f of ['rng.js', 'dyes.js', 'world.js'])
  fs.writeFileSync(path.join(tmp, f), execFileSync('git', ['show', `${base}:web/prototype/scope/${f}`], { cwd: ROOT, maxBuffer: 1 << 26 }));
const Old = await import(pathToFileURL(path.join(tmp, 'world.js')).href);
const New = await import(pathToFileURL(path.join(ROOT, 'web/prototype/scope/world.js')).href);
const D = await import(pathToFileURL(path.join(ROOT, 'web/prototype/scope/dyes.js')).href);

const seed = 1249, geom = P.paramsFrom(P.defaults);
const win = [64, 4, 68, 8, -5, 50];
const kin = { activationRatePerSec: 0.05, onSec: 0.05, offSec: 1.0, bleachProb: 0.5, photonCV: 0.5 };
let fails = 0;
const same = (what, a, b) => {
  const strip = e => [e.x, e.y, e.z, e.tOn, e.tOff, e.brightness, e.id];
  const ok = a.length === b.length && a.every((e, i) => strip(e).every((v, j) => v === strip(b[i])[j]));
  console.log(`${ok ? 'ok  ' : 'FAIL'} ${what}: ${a.length} vs ${b.length} events`);
  if (!ok) fails++;
};
const oldWorld = (e, p) => new Old.World(P, seed, { ...geom, labelEfficiency: e, labelNonBleaching: p }, kin);
const newWorld = (label) => new New.World(P, seed, geom, [D.makeLabel({ kinetics: kin, ...label })]);

same('bleaching dyes == PALM', oldWorld(0.1, 0).eventsInWindow(...win, 0, 60), newWorld({ mode: 'PALM', density: 0.1 }).eventsInWindow(...win, 0, 60));
same('persistent sites == DNA-PAINT', oldWorld(0, 0.7).eventsInWindow(...win, 10, 12), newWorld({ mode: 'DNA-PAINT', density: 0.7 }).eventsInWindow(...win, 10, 12));
{
  const a = oldWorld(0.3, 0.4).sitesInWindow(...win), b = newWorld({ density: 0.7 }).sitesInWindow(...win);
  const ok = a.length === b.length && a.every((d, i) => d.id === b[i].id && d.x === b[i].x && d.z === b[i].z);
  console.log(`${ok ? 'ok  ' : 'FAIL'} dye set (0.3 + 0.4 == 0.7): ${a.length} vs ${b.length} dyes`); if (!ok) fails++;
}
// Nesting: a fluorescent fraction keeps a subset of the same dyes.
{
  const all = newWorld({ density: 0.7 }).sitesInWindow(...win), half = newWorld({ density: 0.7, fluorescentFraction: 0.5 }).sitesInWindow(...win);
  const ids = new Set(all.map(d => d.id)), frac = half.length / all.length;
  const ok = half.every(d => ids.has(d.id)) && Math.abs(frac - 0.5) < 0.02;
  console.log(`${ok ? 'ok  ' : 'FAIL'} fluorescent fraction 0.5: ${half.length}/${all.length} = ${frac.toFixed(3)}, a subset`); if (!ok) fails++;
}
// dSTORM: initial ON windows Exp(initialOnSec), blinks after; WideField: one always-on window per dye.
{
  const w = newWorld({ mode: 'dSTORM', density: 0.1, kinetics: { ...kin, initialOnSec: 2 } });
  const c = w.continuousInWindow(...win), m = c.reduce((s, e) => s + e.tOff, 0) / c.length;
  const n = w.sitesInWindow(...win).length, ev = w.eventsInWindow(...win, 0, 1e9);
  const ok = c.length === n && Math.abs(m - 2) < 0.1 && c.every(e => e.state === D.EVENT_STATE.INITIAL_ON);
  console.log(`${ok ? 'ok  ' : 'FAIL'} dSTORM initial ON: ${c.length} windows for ${n} dyes, mean ${m.toFixed(3)} s (2), ${ev.length} blinks`); if (!ok) fails++;
  const wf = newWorld({ mode: 'WideField', density: 0.1 }).continuousInWindow(...win);
  const ma = wf.reduce((s, e) => s + e.aux, 0) / wf.length;
  const ok2 = wf.length === n && wf.every(e => e.tOff === Infinity && e.state === D.EVENT_STATE.ALWAYS_ON) && Math.abs(ma - 1) < 0.05;
  console.log(`${ok2 ? 'ok  ' : 'FAIL'} WideField: ${wf.length} always-on windows, mean aux ${ma.toFixed(3)} (1)`); if (!ok2) fails++;
  const pw = newWorld({ mode: 'PALM', density: 0.1, preState: true });
  // Sites and continuous windows come in the same dye order (ids collide: compare by address).
  const pre = pw.continuousInWindow(...win), ps = pw.sitesInWindow(...win), lab = pw.labels[0];
  const ok3 = pre.length === n && ps.length === n && pre.every((e, i) => {
    const d = ps[i], bl = D.dyeSchedule(D.dyeH1(seed, d.cx, d.cy, d.mtIndex), d.k, d.n, lab.kinetics);
    return e.state === D.EVENT_STATE.PRE && e.x === d.x && e.tOff === (bl.length ? bl[0].tOn : Infinity);
  });
  console.log(`${ok3 ? 'ok  ' : 'FAIL'} PALM pre state ends at the first blink: ${pre.length} windows`); if (!ok3) fails++;
}
// Orientation: Free = no dipole; Random isotropic (<z^2> = 1/3); Fixed at polar 0 = along the microtubule axis.
{
  const sites = w0 => w0.sitesInWindow(...win).slice(0, 4000);
  const fr = newWorld({ density: 0.7 });
  const ok0 = sites(fr).every(d => fr.dyeOrientation(d) === null);
  const rw = newWorld({ density: 0.7, orientation: { mode: 'Random', wobbleDeg: 20 } });
  const zs = sites(rw).map(d => rw.dyeOrientation(d)), z2 = zs.reduce((s, o) => s + o.z * o.z, 0) / zs.length;
  const mx = zs.reduce((s, o) => s + o.x, 0) / zs.length;
  const fw = newWorld({ density: 0.7, orientation: { mode: 'Fixed', polarDeg: 90, azimuthDeg: 0 } });
  // polar 90, azimuth 0 = radial: perpendicular to the axis and of unit length.
  const fo = sites(fw).map(d => fw.dyeOrientation(d)), norms = fo.every(o => Math.abs(Math.hypot(o.x, o.y, o.z) - 1) < 1e-12);
  const ok = ok0 && Math.abs(z2 - 1 / 3) < 0.02 && Math.abs(mx) < 0.03 && norms && zs.every(o => o.wobbleDeg === 20);
  console.log(`${ok ? 'ok  ' : 'FAIL'} orientation: Free null, Random <z^2> ${z2.toFixed(3)} (1/3) <x> ${mx.toFixed(3)}, Fixed unit vectors`); if (!ok) fails++;
}
for (const [what, l] of [['SPT motion', { motion: 'Diffusive' }], ['off-target', { offTarget: [{ structure: 1, fraction: 0.05 }] }]]) {
  let threw = false;
  try { D.makeLabel(l); } catch { threw = true; }
  console.log(`${threw ? 'ok  ' : 'FAIL'} ${what} rejected (not implemented yet)`); if (!threw) fails++;
}
fs.rmSync(tmp, { recursive: true, force: true });
console.log(fails ? `label regression: ${fails} FAILED` : 'label regression: PASS');
process.exit(fails ? 1 : 0);
