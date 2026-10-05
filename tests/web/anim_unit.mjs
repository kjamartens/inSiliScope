#!/usr/bin/env node
// SPDX-License-Identifier: BSD-3-Clause
// The animation sequence model and evaluator (web/anim/sequence.js) in Node, with the real layer registry
// (web/scene/core.js): timing, continuous rotation, the zones of up and down sweeps, overshoot, kept layers,
// re-binding, fades, loops, migration, presets, determinism; and that the docs site ships every viewer script.
//   node tests/web/anim_unit.mjs
import fs from 'node:fs';
import path from 'node:path';
import vm from 'node:vm';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const ctx = vm.createContext({ Math, Object, Array, Map, Set, JSON, Date, Float32Array, String, Number, parseInt, Error });
ctx.globalThis = ctx;
for (const f of ['web/scene/core.js', 'web/anim/sequence.js']) vm.runInContext(fs.readFileSync(path.join(ROOT, f), 'utf8'), ctx, { filename: f });
const A = ctx.IscAnimSeq, R = ctx.IscScene.REGISTRY;
let fail = 0;
const check = (ok, what) => { if (!ok) fail++; console.log(`${ok ? 'ok  ' : 'FAIL'}  ${what}`); };
const near = (a, b, tol = 1e-9) => Math.abs(a - b) <= tol;
const bounds = { x: [-15, 15], y: [-12, 12], z: [0, 4], center: [0, 0, 2], diam: 30 };
const C0 = seq => A.compileSequence(seq, { registry: R, bounds, aspect: 16 / 9, target: '0,0' });

// The issue's example: microtubules simulated up, WideField-free variant (data layers come in later phases), then the
// nucleus pops in.
const seq = A.defaults();
seq.cycles = [
  { id: 'c1', name: 'MT', structure: 'mt', orbit: { deg: 360 }, keepAfter: ['$.gt'], steps: [
    { id: 's1', duration: 6, sweep: { axis: 'z', from: { rel: 0 }, to: { rel: 1 }, slab: 0, ease: 'linear' },
      layers: [{ ref: 'cyto.surface', zones: ['ahead'], style: { opacity: 0.15 } }, { ref: '$.gt', zones: ['behind'] }] },
    { id: 's2', duration: 6, sweep: { axis: 'z', from: { rel: 1 }, to: { rel: 0 }, slab: 0.3, ease: 'linear' },
      layers: [{ ref: '$.gt', zones: ['ahead'] }, { ref: 'nucleus.surface', zones: ['at'] }, { ref: 'cyto.outline', zones: ['behind'] }] }] },
  { id: 'c2', name: 'Nucleus', structure: 'nucleus', orbit: { degPerSec: 30 }, steps: [
    { id: 's3', duration: 3, fadeSec: 0.3, layers: [{ ref: '$.gt', zones: ['all'] }] }] },
];
const C = C0(seq);
check(near(C.duration, 15) && C.steps.map(s => s.t0).join() === '0,6,12', 'example compiles to 15 s, steps at 0, 6, 12');
{
  const az = t => A.evalCompiled(C, t).camera.azimuthDeg;
  check(near(az(3), 90, 1e-6) && near(az(6 - 1e-6), az(6 + 1e-6), 1e-3) && near(az(12 - 1e-9), 360, 1e-6) &&
    near(az(13) - az(12.5), 15, 1e-6), 'rotation: 30 deg/s through cycle 1 (360 deg), continuous, 30 deg/s in cycle 2');
}
{
  const f = A.evalCompiled(C, 3);   // up sweep, slab 0, halfway: plane at z = 2
  const mt = f.layers['mt.lines'], cy = f.layers['cyto.surface'];
  check(f.sweep.axis === 'z' && near(mt.intervals[0].hi, 2, 1e-9) && mt.intervals[0].lo <= -1e29 && near(cy.intervals[0].lo, 2, 1e-9) &&
    cy.intervals[0].style.opacity === 0.15, 'up sweep: simulated MTs behind (below) the plane, cytoplasm ghost ahead (above)');
}
{
  // down sweep, slab 0.3, overshoot 0.15: from 4.15 to -0.15
  const S2 = C.steps[1];
  check(near(S2.sweep.from, 4.15) && near(S2.sweep.to, -0.15) && S2.sweep.dir === -1, 'down sweep: overshoot slab/2 at both ends');
  const f = A.evalCompiled(C, 9), s = f.slice.pos;
  const mt = f.layers['mt.lines'], nuc = f.layers['nucleus.surface'], out = f.layers['cyto.outline'];
  check(near(mt.intervals[0].hi, s - 0.15, 1e-9) && near(nuc.intervals[0].lo, s - 0.15, 1e-9) && near(nuc.intervals[0].hi, s + 0.15, 1e-9) &&
    near(out.intervals[0].lo, s + 0.15, 1e-9), 'down sweep: ahead below, the slab at the plane, the wake above');
}
{
  const f = A.evalCompiled(C, 13);
  check(f.layers['mt.lines'] && f.layers['mt.lines'].intervals[0].lo <= -1e29 && f.layers['nucleus.surface'] &&
    !f.layers['cyto.surface'], 'keepAfter: cycle 1\'s microtubules stay shown in cycle 2, everywhere');
  const f0 = A.evalCompiled(C, 12), f1 = A.evalCompiled(C, 12.3);
  check(f0.layers['nucleus.surface'].alpha === undefined ? false : near(f0.layers['nucleus.surface'].alpha, 0, 1e-9) &&
    f1.layers['nucleus.surface'].alpha === undefined, 'pop-in: the new layer fades in over fadeSec');
  check(f0.layers['cyto.outline'] && f0.layers['cyto.outline'].alpha > 0.99 && !A.evalCompiled(C, 12.31).layers['cyto.outline'],
    'a dropped layer fades out over fadeSec');
}
{
  const s2 = A.clone(seq);
  s2.cycles.push({ id: 'c3', structure: 'nucleus', dropKept: ['mt.lines'], steps: [{ duration: 1, layers: [] }] });
  const f = A.evalCompiled(C0(s2), 15.5);
  check(!f.layers['mt.lines'], 'dropKept removes a kept layer');
  // a duplicated cycle re-bound to the nucleus
  const s3 = A.clone(seq);
  const dup = A.clone(s3.cycles[0]); dup.structure = 'nucleus'; s3.cycles.push(dup);
  const C3 = C0(s3), f3 = A.evalCompiled(C3, 15 + 3);
  check(f3.layers['nucleus.surface'] && !!f3.layers['mt.lines'], 're-binding: $.gt of a nucleus cycle is nucleus.surface');
  const bad = A.clone(seq); bad.cycles[1].steps[0].layers.push({ ref: '$.wfSlice', zones: ['all'] });
  check(C0(bad).warnings.some(w => /nucleus.wfSlice/.test(w)), 'a layer the structure does not have is skipped with a warning');
}
{
  check(JSON.stringify(A.evalCompiled(C, 1.25, { loop: 'loop' })) === JSON.stringify(A.evalCompiled(C, 16.25, { loop: 'loop' })),
    'loop: t + duration = t');
  check(JSON.stringify(A.evalCompiled(C, 4, { loop: 'pingpong' })) === JSON.stringify(A.evalCompiled(C, 26, { loop: 'pingpong' })),
    'ping-pong: 2 duration - t = t');
  check(JSON.stringify(A.evalCompiled(C, 7.7)) === JSON.stringify(A.evalCompiled(C0(seq), 7.7)), 'deterministic: same input, same frame');
}
{
  const json = JSON.stringify(Object.assign(A.clone(seq), { futureField: { x: 1 } }));
  const m = A.migrate(json).seq;
  check(m.futureField && m.futureField.x === 1 && JSON.stringify(A.migrate(JSON.stringify(m)).seq) === JSON.stringify(m),
    'migrate: unknown fields survive, a round trip is stable');
  check(A.migrate(Object.assign(A.clone(seq), { version: 99 })).warnings.length === 1, 'migrate: a newer file is flagged');
  let threw = false; try { A.migrate({ format: 'other' }); } catch (e) { threw = true; }
  check(threw, 'migrate: rejects another format');
}
{
  check(near(A.ease('inOut', 0), 0) && near(A.ease('inOut', 1), 1) && near(A.ease('smooth', 0.5), 0.5) && A.ease('hold', 0.99) === 0, 'easing endpoints');
  const iv = A.zoneIntervals(['behind', 'at'], { slab: 0.4, dir: 1 }, 2);
  check(iv.length === 1 && iv[0][0] <= -1e29 && near(iv[0][1], 2.2), 'zones behind + at merge into one interval');
}
{
  let ok = true;
  for (const name of Object.keys(A.PRESETS)) for (const st of ['mt', 'nucleus', 'cyto']) {
    if (!A.presetFits(name, st, R)) continue;
    const s = A.defaults(); s.cycles.push(A.cycleFromPreset(name, { structure: st }));
    if (A.validate(s, R).some(p => p.level === 'error')) ok = false;
  }
  check(ok && !A.presetFits('simUpWfDown', 'nucleus', R), 'presets: every fitting preset validates; data presets need a dye structure');
  check(A.validate(A.starter(), R).length === 0 && A.compileSequence(A.starter(), { registry: R, bounds }).duration > 10, 'the starter sequence is clean');
}
{
  // a screen-horizontal sweep moves with the camera: its positions are relative to the target centre
  const s = A.defaults();
  s.cycles = [{ structure: 'mt', orbit: { deg: 90 }, steps: [{ duration: 4, sweep: { axis: 'h', from: { rel: 0 }, to: { rel: 1 }, slab: 0 },
    layers: [{ ref: '$.gt', zones: ['behind'] }] }] }];
  const b2 = Object.assign({}, bounds, { center: [10, -5, 2] });
  const Ch = A.compileSequence(A.migrate(s).seq, { registry: R, bounds: b2, aspect: 1 });
  const f = A.evalCompiled(Ch, 2), a = f.sweep.axis;
  check(Array.isArray(a) && near(Math.hypot(a[0], a[1]), 1) && near(f.slice.pos - (a[0] * 10 + a[1] * -5), 0, 1e-9),
    'screen-horizontal sweep: the axis turns with the camera, the plane passes the cell centre halfway');
}
{
  // every <script src> of the viewer is copied by the docs-site build
  const html = fs.readFileSync(path.join(ROOT, 'web/index.html'), 'utf8'), sh = fs.readFileSync(path.join(ROOT, 'tools/build_site.sh'), 'utf8');
  const srcs = [...html.matchAll(/<script src="([^"]+)"/g)].map(m => m[1]);
  const dirs = (/for d in ([\w ]+); do/.exec(sh) || [, ''])[1].split(' ');
  const missing = srcs.filter(s => !(s.includes('/') ? dirs.includes(s.split('/')[0]) : sh.includes('web/' + s)));
  check(!missing.length, 'tools/build_site.sh ships every viewer script' + (missing.length ? ': missing ' + missing.join(', ') : ''));
}
process.exit(fail ? 1 : 0);
