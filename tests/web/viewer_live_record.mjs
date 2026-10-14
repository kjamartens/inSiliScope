#!/usr/bin/env node
// SPDX-License-Identifier: BSD-3-Clause
// The live panel in headless Chromium: the performance breakdown has a fixed set of rows (no row comes and goes from
// frame to frame) with one decimal per time; ● Record saves N frames as one 16-bit ImageJ TIFF stack (the pixel size
// as XResolution, the frame interval and every setting in the header): by default a fresh sample (a movie; the live
// sample's time stays), with "continue the live sample" the live sample's time runs on by N frames.
//   node tests/web/viewer_live_record.mjs [--channel=chrome]
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

// The pages of a little-endian TIFF: [{w, h, bits, desc, xres}] (xres = pixels per unit), and the first page's pixels.
function readTiff(buf) {
  const dv = new DataView(buf.buffer, buf.byteOffset, buf.byteLength), pages = [];
  let ifd = dv.getUint32(4, true);
  while (ifd) {
    const n = dv.getUint16(ifd, true), t = {};
    for (let i = 0; i < n; i++) {
      const o = ifd + 2 + 12 * i, tag = dv.getUint16(o, true), type = dv.getUint16(o + 2, true), cnt = dv.getUint32(o + 4, true);
      t[tag] = type === 3 ? dv.getUint16(o + 8, true) : type === 5 ? (() => { const at = dv.getUint32(o + 8, true); return dv.getUint32(at, true) / dv.getUint32(at + 4, true); })()
        : type === 2 ? new TextDecoder().decode(buf.subarray(dv.getUint32(o + 8, true), dv.getUint32(o + 8, true) + cnt - 1)) : dv.getUint32(o + 8, true);
    }
    pages.push({ w: t[256], h: t[257], bits: t[258], desc: t[270], xres: t[282], data: t[273] });
    ifd = dv.getUint32(ifd + 2 + 12 * n, true);
  }
  return pages;
}

let fail = 0;
const t0 = Date.now(), check = (ok, what) => { if (!ok) fail++; console.log(`${ok ? 'ok  ' : 'FAIL'}  ${what}  [${((Date.now() - t0) / 1000).toFixed(0)} s]`); };
const browser = await chromium.launch({ channel: arg('channel') || undefined, args: ['--use-angle=swiftshader', '--enable-unsafe-swiftshader'] });
const ctx = await browser.newContext({ viewport: { width: 1200, height: 800 }, acceptDownloads: true });
const page = await ctx.newPage();
const errors = [];
page.on('pageerror', e => errors.push(e.message));
try {
  await page.goto(`http://localhost:${server.address().port}/index.html`);
  await page.waitForFunction(() => window.liveState && typeof genWorkers !== 'undefined' && genWorkers.ok, null, { timeout: 120000 });
  await page.evaluate(() => {
    document.getElementById('lv_perf').checked = true; document.getElementById('lv_perf').dispatchEvent(new Event('change'));
    document.getElementById('mv_size').value = 64; document.getElementById('mv_size').dispatchEvent(new Event('input', { bubbles: true }));
    document.getElementById('lv_live').click();
  });
  // the performance rows of a few frames: the same names in the same order every frame
  const seen = [];
  const liveMsAll = [];
  for (let k = 0; k < 6; k++) {
    await page.waitForFunction(n => liveState.frames >= n, 3 + k, { timeout: 180000 });
    liveMsAll.push(await page.evaluate(() => liveState.ms));
    seen.push(await page.evaluate(() => [...document.querySelectorAll('#lv_perfOut span:nth-child(2n+1)')].map(e => e.textContent).join('|') + '#' +
      [...document.querySelectorAll('#lv_perfOut span:nth-child(2n)')].map(e => e.textContent).join('|')));
  }
  await page.evaluate(() => document.getElementById('lv_live').click());
  const names = seen.map(s => s.split('#')[0]), vals = seen.map(s => s.split('#')[1].split('|'));
  check(names.every(n => n === names[0]) && /blink events \(core\)/.test(names[0]) && /BrightField image/.test(names[0]),
    `the performance rows stay put: ${names[0].split('|').length} rows in every frame`);
  check(vals.every(v => v.every(x => x === '—' || /^\d+\.\d ms$/.test(x))), 'every time has one decimal (or "—"): ' + vals[5].slice(0, 4).join(', '));

  // Record 40 frames at 128 x 128 (at 64 x 64 a frame is cheap either way)
  await page.evaluate(() => { const s = document.getElementById('mv_size'); s.value = 128; s.dispatchEvent(new Event('input', { bubbles: true })); });
  // the engine's time per frame (the worker's call; headless Chromium sometimes holds a worker's result back for
  // seconds, so the wall clock says little here)
  const snapMs = [];
  for (let k = 0; k < 6; k++) snapMs.push(await page.evaluate(() => new Promise(r => {
    const f = liveState.frames;
    document.getElementById('lv_snap').click();
    const t = setInterval(() => { if (liveState.frames > f && !liveState.pending) { clearInterval(t); r(liveState.perf.workerMs); } }, 5);
  })));
  const liveMs128 = snapMs.slice(2).sort((a, b) => a - b)[1];   // the lower median of four
  // one recording of 40 frames: [download, its TIFF pages, the buffer, the page's state after it]
  const record = async () => {
    const tRec = Date.now();
    // ● Record opens the box, its Start records
    const shown = await page.evaluate(() => { document.getElementById('lv_rec').click(); return !document.getElementById('lv_recPop').hidden; });
    const [dl] = await Promise.all([page.waitForEvent('download', { timeout: 240000 }), page.evaluate(() => document.getElementById('lv_recGo').click())]);
    const hiddenAfter = await page.evaluate(() => document.getElementById('lv_recPop').hidden);
    const recSec = (Date.now() - tRec) / 1000;
    const buf = fs.readFileSync(await dl.path()), pages = readTiff(buf);
    const info = await page.evaluate(() => ({ t: liveState.t, text: document.getElementById('lv_info').textContent, btn: document.getElementById('lv_rec').textContent,
      px: +document.getElementById('mv_pixel-nm').value, exp: +document.getElementById('mv_exposure-ms').value }));
    check(shown && hiddenAfter, 'Record opens the frames box, Start closes it and records');
    return { dl, pages, buf, info, recSec };
  };
  const frameOf = (r, k) => new Uint16Array(r.buf.buffer.slice(r.buf.byteOffset + r.pages[k].data, r.buf.byteOffset + r.pages[k].data + 128 * 128 * 2));
  const fileChecks = (r, kind) => {
    const d0 = r.pages[0] || {};
    check(r.dl.suggestedFilename() === 'insiliscope_live_128x128x40.tif' && r.pages.length === 40 && r.pages.every(p => p.w === 128 && p.h === 128 && p.bits === 16),
      `${kind}: ${r.dl.suggestedFilename()}: ${r.pages.length} pages of ${d0.w}×${d0.h}, 16 bit (${(r.buf.length / 1e6).toFixed(2)} MB)`);
    check(/^ImageJ=1\.11a\nimages=40\nframes=40\nunit=micron\nfinterval=0\.05\n/.test(d0.desc || '') && /\ninsiliscope\.spec=\S/.test(d0.desc) &&
      / exposure-ms=50( |$)/.test((/insiliscope\.spec=(.*)/.exec(d0.desc) || [])[1] || ''),
      `${kind}: the header: ImageJ frames, unit, frame interval and the full engine spec`);
    check(Math.abs(d0.xres - 1000 / r.info.px) < 1e-4, `${kind}: the pixel size: ${(1000 / d0.xres).toFixed(2)} nm (viewer ${r.info.px.toFixed(2)} nm)`);
    check(frameOf(r, 0).some((v, i) => v !== frameOf(r, 39)[i]), `${kind}: the frames differ (blinks, noise)`);
    check(r.info.btn === '● Record' && /saved insiliscope_live_128x128x40\.tif/.test(r.info.text), `${kind}: the button is back, the info line names the file`);
    return d0;
  };
  await page.evaluate(() => { const n = document.getElementById('lv_recN'); n.value = 40; n.dispatchEvent(new Event('change')); });

  // the default: a fresh sample (a movie from the Start time); the live sample is left as it was
  const tBefore = await page.evaluate(() => new Promise(r => setTimeout(() => r(liveState.t), 300)));
  const fresh = await record();
  const df = fileChecks(fresh, 'fresh');
  check(/\ninsiliscope=recording\n/.test(df.desc || '') && / frames=40( |$)/.test((/insiliscope\.spec=(.*)/.exec(df.desc) || [])[1] || ''),
    'fresh: the header says a fresh recording, its spec is the 40-frame movie');
  check(fresh.info.t === tBefore, `fresh: the live sample's time stays: ${tBefore.toFixed(2)} -> ${fresh.info.t.toFixed(2)} s`);
  console.log(`      fresh record ${fresh.recSec.toFixed(1)} s for 40 frames (wall)`);

  // "continue the live sample": the frames go on from the live sample
  await page.evaluate(() => { const c = document.getElementById('lv_recLive'); c.checked = true; c.dispatchEvent(new Event('change')); });
  const t2 = await page.evaluate(() => liveState.t);
  const cont = await record();
  const dc = fileChecks(cont, 'continue');
  check(/\ninsiliscope=live recording\n/.test(dc.desc || ''), 'continue: the header says a live recording');
  check(Math.abs(cont.info.t - (t2 + 40 * cont.info.exp / 1000)) < 1e-6, `continue: the sample's time runs on by 40 frames: ${t2.toFixed(2)} -> ${cont.info.t.toFixed(2)} s`);
  const recMs = await page.evaluate(() => liveState.rec.engineMs / 40);
  console.log(`      continue record ${cont.recSec.toFixed(1)} s for 40 frames (wall); engine ${recMs.toFixed(1)} ms/frame, a snap's ${liveMs128.toFixed(1)} ms; info: ${cont.info.text.split('\n').pop()}`);
  // Not a check: a frame's cost depends on the light the place has had (the core's blink-event query), so a snap and a
  // recording at different moments are not comparable; LiveScope::Frames' speed is measured in ctest live_scope.
  check(!errors.length, 'no page errors' + (errors.length ? ': ' + errors.join('; ') : ''));
} catch (e) { check(false, 'error: ' + e.message); }
await browser.close();
server.close();
console.log(`${fail ? 'FAIL' : 'ok'}: viewer live record (${((Date.now() - t0) / 1000).toFixed(0)} s)`);
process.exit(fail ? 1 : 0);
