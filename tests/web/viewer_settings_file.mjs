#!/usr/bin/env node
// SPDX-License-Identifier: BSD-3-Clause
// Save and load settings (web/live/settings_file.js, docs/dev/SAVE_SETTINGS.md) in headless Chromium:
//   - page A: a value changed in each part, live frames with a laser step and a stage move, Save settings (the dialog,
//     a download): every engine option of each part in the file, enumerations by name, the light history's runs;
//   - page B (a browser context of its own: nothing restored from A): Load -> the same movieSpec(), the light history
//     replayed, and the next live frame (a Snap) = page A's next one, bit for bit;
//   - another file's instrument over it: its camera values, the lasers and the sample stay;
//   - a file from another release is warned about, a key the viewer does not know listed;
//   - the saved file rendered by insiliscope_cli --settings (when built): the same next frame.
//   node tests/web/viewer_settings_file.mjs [--cli=<insiliscope_cli>] [--swiftshader]
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { execFileSync } from 'node:child_process';
import { createRequire } from 'node:module';
import { fileURLToPath } from 'node:url';
import http from 'node:http';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const require = createRequire(path.join(ROOT, 'web/tools/package.json'));
let chromium;
try { ({ chromium } = require('playwright')); }
catch { ({ chromium } = await import('/opt/node22/lib/node_modules/playwright/index.mjs')); }
const arg = k => { const a = process.argv.find(s => s.startsWith('--' + k)); return a ? (a.split('=')[1] || true) : null; };
const server = http.createServer((req, res) => {
  const f = path.join(ROOT, 'web', decodeURIComponent(req.url.split('?')[0]).replace(/^\/+/, '') || 'index.html');
  if (!f.startsWith(path.join(ROOT, 'web')) || !fs.existsSync(f) || fs.statSync(f).isDirectory()) { res.statusCode = 404; res.end(); return; }
  res.setHeader('content-type', f.endsWith('.js') ? 'text/javascript' : 'text/html');
  res.end(fs.readFileSync(f));
}).listen(0);
const URL0 = `http://localhost:${server.address().port}/index.html?live=0`;

let fail = 0;
const t0 = Date.now(), sec = () => ((Date.now() - t0) / 1000).toFixed(0);
const check = (ok, what) => { if (!ok) fail++; console.log(`${ok ? 'ok  ' : 'FAIL'}  ${what}  [${sec()} s]`); };
const dog = setTimeout(() => { console.log('FAIL  no end after 240 s'); process.exit(2); }, 240000);
const browser = await chromium.launch({ args: arg('swiftshader') ? ['--use-angle=swiftshader', '--enable-unsafe-swiftshader'] : ['--enable-gpu', '--ignore-gpu-blocklist'] });
const errors = [];

async function openPage() {
  const ctx = await browser.newContext({ viewport: { width: 1200, height: 800 }, acceptDownloads: true });
  const page = await ctx.newPage();
  page.on('pageerror', e => errors.push(e.message));
  await page.goto(URL0);
  await page.waitForFunction(() => window.liveState && window.iscSettings && typeof genWorkers !== 'undefined' && genWorkers.ok &&
    self.ISC_ENGINE_OPTIONS && self.ISC_ENGINE_OPTIONS.size > 100, null, { timeout: 120000 });
  return page;
}
const set = (page, id, v) => page.evaluate(([id, v]) => {
  const el = document.getElementById(id);
  el.value = v; el.dispatchEvent(new Event('input', { bubbles: true })); el.dispatchEvent(new Event('change', { bubbles: true }));
}, [id, v]);
const idle = page => page.waitForFunction(() => !liveState.pending, null, { timeout: 60000 });
// One Snap: the live sample's next frame; its pixels' hash and size.
async function snap(page) {
  await idle(page);
  const n = await page.evaluate(() => liveState.frames);
  await page.evaluate(() => document.getElementById('lv_snap').click());
  await page.waitForFunction(n => liveState.frames > n && !liveState.pending, n, { timeout: 60000 });
  return page.evaluate(() => {
    const f = liveState.frame; let h = 2166136261;
    for (let i = 0; i < f.length; i++) h = Math.imul(h ^ f[i], 16777619) >>> 0;
    return { h, n: f.length, sum: f.reduce((a, b) => a + b, 0), t: liveState.t, spec: movieSpec({ frames: 1 }) };
  });
}

try {
  // ---- page A: a session ----
  const A = await openPage();
  await set(A, 'mv_size', 48);
  await set(A, 'mv_read-noise', 1.7);          // instrument
  await set(A, 'mv_bf-n-medium', 1.34);        // sample
  await set(A, 'mv_exposure-ms', 40);          // measurement
  const parts = await A.evaluate(() => ['mv_read-noise', 'mv_laser-640', 'mv_bf-n-medium', 'mv_exposure-ms'].map(id => iscSettings.partOf(id.slice(3))));
  check(parts.join() === 'instrument,settings,sample,measurement', `the parts of four controls: ${parts}`);
  await A.evaluate(() => document.getElementById('lv_live').click());
  await A.waitForFunction(() => liveState.frames >= 3, null, { timeout: 120000 });
  const p640 = await A.evaluate(() => +document.getElementById('mv_laser-640').value);
  await set(A, 'mv_laser-640', +(p640 * 1.5 + 0.5).toFixed(3));   // settings, mid-session
  await A.waitForFunction(() => liveState.frames >= 6, null, { timeout: 60000 });
  await A.evaluate(() => { view.cx += 4; view.cy -= 3; requestDraw(); });   // a stage move
  await A.waitForFunction(() => liveState.frames >= 9, null, { timeout: 60000 });
  await A.evaluate(() => document.getElementById('lv_live').click());
  await idle(A);
  const logA = await A.evaluate(() => ({ runs: liveState.log.length, frames: liveState.log.reduce((a, r) => a + r.n, 0), t: liveState.t }));
  // Save through the dialog
  const bar = await A.evaluate(() => { const ids = [...document.getElementById('liveBar').children].map(e => e.id), i = ids.indexOf('setIcons');
    return { order: ids[i - 1] + ',' + ids[i + 1], icons: document.querySelectorAll('#setIcons button svg').length, first: document.getElementById('menu').firstElementChild.id }; });
  check(bar.order === 'levelSeg,menuBtn' && bar.icons === 2 && bar.first === 'menuSetup',
    `two icons between the settings level and ☰ (${bar.order}, ${bar.icons} icons), save / load the first section of ☰ (${bar.first})`);
  await A.evaluate(() => document.getElementById('setIcoSave').click());
  await A.evaluate(() => { document.getElementById('setName').value = 'session A'; });
  const [dl] = await Promise.all([A.waitForEvent('download'), A.evaluate(() => document.getElementById('setGo').click())]);
  const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'isc-settings-'));
  const fileA = path.join(tmp, dl.suggestedFilename());
  await dl.saveAs(fileA);
  const text = fs.readFileSync(fileA, 'utf8'), file = JSON.parse(text);
  const byPart = await A.evaluate(() => { const c = { instrument: [], settings: [], sample: [], measurement: [] };
    for (const [k, o] of self.ISC_ENGINE_OPTIONS) if (o.part in c) c[o.part].push(k); return c; });
  const missing = Object.entries(byPart).flatMap(([p, ks]) => ks.filter(k => !(k in file.parts[p].values)));
  const count = Object.fromEntries(Object.entries(byPart).map(([p, ks]) => [p, ks.length]));
  check(dl.suggestedFilename() === 'session A.isc.json' && file.format === 'insiliscope-settings' && !file.parts.view && !missing.length,
    `saved "${dl.suggestedFilename()}": every engine option of each part ${JSON.stringify(count)}${missing.length ? ', missing ' + missing : ''}, no view part`);
  const v = file.parts;
  check(typeof v.instrument.values['camera-preset'] === 'string' && typeof v.sample.values['mt-mode'] === 'string' &&
    typeof v.settings.values.modality === 'string' && v.instrument.values['read-noise'] === 1.7 && v.sample.values['bf-n-medium'] === 1.34 &&
    v.measurement.values['exposure-ms'] === 40, `names for enumerations (camera ${v.instrument.values['camera-preset']}, MT ${v.sample.values['mt-mode']}), the changed values kept`);
  const recs = v.measurement.history.records;
  const changed = recs.slice(1).map(r => Object.keys(r.set).join('+'));
  check(recs.length === logA.runs && recs.reduce((a, r) => a + r.n, 0) === logA.frames && recs.length >= 2 &&
    !Object.keys(recs[0].set).some(k => k === 'mt-mode' || k === 'world-seed') && recs.some(r => 'laser-640' in r.set) &&
    recs.slice(1).some(r => 'x' in r.set), `the light history: ${recs.length} runs (${recs.map(r => r.n)} frames; then ${changed.join(', ')}), ` +
    `${logA.t.toFixed(2)} s, without the sample's keys`);
  const nextA = await snap(A);

  // ---- page B: load it ----
  const B = await openPage();
  await set(B, 'mv_laser-561', 0.123);   // (overwritten by the file's settings)
  await B.evaluate(([t, n]) => iscSettings.open(t, n), [text, 'session A.isc.json']);
  await B.evaluate(() => document.getElementById('setGo').click());
  await B.waitForFunction(() => window.iscLastLoad, null, { timeout: 60000 });
  const load = await B.evaluate(() => iscLastLoad);
  const specB = await B.evaluate(() => movieSpec({ frames: 1 }));
  const specA = (await A.evaluate(() => movieSpec({ frames: 1 })));
  check(specB === specA, specB === specA ? 'loaded: the same movieSpec() as page A' : `loaded: movieSpec() differs:\n  A ${specA}\n  B ${specB}`);
  check(!load.differ.length && !load.notes.length && /replayed/.test(load.replay), `nothing left out (${load.differ.join('; ') || 'no differences'}${load.notes.length ? '; ' + load.notes.join('; ') : ''}); ${load.replay}`);
  await B.evaluate(() => document.getElementById('menuBtn').click());
  const rows = await B.waitForFunction(() => document.getElementById('menu').classList.contains('open') &&
    document.querySelectorAll('#lv_setupRows .n').length === 5 && [...document.querySelectorAll('#lv_setupRows .n')].map(e => e.textContent)).then(h => h.jsonValue());
  await B.evaluate(() => document.body.click());
  check(rows.slice(0, 4).every(r => r === 'session A'), `☰ names the file for the four parts: ${rows.join(', ')}`);
  const nextB = await snap(B);
  check(nextB.h === nextA.h && nextB.n === nextA.n && Math.abs(nextB.t - nextA.t) < 1e-9,
    `the next frame after the replayed history = page A's, bit for bit (${nextA.n} px, sum ${nextA.sum} / ${nextB.sum}, t ${nextA.t.toFixed(3)} / ${nextB.t.toFixed(3)} s)`);

  // ---- another file's instrument over it ----
  const other = JSON.parse(text);
  other.parts.instrument.name = 'other';
  other.parts.instrument.values['read-noise'] = 2.9;
  other.parts.instrument.values.gain = 0.5;
  other.parts.settings.values['laser-640'] = 9;
  other.parts.sample.values['bf-n-medium'] = 1.36;
  const laserBefore = await B.evaluate(() => +document.getElementById('mv_laser-640').value);
  await B.evaluate(() => { window.iscLastLoad = null; });
  await B.evaluate(([t]) => iscSettings.open(t, 'other.isc.json'), [JSON.stringify(other)]);
  await B.evaluate(() => { for (const i of document.querySelectorAll('#setDlg [data-part]')) i.checked = i.dataset.part === 'instrument'; document.getElementById('setGo').click(); });
  await B.waitForFunction(() => window.iscLastLoad, null, { timeout: 60000 });
  const mix = await B.evaluate(() => ({ rn: +document.getElementById('mv_read-noise').value, gain: +document.getElementById('mv_gain').value,
    laser: +document.getElementById('mv_laser-640').value, med: +document.getElementById('mv_bf-n-medium').value,
    rows: [...document.querySelectorAll('#lv_setupRows .n')].map(e => e.textContent) }));
  check(mix.rn === 2.9 && mix.gain === 0.5 && mix.laser === laserBefore && mix.med === 1.34 && mix.rows[0] === 'other' && mix.rows[2] === 'session A',
    `another file's instrument: its read noise and gain, the lasers and the sample kept ${JSON.stringify(mix)}`);

  // ---- another release, an unknown key ----
  const old = JSON.parse(text);
  old.made.release = '0.0.1-old';
  old.parts.settings.values['no-such-option'] = 3;
  await B.evaluate(() => { window.iscLastLoad = null; });
  await B.evaluate(([t]) => iscSettings.open(t, 'old.isc.json'), [JSON.stringify(old)]);
  const warn = await B.evaluate(() => [...document.querySelectorAll('#setDlg .warn')].map(e => e.textContent).join(' | '));
  await B.evaluate(() => { for (const i of document.querySelectorAll('#setDlg [data-part]')) i.checked = i.dataset.part === 'settings'; document.getElementById('setGo').click(); });
  await B.waitForFunction(() => window.iscLastLoad, null, { timeout: 60000 });
  const out = await B.evaluate(() => document.getElementById('setOut').textContent);
  check(/0\.0\.1-old/.test(warn) && /no-such-option/.test(out), `another release warned ("${warn}"), the unknown key listed ("${out.split('\n').find(l => /no-such/.test(l))}")`);

  // ---- the back-end: insiliscope_cli --settings ----
  const cli = arg('cli') || path.join(ROOT, 'build/msvc/cli/Release/insiliscope_cli.exe');
  if (fs.existsSync(cli)) {
    const tif = path.join(tmp, 'next.tif'), tc = Date.now();
    execFileSync(cli, ['--settings', fileA, '--frames', '1', '--out', tif], { stdio: 'pipe' });
    const buf = fs.readFileSync(tif), dv = new DataView(buf.buffer, buf.byteOffset), ifd = dv.getUint32(4, true), tags = {};
    for (let i = 0, n = dv.getUint16(ifd, true); i < n; i++) { const o = ifd + 2 + 12 * i; tags[dv.getUint16(o, true)] = dv.getUint32(o + 8, true); }
    const px = new Uint16Array(buf.buffer.slice(buf.byteOffset + tags[273], buf.byteOffset + tags[273] + nextA.n * 2));
    let h = 2166136261, sum = 0;
    for (let i = 0; i < px.length; i++) { h = Math.imul(h ^ px[i], 16777619) >>> 0; sum += px[i]; }
    check(h === nextA.h, `insiliscope_cli --settings: the same next frame as the viewer's (sum ${sum} / ${nextA.sum}; ${((Date.now() - tc) / 1000).toFixed(1)} s)`);
  } else console.log(`skip  insiliscope_cli --settings (no ${cli})`);
  fs.rmSync(tmp, { recursive: true, force: true });
} catch (e) {
  fail++;
  console.log(`FAIL  ${e.message.split('\n')[0]}`);
}
if (errors.length) { fail++; console.log(`FAIL  page errors: ${errors.join('; ')}`); }
clearTimeout(dog);
await browser.close();
server.close();
console.log(`viewer settings file: ${fail ? fail + ' failure(s)' : 'all ok'}, ${sec()} s`);
process.exit(fail ? 1 : 0);
