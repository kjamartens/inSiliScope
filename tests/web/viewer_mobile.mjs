#!/usr/bin/env node
// SPDX-License-Identifier: BSD-3-Clause
// The viewer on a phone (web/live/mobile.js) in headless Chromium with touch: the layout (no sideways scroll, the
// square stage at full width, the world view as the inset in its top-right corner), Live starting by itself, the focus knob (a swipe and
// − / +), two fingers on the live image (up = focus up, a spread = zoom), the inset swap (kept over a reload), two
// fingers on the world view (pinch = zoom, both up = tilt), and a wide window putting every node back.
//   node tests/web/viewer_mobile.mjs [--channel=chrome] [--swiftshader]   (the hardware GPU; --swiftshader: software GL, ~10x slower)
import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';
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

let fail = 0;
const t0 = Date.now(), check = (ok, what) => { if (!ok) fail++; console.log(`${ok ? 'ok  ' : 'FAIL'}  ${what}  [${((Date.now() - t0) / 1000).toFixed(0)} s]`); };
const sleep = ms => new Promise(r => setTimeout(r, ms));
setTimeout(() => { console.log('FAIL  timed out after 240 s'); process.exit(1); }, 240000).unref();
const browser = await chromium.launch({ channel: arg('channel') || undefined, args: arg('swiftshader') ? ['--use-angle=swiftshader', '--enable-unsafe-swiftshader'] : ['--enable-gpu', '--ignore-gpu-blocklist'] });
const ctx = await browser.newContext({ viewport: { width: 390, height: 844 }, isMobile: true, hasTouch: true, deviceScaleFactor: 1 });
const page = await ctx.newPage();
const cdp = await ctx.newCDPSession(page);
const errors = [];
page.on('pageerror', e => errors.push(e.message));
// A touch gesture: each finger from its start to its end point in `steps` moves, `ms` apart.
async function touch(fingers, steps = 10, ms = 40) {
  const at = s => fingers.map((f, id) => ({ id, x: f[0] + (f[2] - f[0]) * s, y: f[1] + (f[3] - f[1]) * s }));
  await cdp.send('Input.dispatchTouchEvent', { type: 'touchStart', touchPoints: at(0) });
  for (let i = 1; i <= steps; i++) { await sleep(ms); await cdp.send('Input.dispatchTouchEvent', { type: 'touchMove', touchPoints: at(i / steps) }); }
  await cdp.send('Input.dispatchTouchEvent', { type: 'touchEnd', touchPoints: [] });
  await sleep(100);
}
// a tap at an element's centre (no move, released at once: a held − / + would repeat)
async function tap(sel) {
  const r = await rect(sel), p = [{ id: 0, x: r.x + r.w / 2, y: r.y + r.h / 2 }];
  await cdp.send('Input.dispatchTouchEvent', { type: 'touchStart', touchPoints: p });
  await cdp.send('Input.dispatchTouchEvent', { type: 'touchEnd', touchPoints: [] });
  await sleep(150);
}
const rect = sel => page.evaluate(s => { const r = document.querySelector(s).getBoundingClientRect(); return { x: r.left, y: r.top, w: r.width, h: r.height }; }, sel);
const z = () => page.evaluate(() => +document.getElementById('mv_z').value);
const ready = () => page.waitForFunction(() => window.liveState && typeof genWorkers !== 'undefined' && genWorkers.ok, null, { timeout: 120000 });
try {
  await page.goto(`http://localhost:${server.address().port}/index.html`);
  await ready();
  const lay = await page.evaluate(() => {
    const s = document.getElementById('mStage').getBoundingClientRect(), h = document.getElementById('canvasHost').getBoundingClientRect();
    return { mobile: document.body.classList.contains('mobile'), sw: document.documentElement.scrollWidth, iw: innerWidth,
      s: [s.left, s.width, s.height], h: [h.right, h.top - s.top, h.width / s.width] };
  });
  check(lay.mobile && lay.sw <= lay.iw && lay.s[0] === 0 && Math.abs(lay.s[1] - lay.iw) < 1 && Math.abs(lay.s[1] - lay.s[2]) < 1
    && Math.abs(lay.h[0] - (lay.s[1] - 6)) < 2 && lay.h[1] < 10 && Math.abs(lay.h[2] - 0.375) < 0.02,
    `layout: body.mobile, no sideways scroll (${lay.sw}/${lay.iw} px), the stage square at full width, the world view the top-right inset ${JSON.stringify(lay)}`);

  await page.evaluate(() => { document.getElementById('mv_size').value = 64; document.getElementById('mv_size').dispatchEvent(new Event('input', { bubbles: true })); });
  await page.waitForFunction(() => liveState.on && liveState.frames >= 2, null, { timeout: 120000 });
  await tap('#lv_live');   // (stopped: touches wait on busy frames)
  await page.waitForFunction(() => !liveState.on && !liveState.pending, null, { timeout: 60000 });
  check(true, 'Live starts by itself, a tap on Stop stops it');

  // the focus knob (in the Focus box): a slow swipe 60 px to the left = 5 steps up; then − once
  const k = await rect('#mKnob'), z0 = await z();
  await touch([[k.x + k.w * 0.7, k.y + k.h / 2, k.x + k.w * 0.7 - 60, k.y + k.h / 2]], 12, 60);
  const z1 = await z();
  await tap('#mFocus button[data-dir="-1"]');
  const z2 = await z();
  check(Math.abs(z1 - z0 - 0.5) < 1e-6 && Math.abs(z2 - z1 + 0.1) < 1e-6, `the focus knob: a 60 px swipe left ${z0} -> ${z1} µm (5 steps up), − -> ${z2} µm`);

  // two fingers on the live image: both 64 px up = 4 steps up; spread apart = zoom in
  const v = await rect('#liveView'), cx = v.x + v.w / 2, cy = v.y + v.h / 2;
  await touch([[cx - 40, cy + 40, cx - 40, cy - 24], [cx + 40, cy + 40, cx + 40, cy - 24]], 16, 40);
  const z3 = await z();
  await touch([[cx - 20, cy, cx - 90, cy], [cx + 20, cy, cx + 90, cy]], 10, 40);
  const zoom = await page.evaluate(() => liveState.zoom.z);
  check(Math.abs(z3 - z2 - 0.4) < 1e-6 && zoom > 2, `two fingers on the live image: up 64 px ${z2} -> ${z3} µm, a spread zooms ${zoom.toFixed(2)}x`);

  // the inset: a tap swaps; the world view big then pinches and tilts
  await tap('#mSwap');
  await sleep(300);
  const sw = await page.evaluate(() => { const h = document.getElementById('canvasHost').getBoundingClientRect(), l = document.getElementById('liveView').getBoundingClientRect();
    return { big: document.getElementById('mStage').classList.contains('worldBig'), hw: h.width, lw: l.width, saved: localStorage.getItem('isc.ui.mobileBig') }; });
  check(sw.big && sw.hw > 2.5 * sw.lw && sw.saved === 'world', `a tap on the inset swaps: the world view ${sw.hw.toFixed(0)} px, the live image ${sw.lw.toFixed(0)} px`);
  const w = await rect('#cv'), wx = w.x + w.w / 2, wy = w.y + w.h / 2;
  // (without the microtubule and pore layers: a tilted or zoomed-in frame of them takes seconds under SwiftShader)
  await page.evaluate(() => { for (const id of ['showMt', 'showNpcs']) if (els[id] && els[id].checked) els[id].click(); });
  const t0d = await page.evaluate(() => +els.tiltDeg.value);
  await touch([[wx - 50, wy + 30, wx - 50, wy - 30], [wx + 50, wy + 30, wx + 50, wy - 30]], 6, 40);
  const t1d = await page.evaluate(() => +els.tiltDeg.value), s0 = await page.evaluate(() => view.scale);
  await touch([[wx - 20, wy - 20, wx - 70, wy - 70], [wx + 20, wy + 20, wx + 70, wy + 70]], 5, 40);
  const s1 = await page.evaluate(() => view.scale);
  check(s1 > s0 * 2 && Math.abs(t1d - t0d) > 10, `two fingers on the world view: a pinch zooms ${s0.toFixed(2)} -> ${s1.toFixed(2)}, both up tilt ${t0d} -> ${t1d}°`);

  // the position in the sample (moved here) and the zoom (the pinch's) are kept over a reload, like the settings
  const pos0 = await page.evaluate(async () => { view.cx += 37.25; view.cy -= 12.5; requestDraw(); await new Promise(r => setTimeout(r, 300));
    return [view.cx, view.cy, view.scale]; });
  await page.reload({ waitUntil: 'domcontentloaded', timeout: 120000 });
  await ready();
  check(await page.evaluate(() => document.getElementById('mStage').classList.contains('worldBig')), 'the world view stays big over a reload');
  await page.waitForFunction(() => viewPosReady, null, { timeout: 30000 });   // (after the controls are restored)
  const pos1 = await page.evaluate(() => [view.cx, view.cy, view.scale]);
  check(pos1.every((v, i) => Math.abs(v - pos0[i]) < 1e-3 * Math.max(1, Math.abs(pos0[i]))),
    `the position and zoom are kept over a reload: (${pos0.map(v => v.toFixed(2))}) -> (${pos1.map(v => v.toFixed(2))})`);

  // a wide window: the side-by-side layout again, every node in its place
  await page.setViewportSize({ width: 1200, height: 800 });
  await sleep(500);
  const back = await page.evaluate(() => { const v = document.getElementById('liveView').getBoundingClientRect();
    return { mobile: document.body.classList.contains('mobile'), top: !!document.getElementById('mTop'),
      host: document.getElementById('canvasHost').parentElement.id, live: document.getElementById('liveView').parentElement.id,
      bar: document.getElementById('liveBar').parentElement.id, barFirst: document.getElementById('side').firstElementChild.id, sq: Math.abs(v.width - v.height) < 1 && v.width > 200 }; });
  check(!back.mobile && !back.top && back.host === 'worldCol' && back.live === 'sideMain' && back.bar === 'side' && back.barFirst === 'liveBar' && back.sq,
    `a wide window puts every node back ${JSON.stringify(back)}`);
  check(!errors.length, 'no page errors' + (errors.length ? ': ' + errors.slice(0, 3).join(' | ') : ''));
} catch (e) { fail++; console.log('FAIL ', e.message); }
await browser.close(); server.close();
console.log(fail ? `${fail} FAILED` : 'all ok', `(${((Date.now() - t0) / 1000).toFixed(0)} s)`);
process.exit(fail ? 1 : 0);
