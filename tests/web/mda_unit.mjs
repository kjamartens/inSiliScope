#!/usr/bin/env node
// SPDX-License-Identifier: BSD-3-Clause
// A multi-D Record's plan (web/live/mda.js) under Node, in well under a second:
//   - values: lists, ranges, names;
//   - the for blocks run in the order built, nested as built: the example of 2026-11-23 (z outside, positions inside,
//     a setting loop with recording A and, beside it in the position loop, recording B), the spec's overrides with the
//     innermost value winning;
//   - P / Z / C / T: four channels, relative Z offsets one index across positions, a cell visited twice appends in T,
//     blank planes where a cell is short;
//   - the files' pages (OME: run order then the blanks; ImageJ: c fastest per position; flat), index.csv, the OME-XML's
//     sizes and TiffData runs, the ImageJ description;
//   - the plan's checks: the keys refused and why, bad values, empty loops, no recording;
//   - settings by their Micro-Manager names (web/live/mm_cfg.js planKeys on the real table): choices with what they
//     set, the phase matrix / Beams / Zernike modes / BFP maps whole, the value now; quoted and whole values, a
//     recording's settings text with a matrix in it; a loop over two phase matrices; names that map to refused keys.
//   node tests/web/mda_unit.mjs
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const t0 = performance.now();
let fail = 0;
const check = (ok, what) => { if (!ok) fail++; console.log(`${ok ? 'ok  ' : 'FAIL'}  ${what}`); };
const self = globalThis.self || (globalThis.self = globalThis);
new Function('self', fs.readFileSync(path.join(ROOT, 'web/live/settings_core.js'), 'utf8'))(self);
new Function('self', fs.readFileSync(path.join(ROOT, 'web/live/mda.js'), 'utf8'))(self);
const D = self.ISC_MDA;
const eng = new Map([['laser-561', { def: 0, part: 'settings', names: null }], ['laser-640', { def: 0, part: 'settings', names: null }],
  ['sim-type', { def: 0, part: 'settings', names: ['Off', 'SIM2D', 'SIM3D'] }], ['size', { def: 128, part: 'measurement', names: null }]]);
const known = k => eng.has(k);

// ---- values ----
check(JSON.stringify(D.parseValues('1, 2, 3')) === '[1,2,3]', 'a list');
check(JSON.stringify(D.parseValues('0:0.5:2')) === '[0,0.5,1,1.5,2]', 'a range, both ends in');
check(JSON.stringify(D.parseValues('2:-1:0')) === '[2,1,0]', 'a range downwards');
check(JSON.stringify(D.parseValues('0.1:0.1:0.3')) === '[0.1,0.2,0.3]', 'a range with decimal steps (no 0.30000000000000004)');
check(JSON.stringify(D.parseValues('P1; SIM2D')) === '["P1","SIM2D"]', 'names stay text');
let threw = false; try { D.parseValues('0:-1:2'); } catch (e) { threw = true; }
check(threw, 'a range stepping away from its end is refused');

// ---- the example: for z in 1,2,3 { for P in P1..P3 { for laser-561 in 10,20,30 { A } ; for laser-561 in 40 { B } } } ----
const plan = {
  positions: [{ name: 'P1', x: 0, y: 0, z: null }, { name: 'P2', x: 10, y: 0, z: 2 }, { name: 'P3', x: 0, y: 10, z: null }],
  root: [{ kind: 'loop', axis: 'z', values: '1, 2, 3', children: [
    { kind: 'loop', axis: 'position', values: 'P1, P2, P3', children: [
      { kind: 'loop', axis: 'setting', key: 'laser-561', values: '10, 20, 30', children: [
        { kind: 'record', name: 'A', frames: 4, exposureMs: 10, settings: {} }] },
      { kind: 'loop', axis: 'setting', key: 'laser-561', values: [40], children: [
        { kind: 'record', name: 'B', frames: 2, exposureMs: 20, settings: { 'laser-640': 5, 'sim-type': 'SIM2D' } }] }] }] }] };
const base = { x: 1, y: 2, z: 0.5 };
check(D.validate(plan, base, { eng, known }).length === 0, 'the example is a valid plan: ' + D.validate(plan, base, { eng, known }).join('; '));
const B = D.expand(plan, base, eng);
check(B.length === 3 * 3 * 4, `3 z x 3 positions x (3 + 1) recordings = 36 blocks (${B.length})`);
const order = B.slice(0, 9).map(b => `${b.z}/${b.pName}/${b.rec}${b.ov['laser-561']}`).join(' ');
check(order === '1/P1/A10 1/P1/A20 1/P1/A30 1/P1/B40 1/P2/A10 1/P2/A20 1/P2/A30 1/P2/B40 1/P3/A10', 'run order: z, then position, then A x 3, then B: ' + order);
check(B[4].x === 10 && B[4].y === 0 && B[4].z === 1, 'a position\'s x, y; an absolute Z loop overrides its z');
check(B[3].ov['exposure-ms'] === 20 && B[3].ov['laser-640'] === 5 && B[3].ov['sim-type'] === 1, 'B: its exposure, its own settings, a name made the option\'s number');
check(B[0].ov['exposure-ms'] === 10 && !('laser-640' in B[0].ov), 'A: its own exposure, no B setting');
check(B[0].cName === 'A · laser-561=10' && B[3].cName === 'B · laser-561=40', 'channel names: ' + B[0].cName + ' / ' + B[3].cName);
const n = B.reduce((s, b) => s + b.frames, 0);
check(n === 9 * (3 * 4 + 2), `frames ${n}`);
const L = D.layout(B, n, plan.positions);
check(L.P.length === 3 && L.Z.length === 3 && L.C.length === 4 && L.T === 4, `P ${L.P.length} Z ${L.Z.length} C ${L.C.length} T ${L.T}`);

// ---- relative Z: one index per offset across positions; a revisit appends in T ----
const rel = { positions: [{ name: 'a', x: 0, y: 0, z: 1 }, { name: 'b', x: 5, y: 5, z: 3 }], root: [
  { kind: 'loop', axis: 'position', values: 'a, b, a', children: [
    { kind: 'loop', axis: 'z', values: '-0.5:0.5:0.5', relative: true, children: [{ kind: 'record', name: 'R', frames: 2, exposureMs: 5, settings: {} }] }] }] };
const RB = D.expand(rel, base, eng), RL = D.layout(RB, 18, rel.positions);
check(RB[3].z === 2.5 && RB[0].z === 0.5, 'relative z = the position\'s z + the offset');
check(RL.P.length === 2 && RL.Z.length === 3 && RL.C.length === 1 && RL.T === 4, `relative: P 2, Z 3 (not 6), C 1, T 4 (a revisited by the third loop value) (${RL.P.length} ${RL.Z.length} ${RL.C.length} ${RL.T})`);
check(RL.frames[12].t === 2 && RL.frames[12].p === 0, 'the revisit\'s frames follow the first visit\'s in T');

// ---- files and pages ----
const part = D.layout(RB, 8, rel.positions);   // stopped part way: a (3 z x 2) done, b's first z
const om = D.files(part, 'ome')[0];
const pads = om.pages.filter(p => p.pad).length;
check(part.P.length === 2 && part.T === 2 && om.pages.length === 8 + pads && pads === 2 * 3 * 1 * 2 - 8, `OME pages: 8 recorded then ${pads} blank`);
check(om.pages.slice(0, 8).every((p, i) => p.f === i), 'OME: the recorded frames first, in run order');
const ij = D.files(L, 'imagej');
check(ij.length === 3 && ij.map(f => f.name).join() === 'P1.tif,P2.tif,P3.tif', 'ImageJ: a file per position');
const f0 = ij[0].pages, at = (t, z, c) => f0[(t * L.Z.length + z) * L.C.length + c];
check(f0.length === 4 * 3 * 4 && at(0, 0, 1).f === 4 && at(1, 0, 0).f === 1 && at(2, 1, 3).pad && at(1, 2, 3).f !== undefined,
  'ImageJ: c fastest, then z, then t; B (2 frames) padded at t 2, 3');
check(D.files(L, 'flat')[0].pages.length === n, 'flat: every frame in run order');
const place = Array.from({ length: n }, (_, i) => ({ t: 60 + i * 0.01 }));
const csv = D.indexCsv(L, B, ij, place).trim().split('\n');
check(csv.length === 1 + 3 * 48, `index.csv: a row per page (${csv.length - 1})`);
check(csv[0].startsWith('file,page,position,position_name,x_um,y_um,z_um,z_index'), 'index.csv header');
const r5 = csv[1 + 1].split(',');   // P1.tif page 1 = (t 0, z 0, c 1) = frame 4
check(r5[0] === 'P1.tif' && r5[15] === '4' && r5[10] === 'A · laser-561=20' && r5[18] === '0', 'a row: its file, frame, channel, not padding: ' + csv[2]);
check(csv.some(l => /,1,$/.test(l)), 'padding rows are marked');
const omeL = D.files(L, 'ome')[0];
const x = D.omeXml(L, B, omeL, { w: 64, h: 48, pxUm: 0.1, version: 'test', place, t0: 60 });
check((x.match(/<Image /g) || []).length === 3 && /SizeX="64" SizeY="48" SizeZ="3" SizeC="4" SizeT="4"/.test(x), 'OME: an Image per position, the sizes');
check((x.match(/<Plane /g) || []).length === n, 'OME: a Plane per recorded frame');
check(/<TiffData IFD="0" FirstC="0" FirstZ="0" FirstT="0" PlaneCount="4"\/>/.test(x), 'OME: a block\'s frames as one TiffData run');
const tdCount = [...x.matchAll(/PlaneCount="(\d+)"/g)].reduce((s, m) => s + +m[1], 0);
check(tdCount === omeL.pages.length && tdCount === 3 * 3 * 4 * 4, `OME: TiffData cover every page once (${tdCount})`);
const opens = (x.match(/<(Image|Pixels|OME)[ >]/g) || []).length, closes = (x.match(/<\/(Image|Pixels|OME)>/g) || []).length;
check(opens === closes, 'OME: elements closed');
check(/^ImageJ=1\.11a\nimages=48\nchannels=4\nslices=3\nframes=4\nhyperstack=true\nmode=composite\n/.test(D.imagejDesc(L)), 'ImageJ description');
check(D.blockAt(B, 13).b === 3 && D.blockAt(B, 13).k === 1 && D.blockAt(B, 15).b === 4, 'a run frame\'s block');

// ---- refusals ----
const bad = (root, positions = []) => D.validate({ positions, root }, base, { eng, known });
const rec = { kind: 'record', name: 'A', frames: 1, exposureMs: 10, settings: {} };
const has = (errs, re) => errs.some(e => re.test(e));
check(has(bad([{ kind: 'loop', axis: 'setting', key: 'size', values: '64,128', children: [rec] }]), /size cannot be set here: changes the image size/), 'size refused (one stack, one size)');
check(has(bad([{ kind: 'loop', axis: 'setting', key: 'p.cellRough', values: '0.1', children: [rec] }]), /a new sample/), 'a sample key refused');
check(has(bad([{ kind: 'loop', axis: 'setting', key: 'z', values: '1', children: [rec] }]), /Position or Z loop/), 'z as a setting refused');
check(has(bad([{ kind: 'loop', axis: 'setting', key: 'nope', values: '1', children: [rec] }]), /no setting nope/), 'an unknown key');
check(has(bad([{ kind: 'loop', axis: 'setting', key: 'sim-type', values: 'SIM9', children: [rec] }]), /one of Off, SIM2D, SIM3D/), 'a name the option does not have');
check(has(bad([{ kind: 'loop', axis: 'z', values: '', children: [rec] }]), /no values/), 'a loop without values');
check(has(bad([{ kind: 'loop', axis: 'z', values: '1', children: [] }]), /nothing inside/), 'a loop with nothing inside');
check(has(bad([{ kind: 'loop', axis: 'position', values: 'Q', children: [rec] }], [{ name: 'P1', x: 0, y: 0 }]), /no position Q/), 'an unknown position');
check(has(bad([{ ...rec, frames: 0 }]), /frames, a whole number/), 'zero frames');
check(has(bad([]), /No recording/), 'no recording');
check(has(bad([rec], [{ name: 'P', x: 0, y: 0 }, { name: 'P', x: 1, y: 1 }]), /used twice/), 'a position name used twice');

// ---- settings by the Micro-Manager names their help shows (web/live/mm_cfg.js planKeys, the real table) ----
for (const f of ['web/live/mm_config.js', 'web/live/mm_cfg.js']) new Function('self', fs.readFileSync(path.join(ROOT, f), 'utf8'))(self);
const K = self.iscMmCfg.planKeys({ engine: eng, known });
const KO = n => JSON.stringify(K.resolve(n.split('=')[0], n.slice(n.indexOf('=') + 1)));
check(K.nameOf('laser-561') === 'Lasers.Power561' || /^Lasers\./.test(K.nameOf('laser-561')), `an option's name is its property (laser-561 = ${K.nameOf('laser-561')})`);
check(K.nameOf('sim-type') === 'Lasers.Pattern', 'sim-type = Lasers.Pattern');
check(KO('Lasers.Pattern=SIM2D') === '{"sim-type":1}' || /"sim-type":1/.test(KO('Lasers.Pattern=SIM2D')), `a choice by its name (${KO('Lasers.Pattern=SIM2D')})`);
check(/"npc-mode":2/.test(KO('CellField.NuclearPores_Mode=DNA-PAINT')) && /"npc-label-pct"/.test(KO('CellField.NuclearPores_Mode=DNA-PAINT')),
  'a choice brings what it sets (a mode: its labelling, as Micro-Manager)');
const pm = K.resolve('Lasers.PatternPhaseMatrix', '0; 240; 470; 0 ;240; 480; 0; 240; 480');
check(pm['sim-pair-phase.2.0'] === 470 && Object.keys(pm).length === 9, 'the phase matrix: rows by ; -> sim-pair-phase.<row>.<pair>');
const pm2 = K.resolve('Lasers.PatternPhaseMatrix', '0 0\n110 166');
check(pm2['sim-pair-phase.1.1'] === 166 && Object.keys(pm2).length === 4, 'the phase matrix: two pairs, rows by new lines');
const bm = K.resolve('Lasers.Beams', '0.9 0 1 0 0 1 0 0 0 0 0; -0.9 0 1 0 0 1 0.5 0 0 0 0');
check(bm['beam.1.ux'] === -0.9 && bm['beam.1.phase'] === 0.5 && bm['beam.0.nm'] === 0, 'the Beams: rows of the columns ux .. nm');
const zm = K.resolve('Objective.ZernikeCoefficients', '0 0 0 0 0.05 0.08');
check(Object.keys(zm).length === 28 && zm['zern.5'] === 0.08 && zm['zern.27'] === 0, 'the Zernike modes: all 28 (the rest 0)');
check(K.resolve('Lasers.BfpMaps', 'I = 1; phase = x')['bfp-maps'] === 'I = 1; phase = x', 'the BFP maps: the text');
check(K.resolve('laser-640', 5)['laser-640'] === 5, 'an engine key still as itself');
check(K.resolve('Nope.Nothing', 1) === null, 'an unknown name: null');
let bad1 = ''; try { K.resolve('Lasers.PatternPhaseMatrix', '0; x'); } catch (e) { bad1 = e.message; }
check(/numbers/.test(bad1), 'a bad matrix says what it takes');
check(K.whole('Lasers.PatternPhaseMatrix') && K.whole('Lasers.Beams') && !K.whole('Lasers.Pattern'), 'the matrices are whole values');
const now = new Map([['sim-type', 1], ['sim-pair-phase.0.0', 0], ['sim-pair-phase.1.0', 240], ['laser-561', 3]]);
check(K.current('Lasers.Pattern', now) === 'SIM2D' && K.current('Lasers.PatternPhaseMatrix', now) === '0; 240' && K.current('laser-561', now) === 3,
  'the value now, as the name takes it');
// values and settings texts with matrices: quoted, or whole
check(JSON.stringify(D.parseValues('0; 240; 480', true)) === '["0; 240; 480"]', 'a whole setting\'s values: unquoted, one value');
check(JSON.stringify(D.parseValues('"0; 240", "0; 250"', true)) === '["0; 240","0; 250"]', 'a whole setting\'s values: several, quoted');
check(JSON.stringify(D.parseValues('"a, b", 3')) === '["a, b",3]', 'a quoted value keeps its comma');
const isK = n => !!K.resolve(n, 1) || K.whole(n), st = D.parseSettings('Lasers.PatternPhaseMatrix=0; 240; 470; laser-561=10', n => { try { return isK(n); } catch (e) { return true; } }, K.whole);
check(st['Lasers.PatternPhaseMatrix'] === '0; 240; 470' && st['laser-561'] === 10, 'a recording\'s settings: a matrix runs on to the next name=');
check(JSON.stringify(D.parseSettings(D.formatSettings(st), () => true, K.whole)) === JSON.stringify(st), 'settings text round trip (quoted)');
// the editor's text boxes: a setting per line, a matrix's rows on lines of their own (no quotes)
const isK2 = n => { try { return isK(n); } catch (e) { return true; } };
const st2 = D.parseSettings('Lasers.Pattern=SIM2D\nLasers.PatternPhaseMatrix=0 0\n  110 166\n  220 333\nlaser-561=10', isK2, K.whole);
const r2 = K.resolve('Lasers.PatternPhaseMatrix', st2['Lasers.PatternPhaseMatrix']);
check(st2['Lasers.Pattern'] === 'SIM2D' && st2['laser-561'] === 10 && r2['sim-pair-phase.1.1'] === 166 && r2['sim-pair-phase.2.0'] === 220,
  'settings a line each, a matrix\'s rows on lines: one value');
const mv = D.parseValues('"0 0\n110 166",\n"0 0\n120 170"', true);
check(mv.length === 2 && K.resolve('Lasers.PatternPhaseMatrix', mv[1])['sim-pair-phase.1.0'] === 120, 'several matrices with rows on lines, each in quotes');
// a plan over phase matrices by name: the spec's entries per block, refusals through the names
const planM = { positions: [], root: [{ kind: 'loop', axis: 'setting', key: 'Lasers.PatternPhaseMatrix', values: '"0; 240; 480", "0; 240; 470"',
  children: [{ kind: 'record', name: 'A', frames: 2, exposureMs: 10, settings: { 'Lasers.Pattern': 'SIM2D' } }] }] };
const BM = D.expand(planM, base, eng, K);
check(BM.length === 2 && BM[0].ov['sim-pair-phase.2.0'] === 480 && BM[1].ov['sim-pair-phase.2.0'] === 470 && BM[1].ov['sim-type'] === 1,
  'a loop over two phase matrices: each block its entries');
check(D.validate(planM, base, { eng, keys: K }).length === 0, 'that plan is valid');
const shapeErr = D.validate(planM, base, { eng, keys: K, blockCheck: b => b.ov['sim-pair-phase.2.0'] === 470 ? ['shape'] : [] });
check(shapeErr.includes('shape'), 'a block check runs on the blocks');
check(has(D.validate({ positions: [], root: [{ ...rec, settings: { 'Camera.Exposure': 20 } }] }, base, { eng, keys: K }), /Camera\.Exposure cannot be set here \(exposure-ms\): set by the recording/),
  'a name that maps to a refused key is refused, saying which');
check(has(D.validate({ positions: [], root: [{ ...rec, settings: { 'Lasers.PatternPhaseMatrix': '0; x' } }] }, base, { eng, keys: K }), /numbers/), 'a bad matrix in a recording');

console.log(`${fail ? 'FAIL' : 'ok'}: mda_unit, ${(performance.now() - t0).toFixed(0)} ms`);
process.exit(fail ? 1 : 0);
