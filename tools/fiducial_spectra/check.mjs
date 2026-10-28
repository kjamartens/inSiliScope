#!/usr/bin/env node
// Checks the fiducial data against balinovic2019 (< 1 s):
//  - every quoted efficiency from the curves as resampled (data/fiducials/paper_spectra.json 'quoted'): within 20 % of
//    the quoted value above 5 %, within a factor 2 below (the tails a linear plot cannot resolve);
//  - the fitted tails and 'emitters' are current (tools/fiducial_spectra/fit.mjs leaves both files unchanged);
//  - gold_mie.json is current (gold_mie.mjs --check);
//  - for information: the paper's dark red curves against FPbase's dark red FluoSpheres (the TetraSpeck dark red proxy).
//   node tools/fiducial_spectra/check.mjs
// BSD-3-Clause.
import fs from 'node:fs';
import path from 'node:path';
import { execFileSync } from 'node:child_process';
import { ROOT, PAPER_FILE, readPaper, curveValues, quotedEfficiency } from './curves.mjs';
import { sampleAt, idealTransmission, detection } from '../spectra.mjs';

const t0 = performance.now();
let bad = 0;
const paper = readPaper();
for (const [id, q] of Object.entries(paper.quoted)) {
  if (id === 'about') continue;
  for (const r of quotedEfficiency(q, curveValues(paper.curves[q.curve]))) {
    const ok = r.quoted >= 0.05 ? Math.abs(r.computed / r.quoted - 1) <= 0.2 : r.computed / r.quoted <= 2 && r.computed / r.quoted >= 0.5;
    console.log(`${ok ? 'ok  ' : 'FAIL'} ${id}: ${r.what}: quoted ${r.quoted}, computed ${r.computed.toPrecision(3)}`);
    if (!ok) ++bad;
  }
}

// fit.mjs must leave both files unchanged.
const TYPES_FILE = path.join(ROOT, 'data', 'fiducials', 'types.json');
const before = [PAPER_FILE, TYPES_FILE].map(f => fs.readFileSync(f, 'utf8'));
execFileSync(process.execPath, [path.join(ROOT, 'tools', 'fiducial_spectra', 'fit.mjs')], { stdio: 'ignore' });
const after = [PAPER_FILE, TYPES_FILE].map(f => fs.readFileSync(f, 'utf8'));
[PAPER_FILE, TYPES_FILE].forEach((f, i) => {
  if (before[i].replace(/\r\n/g, '\n') !== after[i].replace(/\r\n/g, '\n')) {
    console.log(`FAIL ${path.relative(ROOT, f)} was not current: fit.mjs rewrote it (review and commit the change)`);
    ++bad;
  }
});
try {
  execFileSync(process.execPath, [path.join(ROOT, 'tools', 'fiducial_spectra', 'gold_mie.mjs'), '--check'], { stdio: 'ignore' });
} catch { console.log('FAIL data/fiducials/gold_mie.json is stale (run tools/fiducial_spectra/gold_mie.mjs)'); ++bad; }

// Information: the paper's dark red FluoSpheres vs FPbase's (spectra 4232 / 4233).
const fp = JSON.parse(fs.readFileSync(path.join(ROOT, 'data', 'dyes', 'fpbase_spectra.json'), 'utf8')).spectra;
if (fp[4232] && fp[4233]) {
  const band = (lo, hi) => [idealTransmission({ type: 'bandpass', loNm: lo, hiNm: hi })];
  const rows = [['paper', curveValues(paper.curves.darkRedEx), curveValues(paper.curves.darkRedEm)], ['FPbase', fp[4232].values, fp[4233].values]];
  for (const [who, ex, em] of rows)
    console.log(`info dark red (${who}): excitation at 561 nm ${sampleAt(ex, 561).toPrecision(3)}, emission in 568.5-642.5 `
      + `${detection(em, band(568.5, 642.5)).fraction.toPrecision(3)}, 570-640 ${detection(em, band(570, 640)).fraction.toPrecision(3)}, `
      + `570-620 ${detection(em, band(570, 620)).fraction.toPrecision(3)}`);
}
console.log(`${bad ? 'FAILED' : 'passed'} in ${((performance.now() - t0) / 1000).toFixed(2)} s`);
process.exit(bad ? 1 : 0);
