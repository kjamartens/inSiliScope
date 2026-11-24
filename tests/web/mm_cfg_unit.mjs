#!/usr/bin/env node
// SPDX-License-Identifier: BSD-3-Clause
// The viewer's Micro-Manager .cfg writer (web/live/mm_cfg.js) under Node, on the generated table (web/live/mm_config.js
// self.ISC_MM_CFG), in well under a second:
//   - a spec at a fresh hub's values gives a configuration of the shipped Expert one with no device settings in Startup
//     but the Core's roles, the Multi Shutter's sources, the shutters closed and the Core-Shutter;
//   - every Startup line is device,property,value (no comma in a value);
//   - a target's mode goes in before its label (a label's choices depend on the mode), a combination before its members;
//   - the light: the lamp for BrightField, both through the Multi Shutter, the lasers else;
//   - the specials: the seed, the Zernike preset and typed modes (all 28), the beam matrix, the BFP maps' text as one line
//     (comments dropped, arguments' commas as ';'), the exposure and the camera's size;
//   - a value with more than 4 decimals at the value Micro-Manager has already is left out (MMDevice keeps 4); a value
//     none of a choice row's choices is noted and left out;
//   - uncovered(): an option no property, special writer or VIEWER_ONLY line covers.
// The round trip through Micro-Manager itself: tools/test_viewer_cfg.py.
//   node tests/web/mm_cfg_unit.mjs
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const t0 = performance.now();
let fail = 0;
const check = (ok, what) => { if (!ok) fail++; console.log(`${ok ? 'ok  ' : 'FAIL'}  ${what}`); };
const self = globalThis.self || (globalThis.self = globalThis);
for (const f of ['web/live/mm_config.js', 'web/live/mm_cfg.js'])
  new Function('self', fs.readFileSync(path.join(ROOT, f), 'utf8'))(self);
const W = self.iscMmCfg, CFG = self.ISC_MM_CFG;
const startup = text => text.split('\n').filter(l => l.startsWith('ConfigGroup,System,Startup,')).map(l => l.split(','));
const at = (settings, dev, prop) => settings.findIndex(s => s[0] === dev && s[1] === prop);
const valueOf = (settings, dev, prop) => { const i = at(settings, dev, prop); return i < 0 ? undefined : settings[i][2]; };

// ---- a fresh hub's values ----
{
  const spec = new Map(Object.entries(CFG.fresh).map(([k, v]) => [k, +v]));
  const r = W.write(spec, { engine: null });
  const lines = startup(r.text);
  const own = lines.filter(l => !(l[3] === 'Core' || l[3] === 'Multi Shutter' || (l[4] === 'State' && l[5] === '0')));
  check(r.settings.length > 100 && !r.warnings.length && own.length === r.settings.length,
    `a fresh hub's spec: ${r.settings.length} settings in Startup, no notes`);
  check(lines.some(l => l[3] === 'Core' && l[4] === 'Shutter' && l[5] === 'Lasers')
    && lines.some(l => l[3] === 'Lasers' && l[4] === 'State' && l[5] === '0')
    && lines.some(l => l[3] === 'Core' && l[4] === 'ChannelGroup'),
    `its Startup keeps the Core's roles, closes the shutters, Core-Shutter Lasers (${lines.length} lines)`);
  check(/^# inSiliScope: "inSiliScope", written by the viewer's/m.test(r.text) && /^Property,Hub,RandomSeed,/m.test(r.text)
    && /^Device,Camera,inSiliScope,Camera$/m.test(r.text), 'the header, the hub and the devices of the shipped Expert configuration');
}

// ---- a spec with every kind of setting ----
const spec = new Map([
  ['seed', 7], ['mt-mode', 0], ['mt-dye', 0], ['mt-dye.qy', 0.31], ['camera-preset', 1], ['camera-type', -1],
  ['p.chunkSize', 30], ['psf-zernike-preset', 2], ['zern.5', 0.1], ['zern.12', -0.05],
  ['beam.0.ux', 0.5], ['beam.0.nm', 561], ['beam.1.ux', -0.5], ['beam.1.phase', 0.25],
  ['sim-pair-phase.0.0', 0], ['sim-pair-phase.0.1', 0], ['sim-pair-phase.1.0', 110.77], ['sim-pair-phase.1.1', 166.15], ['sim-pair-phase.2.0', 221.5], ['sim-pair-phase.2.1', 332.3],
  ['bfp-maps', 'I = spot(ux - 0.9, uy, 0.15)  # a comment, with a comma\ncoherent = 0\nI2 = img("a b.tif")'],
  ['exposure-ms', 30], ['size', 96], ['height', 64], ['ruler-paint-on-sec', 0.288539], ['dichroic', 999],
]);
const engine = new Map([['ruler-paint-on-sec', { def: 0.288539 }]]);
const r = W.write(spec, { engine, name: 'unit', date: '2026-11-19', optics: { emissionMag: 0.5 } });
const S = r.settings;
check(r.text.split('\n').every(l => !l.startsWith('ConfigGroup,System,Startup,') || l.split(',').length === 6),
  'every Startup line is device,property,value');
check(/^Property,Hub,RandomSeed,7$/m.test(r.text), 'the seed is the hub\'s RandomSeed');
const mode = at(S, 'CellField', 'Microtubules_Mode'), label = at(S, 'CellField', 'Microtubules_Label');
check(mode >= 0 && label > mode && valueOf(S, 'CellField', 'Microtubules_Mode') === 'dSTORM',
  `a target's mode before its label (${mode} < ${label})`);
const cam = at(S, 'Camera', 'CameraPreset');
check(cam >= 0 && !S.some(s => s[1] === 'CameraType'), 'a camera preset, its type (-1: the preset\'s) left to it');
check(valueOf(S, 'CellField', 'ChunkUm') !== undefined || S.some(s => s[2] === '30'),
  'a world parameter under the viewer\'s name (p.chunkSize) reaches its property');
check(valueOf(S, 'Objective', 'ZernikePreset') === CFG.zernike[2], `the Zernike preset ${CFG.zernike[2]}`);
const z = (valueOf(S, 'Objective', 'ZernikeCoefficients') || '').split(' ');
check(z.length === 28 && z[5] === '0.1' && z[12] === '-0.05' && z[0] === '0', 'the typed Zernike modes: all 28, space-separated');
check(valueOf(S, 'Lasers', 'Beams') === '0.5 0 0 0 0 0 0 0 0 0 561; -0.5 0 0 0 0 0 0.25 0 0 0 0',
  `the beam matrix: ${valueOf(S, 'Lasers', 'Beams')}`);
check(valueOf(S, 'Lasers', 'PatternPhaseMatrix') === '0 0; 110.77 166.15; 221.5 332.3', `the phase matrix: ${valueOf(S, 'Lasers', 'PatternPhaseMatrix')}`);
check(valueOf(S, 'Lasers', 'BfpMaps') === 'I = spot(ux - 0.9; uy; 0.15); coherent = 0; I2 = img("a b.tif")',
  `the BFP maps as one line: ${valueOf(S, 'Lasers', 'BfpMaps')}`);
{
  const c = W.write(new Map([['bfp-maps', 'I = img("a,b.tif")']]), { engine: null });
  check(at(c.settings, 'Lasers', 'BfpMaps') < 0 && c.warnings.some(w => /comma/.test(w)), 'a map image named with a comma: noted, left out');
}
check(W.bfpOneLine('I = spot(ux, uy, 0.1) # x, y\n\npol = s') === 'I = spot(ux; uy; 0.1); pol = s',
  'bfpOneLine: comments dropped, the arguments\' commas as \';\'');
check(r.warnings.some(w => /img\(/.test(w)), 'a note on the map images');
check(valueOf(S, 'Camera', 'Exposure') === '30' && valueOf(S, 'Camera', 'SensorWidthPx') === '96'
  && valueOf(S, 'Camera', 'SensorHeightPx') === '64', 'the exposure and the camera\'s size');
check(valueOf(S, 'EmissionPath', 'EmissionMagnification') === '0.5', 'the pixel size\'s relay');
check(at(S, 'Nanorulers', 'Docking_BoundTimeSec') < 0, 'a value with more than 4 decimals at the engine\'s default is left out');
check(at(S, 'Dichroic', 'Label') < 0 && r.warnings.some(w => /dichroic = 999/.test(w)), 'a value none of the choices is noted and left out');
check(/^# Note: /m.test(r.text) && /"unit", written .* on 2026-11-19/.test(r.text), 'the notes and the name in the header');

// ---- the light ----
const light = extra => W.write(new Map(extra), { engine: null }).shutter;
check(light([['modality', 1]]) === 'TransmittedLamp' && light([['light-trans', 1]]) === 'Multi Shutter' && light([]) === 'Lasers',
  'the light: the lamp for BrightField, both through the Multi Shutter, the lasers else');

// ---- uncovered ----
const un = W.uncovered(['mt-dye', 'p.chunkSize', 'zern.3', 'beam.2.ux', 'sim-pair-phase.1.2', 'x', 'bfp-maps', 'made-up-key', 'p.madeUp'], CFG);
check(JSON.stringify(un) === '["made-up-key","p.madeUp"]', `uncovered: ${JSON.stringify(un)}`);

console.log(`mm_cfg_unit: ${fail ? 'FAILED' : 'PASS'} (${((performance.now() - t0) / 1000).toFixed(2)} s)`);
process.exit(fail ? 1 : 0);
