#!/usr/bin/env node
// Fits the fiducial data to balinovic2019's numbers and writes the results back:
//  1. each paper curve's tail (data/fiducials/paper_spectra.json 'tail'): the decay length (nm) of the exponential below
//     the curve's first point that reproduces the quoted efficiency it names (several bands: least squares in log);
//  2. each calibrated component's 'emitters' (data/fiducials/types.json 'calibration'): ratio x (eps QY readout of the
//     reference molecule) / (eps QY of the equivalent dye x the bead's excitation at the laser x its emission in the
//     band), from the fitted curves.
//   node tools/fiducial_spectra/fit.mjs
// BSD-3-Clause.
import fs from 'node:fs';
import path from 'node:path';
import { ROOT, PAPER_FILE, readPaper, curveValues, quotedEfficiency, formatJson } from './curves.mjs';
import { sampleAt, idealTransmission, detection } from '../../web/prototype/scope/spectra.js';

const paper = readPaper();
const err = (curve, q, decay) => quotedEfficiency(q, curveValues(curve, decay))
  .reduce((s, r) => s + Math.log(Math.max(r.computed, 1e-12) / r.quoted) ** 2, 0);
for (const [id, curve] of Object.entries(paper.curves)) {
  if (!curve.tail) continue;
  const q = paper.quoted[curve.tail.fitTo];
  if (!q || q.curve !== id) throw new Error(`${id}: tail fitTo '${curve.tail.fitTo}' is not a quoted value of this curve`);
  // Golden-section search over log(decay) in [0.1, 200] nm.
  let a = Math.log(0.1), b = Math.log(200);
  const g = (Math.sqrt(5) - 1) / 2;
  for (let it = 0; it < 100; ++it) {
    const c = b - g * (b - a), d = a + g * (b - a);
    if (err(curve, q, Math.exp(c)) < err(curve, q, Math.exp(d))) b = d; else a = c;
  }
  curve.tail.decayNm = Number(Math.exp((a + b) / 2).toPrecision(4));
  for (const r of quotedEfficiency(q, curveValues(curve)))
    console.log(`${id}: tail decay ${curve.tail.decayNm} nm; ${r.what}: quoted ${r.quoted}, computed ${r.computed.toPrecision(3)}`);
}
fs.writeFileSync(PAPER_FILE, formatJson(paper));

const TYPES_FILE = path.join(ROOT, 'data', 'fiducials', 'types.json');
const types = JSON.parse(fs.readFileSync(TYPES_FILE, 'utf8'));
const eq = types.equivalentDye;
for (const t of types.types) for (const c of t.components) {
  const cal = c.calibration;
  if (!cal) continue;
  const ref = types.references[cal.reference];
  if (!ref) throw new Error(`${t.id} ${c.id}: unknown reference '${cal.reference}'`);
  if (!c.ex.startsWith('paper:') || !c.em.startsWith('paper:')) throw new Error(`${t.id} ${c.id}: a calibrated component needs paper curves`);
  const ex = sampleAt(curveValues(paper.curves[c.ex.slice(6)]), cal.laserNm);
  const em = detection(curveValues(paper.curves[c.em.slice(6)]), [idealTransmission({ type: 'bandpass', loNm: cal.band[0], hiNm: cal.band[1] })]).fraction;
  c.emitters = Number((cal.ratio * ref.extCoeff * ref.qy * cal.readout / (eq.extCoeff * eq.qy * ex * em)).toPrecision(3));
  console.log(`${t.id} ${c.id}: excitation ${ex.toPrecision(3)} at ${cal.laserNm} nm, emission ${em.toPrecision(3)} in ${cal.band.join('-')} nm `
    + `-> ${c.emitters} equivalent dyes at ${t.refDiameterNm} nm`);
}
fs.writeFileSync(TYPES_FILE, formatJson(types));
