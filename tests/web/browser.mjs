// SPDX-License-Identifier: BSD-3-Clause
// The browser checks' common part: a static server over web/, headless Chromium (the hardware GPU; --swiftshader: the
// software one, ~10x slower: a first frame there costs ~40 s of shader work), a page
// with its errors collected, timed checks, a watchdog (a hung page fails with the last check that ended instead of
// running into CI's limit), and the browser and server closed in every case.
//   import { runViewer } from './browser.mjs';
//   await runViewer('viewer scene', { watchdogSec: 120 }, async ({ page, check, port }) => { ... });
// Exits 0 when every check passed, 1 on a failed check or a page error, 2 on the watchdog or an exception.
import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';
import { createRequire } from 'node:module';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const require = createRequire(path.join(ROOT, 'web/tools/package.json'));
const arg = k => { const a = process.argv.find(s => s.startsWith('--' + k)); return a ? (a.split('=')[1] || true) : null; };

export async function runViewer(name, { watchdogSec = 120, viewport = { width: 1000, height: 700 }, query = '?live=0',
  waitCells = true } = {}, body) {
  let chromium;
  try { ({ chromium } = require('playwright')); }
  catch { ({ chromium } = await import('/opt/node22/lib/node_modules/playwright/index.mjs')); }
  const server = http.createServer((req, res) => {
    const f = path.join(ROOT, 'web', decodeURIComponent(req.url.split('?')[0]).replace(/^\/+/, '') || 'index.html');
    if (!f.startsWith(path.join(ROOT, 'web')) || !fs.existsSync(f) || fs.statSync(f).isDirectory()) { res.statusCode = 404; res.end(); return; }
    res.setHeader('content-type', f.endsWith('.js') ? 'text/javascript' : 'text/html');
    res.end(fs.readFileSync(f));
  }).listen(0);
  const t0 = Date.now(), sec = () => ((Date.now() - t0) / 1000).toFixed(1);
  let fail = 0, last = 'start', browser = null;
  const check = (ok, what) => { if (!ok) fail++; last = what; console.log(`${ok ? 'ok  ' : 'FAIL'}  ${what}  [${sec()} s]`); };
  const dog = setTimeout(() => {
    console.log(`FAIL  ${name}: no end after ${watchdogSec} s (the last check that ended: ${last})`);
    process.exit(2);
  }, watchdogSec * 1000);
  let code = 0;
  try {
    browser = await chromium.launch({ channel: arg('channel') || undefined,
      args: arg('swiftshader') ? ['--use-angle=swiftshader', '--enable-unsafe-swiftshader'] : ['--enable-gpu', '--ignore-gpu-blocklist'] });
    const page = await browser.newPage({ viewport });
    const errors = [];
    page.on('pageerror', e => errors.push(e.message));
    const port = server.address().port;
    await page.goto(`http://localhost:${port}/index.html${query}`);
    if (waitCells)
      await page.waitForFunction(() => window.iscScene && +document.getElementById('hud').dataset.cells > 0 && iscScene.packingIdle(),
        null, { timeout: watchdogSec * 1000 });
    check(true, `${name}: the viewer is up`);
    await body({ page, check, port });
    if (errors.length) { fail++; console.log(`FAIL  page errors: ${errors.join('; ')}`); }
    code = fail ? 1 : 0;
  } catch (e) {
    console.log(`FAIL  ${name}: ${e.message.split('\n')[0]} (the last check that ended: ${last})`);
    code = 2;
  } finally {
    clearTimeout(dog);
    if (browser) await browser.close().catch(() => {});
    server.close();
  }
  console.log(`${name}: ${code ? 'FAILED' : 'PASS'} (${sec()} s)`);
  process.exit(code);
}

// In the page: a state for the centre cell, a capture that waits until everything is in (__st, __cap, __key).
export async function sceneHelpers(page) {
  await page.evaluate(() => {
    window.__key = iscScene.nearestCell(view.cx, view.cy);
    window.__st = o => Object.assign({ camera: { pivot: [0, 0, 0], azimuthDeg: 0, tiltDeg: 0, fovUm: 60 }, scope: 'target', target: __key,
      detailCells: 1, theme: 'fluo', zClip: null, sweep: null, layers: {}, grid: false, xz: false, ui: false }, o);
    window.__cap = async (st, W = 400, H = 300) => {
      for (let i = 0; i < 300; i++) {
        const r = iscScene.captureFrame(st, { width: W, height: H, pxScale: 1 });
        if (!r.info.loading && !r.info.dyes && !r.info.data.length) return { data: r.canvas.getContext('2d').getImageData(0, 0, W, H).data, cam: r.cam };
        await new Promise(res => setTimeout(res, 100));
      }
      throw new Error('frame never complete');
    };
  });
}
// The main view nearly empty (its layers and the x-z view off): a readPixels also waits for the page's own redraws.
export async function quietMainView(page) {
  await page.evaluate(() => {
    for (const id of ['showCyto', 'showNucleus', 'showMt', 'showXZ']) { els[id].checked = false; els[id].dispatchEvent(new Event('input', { bubbles: true })); }
  });
}
