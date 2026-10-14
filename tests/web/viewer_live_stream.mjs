#!/usr/bin/env node
// SPDX-License-Identifier: BSD-3-Clause
// The live panel's "⇢ webSMLM" stream in headless Chromium: the viewer and the site's copy of webSMLM
// (web/websmlm/webSMLM.html, tools/fetch_websmlm.mjs; fetched here when missing) open in two tabs; the button finds the
// webSMLM tab over the BroadcastChannel (live/websmlm_bridge.js), webSMLM localizes the live frames in its live-stream
// session with the viewer's camera (pixel size, gain, offset), and stopping ends the session; the button is highlighted
// while on; the performance breakdown lists the engine's phases.
//   node tests/web/viewer_live_stream.mjs [--channel=chrome]
import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';
import { execFileSync } from 'node:child_process';
import { createRequire } from 'node:module';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const require = createRequire(path.join(ROOT, 'web/tools/package.json'));
let chromium;
try { ({ chromium } = require('playwright')); }
catch { ({ chromium } = await import('/opt/node22/lib/node_modules/playwright/index.mjs')); }
const arg = k => { const a = process.argv.find(s => s.startsWith('--' + k)); return a ? (a.split('=')[1] || true) : null; };
if (!fs.existsSync(path.join(ROOT, 'web/websmlm/webSMLM.html')))
  execFileSync(process.execPath, [path.join(ROOT, 'tools/fetch_websmlm.mjs')], { stdio: 'inherit' });
const server = http.createServer((req, res) => {
  const f = path.join(ROOT, 'web', decodeURIComponent(req.url.split('?')[0]).replace(/^\/+/, '') || 'index.html');
  if (!f.startsWith(path.join(ROOT, 'web')) || !fs.existsSync(f) || fs.statSync(f).isDirectory()) { res.statusCode = 404; res.end(); return; }
  res.setHeader('content-type', f.endsWith('.js') ? 'text/javascript' : 'text/html');
  res.end(fs.readFileSync(f));
}).listen(0);
const base = `http://localhost:${server.address().port}`;

let fail = 0;
const t0 = Date.now(), check = (ok, what) => { if (!ok) fail++; console.log(`${ok ? 'ok  ' : 'FAIL'}  ${what}  [${((Date.now() - t0) / 1000).toFixed(0)} s]`); };
const browser = await chromium.launch({ channel: arg('channel') || undefined, args: ['--use-angle=swiftshader', '--enable-unsafe-swiftshader'] });
const ctx = await browser.newContext({ viewport: { width: 1200, height: 800 } });
const page = await ctx.newPage(), wsm = await ctx.newPage();
const errors = [];
page.on('pageerror', e => errors.push('viewer: ' + e.message));
try {
  await page.goto(`${base}/index.html`);
  await page.waitForFunction(() => window.liveState && typeof genWorkers !== 'undefined' && genWorkers.ok, null, { timeout: 120000 });
  await page.evaluate(() => {
    document.getElementById('lv_perf').checked = true; document.getElementById('lv_perf').dispatchEvent(new Event('change'));
    document.getElementById('mv_size').value = 64; document.getElementById('mv_size').dispatchEvent(new Event('input', { bubbles: true }));
    document.getElementById('lv_stream').click();
  });
  check(await page.evaluate(() => document.getElementById('lv_stream').classList.contains('on')), 'the stream button is highlighted while on');
  const link = await page.evaluate(() => (document.querySelector('#lv_streamStatus a') || {}).href);
  check(/\/websmlm\/webSMLM\.html$/.test(link || ''), 'with no webSMLM tab, the status links the site\'s webSMLM');
  await wsm.goto(link);   // the user opens webSMLM in another tab; the viewer finds it by itself
  await wsm.waitForFunction(() => window.webSMLM && window.webSMLM.liveStream, null, { timeout: 60000 });
  await page.waitForFunction(() => /to webSMLM v/.test(document.getElementById('lv_streamStatus').textContent), null, { timeout: 20000 });
  check(true, 'the viewer found the webSMLM tab');
  await page.evaluate(() => document.getElementById('lv_live').click());
  await wsm.waitForFunction(() => /Streaming — ([3-9]|\d\d+) frames/.test(document.getElementById('liveStreamStatus').textContent), null, { timeout: 180000 });
  await page.evaluate(() => document.getElementById('lv_live').click());
  const px = await page.evaluate(() => +document.getElementById('mv_pixel-nm').value);
  const w = await wsm.evaluate(() => ({ active: webSMLM.liveStream.isActive(), status: document.getElementById('liveStreamStatus').textContent,
    pxnm: +document.getElementById('pxnm').value, gain: +document.getElementById('gain').value, off: +document.getElementById('camoffset').value }));
  const cam = await page.evaluate(() => ({ gain: +document.getElementById('mv_gain').value, off: +document.getElementById('mv_offset').value }));
  check(w.active, `webSMLM's live session runs: "${w.status}"`);
  check(Math.abs(w.pxnm - px) < 1e-6 && Math.abs(w.gain - cam.gain) < 1e-9 && w.off === cam.off, `webSMLM has the viewer's camera: ${w.pxnm} nm, ${w.gain} e-/ADU, offset ${w.off}`);
  await page.waitForFunction(() => /localized there/.test(document.getElementById('lv_streamStatus').textContent) && !/ 0 localized/.test(document.getElementById('lv_streamStatus').textContent), null, { timeout: 60000 });
  check(true, 'the viewer shows webSMLM\'s count: ' + await page.evaluate(() => document.getElementById('lv_streamStatus').textContent));
  const perf = await page.evaluate(() => document.getElementById('lv_perfOut').innerText);
  check(/engine \(worker\)/.test(perf) && /fluorescence, total/.test(perf) && /blinks/.test(perf), 'the performance breakdown lists the engine phases');
  await page.evaluate(() => document.getElementById('lv_stream').click());
  await wsm.waitForFunction(() => !webSMLM.liveStream.isActive(), null, { timeout: 30000 });
  check(true, 'stopping ends webSMLM\'s session');
  check(!errors.length, 'no page errors' + (errors.length ? ': ' + errors.join('; ') : ''));
} catch (e) { check(false, 'error: ' + e.message); }
await browser.close();
server.close();
console.log(`${fail ? 'FAIL' : 'ok'}: viewer live stream (${((Date.now() - t0) / 1000).toFixed(0)} s)`);
process.exit(fail ? 1 : 0);
