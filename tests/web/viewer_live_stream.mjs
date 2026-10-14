#!/usr/bin/env node
// SPDX-License-Identifier: BSD-3-Clause
// The live panel's "⇢ webSMLM" stream in headless Chromium: through tools/websmlm_relay.mjs, a receiver standing in for
// webSMLM gets each live frame as a 16-bit ImageJ TIFF of the frame's size, and {"cmd":"stop"} when streaming stops; the
// button is highlighted while on; the performance breakdown lists the engine's phases.
//   node tests/web/viewer_live_stream.mjs [--channel=chrome]
import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { createRequire } from 'node:module';
import { fileURLToPath } from 'node:url';

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
const port = 20000 + Math.floor(Math.random() * 20000);
const relay = spawn(process.execPath, [path.join(ROOT, 'tools/websmlm_relay.mjs'), '--port', String(port)], { stdio: ['ignore', 'pipe', 'inherit'] });
await new Promise(res => relay.stdout.once('data', res));

let fail = 0;
const t0 = Date.now(), check = (ok, what) => { if (!ok) fail++; console.log(`${ok ? 'ok  ' : 'FAIL'}  ${what}  [${((Date.now() - t0) / 1000).toFixed(0)} s]`); };
const got = [];
const rx = new WebSocket(`ws://localhost:${port}`);
rx.binaryType = 'arraybuffer';
rx.onmessage = e => { got.push(e.data); if (typeof e.data !== 'string') rx.send(JSON.stringify({ cmd: 'ack', framesReceived: got.length, ok: true })); };
await new Promise((res, rej) => { rx.onopen = res; rx.onerror = rej; });

const browser = await chromium.launch({ channel: arg('channel') || undefined, args: ['--use-angle=swiftshader', '--enable-unsafe-swiftshader'] });
const page = await browser.newPage({ viewport: { width: 1200, height: 800 } });
const errors = [];
page.on('pageerror', e => errors.push(e.message));
try {
  await page.goto(`http://localhost:${server.address().port}/index.html`);
  await page.waitForFunction(() => window.liveState && typeof genWorkers !== 'undefined' && genWorkers.ok, null, { timeout: 120000 });
  await page.evaluate(p => {
    const u = document.getElementById('lv_streamUrl');
    u.value = `ws://localhost:${p}/source`; u.dispatchEvent(new Event('change'));
    document.getElementById('lv_perf').checked = true; document.getElementById('lv_perf').dispatchEvent(new Event('change'));
    document.getElementById('mv_size').value = 64; document.getElementById('mv_size').dispatchEvent(new Event('input', { bubbles: true }));
    document.getElementById('lv_stream').click();
  }, port);
  await page.waitForFunction(() => /relay connected/.test(document.getElementById('lv_streamStatus').textContent), null, { timeout: 20000 });
  check(await page.evaluate(() => document.getElementById('lv_stream').classList.contains('on')), 'the stream button is highlighted while on');
  await page.evaluate(() => document.getElementById('lv_live').click());
  await page.waitForFunction(() => liveState.frames >= 3, null, { timeout: 180000 });
  await page.evaluate(() => document.getElementById('lv_live').click());
  const perf = await page.evaluate(() => document.getElementById('lv_perfOut').innerText);
  check(/engine \(worker\)/.test(perf) && /fluorescence, total/.test(perf) && /blinks/.test(perf), 'the performance breakdown lists the engine phases');
  await page.evaluate(() => document.getElementById('lv_stream').click());
  const until = Date.now() + 10000;
  while (!got.some(x => typeof x === 'string') && Date.now() < until) await new Promise(r => setTimeout(r, 50));
  const bins = got.filter(x => typeof x !== 'string');
  const tif = bins[0] && new DataView(bins[0]);
  const ifd = tif && tif.getUint32(4, true), tag = k => {   // the first page's tag value (SHORT or LONG)
    const n = tif.getUint16(ifd, true);
    for (let i = 0; i < n; i++) { const o = ifd + 2 + 12 * i; if (tif.getUint16(o, true) === k) return tif.getUint16(o + 2, true) === 3 ? tif.getUint16(o + 8, true) : tif.getUint32(o + 8, true); }
    return null;
  };
  const text = bins[0] ? new TextDecoder().decode(new Uint8Array(bins[0])) : '';
  check(bins.length >= 3 && tif.getUint16(0, true) === 0x4949 && tag(256) === 64 && tag(257) === 64 && tag(258) === 16 && /ImageJ=1\.11a\nimages=1/.test(text),
    `the receiver got ${bins.length} chunks, each a 64 x 64 16-bit ImageJ TIFF`);
  check(got[got.length - 1] === '{"cmd":"stop"}', 'stopping sends {"cmd":"stop"}');
  check(!errors.length, 'no page errors' + (errors.length ? ': ' + errors.join('; ') : ''));
} catch (e) { check(false, 'error: ' + e.message); }
await browser.close();
rx.close(); relay.kill(); server.close();
console.log(`${fail ? 'FAIL' : 'ok'}: viewer live stream (${((Date.now() - t0) / 1000).toFixed(0)} s)`);
process.exit(fail ? 1 : 0);
