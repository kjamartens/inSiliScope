#!/usr/bin/env node
// SPDX-License-Identifier: BSD-3-Clause
// The gallery's and Home page's 3D overview (overview_3d.jpg): the viewer in headless Chromium at the gallery's spot
// (the viewer's start position), tilted, turned and in perspective with the distance haze, every cell detailed, the
// chunk grid on; then its live microscope's DNA-PAINT and widefield frames of the camera box as two insets on the floor
// under the box, tied to its near corners by dashed lines. The DNA-PAINT imagers are the gallery's (gallery/manifest.json
// defaults: a tenth of the cli's, bound 10x longer). Rendered by release.yml before tools/build_gallery.py, which puts
// it above the 2x2 overview; tools/build_site.sh shows it on Home.
//   node tools/build_hero.mjs --out site_gallery [--channel=chrome]      (~1 min with --gpu, ~4 min on SwiftShader; needs web/tools' Playwright)
import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';
import { createRequire } from 'node:module';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const require = createRequire(path.join(ROOT, 'web/tools/package.json'));
const { chromium } = require('playwright');
const arg = k => { const a = process.argv.find(s => s.startsWith('--' + k)); return a ? (a.includes('=') ? a.split('=')[1] : process.argv[process.argv.indexOf(a) + 1]) : null; };
const OUT = arg('out') || 'site_gallery';
const VIEW = { tiltDeg: 72, azimuthDeg: 20, perspDeg: 16, perspMist: 70, detailCells: 0, zoomSteps: 4 };
const man = JSON.parse(fs.readFileSync(path.join(ROOT, 'gallery/manifest.json'), 'utf8')).defaults;
const PAINT = Object.fromEntries(['mt-imager-nm', 'mt-paint-on-sec', 'npc-imager-nm', 'npc-paint-on-sec'].map(k => [k, man[k]]));
const LIVE_SEC = 15;
const types = { '.js': 'text/javascript', '.mjs': 'text/javascript', '.html': 'text/html', '.css': 'text/css', '.wasm': 'application/wasm',
  '.json': 'application/json', '.svg': 'image/svg+xml', '.png': 'image/png', '.jpg': 'image/jpeg' };
const server = http.createServer((req, res) => {
  const f = path.join(ROOT, 'web', decodeURIComponent(req.url.split('?')[0]).replace(/^\/+/, '') || 'index.html');
  if (!f.startsWith(path.join(ROOT, 'web')) || !fs.existsSync(f) || fs.statSync(f).isDirectory()) { res.statusCode = 404; res.end(); return; }
  res.setHeader('content-type', types[path.extname(f)] || 'application/octet-stream');
  res.end(fs.readFileSync(f));
}).listen(0);

// a clipped page screenshot of an element (an element screenshot waits for a "stable" page, which a view that
// keeps redrawing on software GL never is)
const shot = async (page, sel) => page.screenshot({ clip: await page.locator(sel).boundingBox(), timeout: 300000 });
const t0 = Date.now(), log = s => console.log(`${s}  [${((Date.now() - t0) / 1000).toFixed(0)} s]`);
// --gpu: the machine's GPU (else SwiftShader, as on a CI runner: slow with every cell's microtubules)
const browser = await chromium.launch({ channel: arg('channel') || undefined,
  args: process.argv.includes('--gpu') ? [] : ['--use-angle=swiftshader', '--enable-unsafe-swiftshader'] });
// 1600 x 828 CSS px at 2x: the world view's canvas is square (1588 x 1588 device px with the x-z view under it)
const ctx = await browser.newContext({ viewport: { width: 1600, height: 828 }, deviceScaleFactor: 2, colorScheme: 'dark' });
const page = await ctx.newPage();
page.on('pageerror', e => console.log('pageerror', e.message));
try {
  await page.goto(`http://localhost:${server.address().port}/index.html?live=0`);
  await page.waitForFunction(() => window.liveState && typeof genWorkers !== 'undefined' && genWorkers.ok, null, { timeout: 120000 });
  const box = await page.locator('#cv').boundingBox();
  await page.mouse.move(box.x + box.width * 0.48, box.y + box.height * 0.5);
  for (let i = 0; i < VIEW.zoomSteps; i++) { await page.mouse.wheel(0, -300); await page.waitForTimeout(400); }
  const set = vals => page.evaluate(vals => {
    for (const [id, val] of Object.entries(vals)) {
      const e = document.getElementById(id);
      if (e.type === 'checkbox') e.checked = val; else e.value = val;
      e.dispatchEvent(new Event('input', { bubbles: true })); e.dispatchEvent(new Event('change', { bubbles: true }));
    }
  }, vals);
  // First the live frames, on the light flat view (on software GL a 3D view of every cell's microtubules takes many
  // seconds per repaint, and the view repaints throughout Live); then the 3D view, once, for one screenshot.
  await set({ perspDeg: 0, ...Object.fromEntries(Object.entries(PAINT).map(([k, v]) => ['mv_' + k, v])) });
  await page.click('#lv_live');
  await page.waitForTimeout(LIVE_SEC * 1000);
  const paint = await shot(page, '#lv_cv');
  log('live DNA-PAINT frame');
  const mode = await page.$eval('select[data-pick="mode"]', e => e.value);
  await page.selectOption('select[data-pick="mode"]', { label: 'WideField' });
  await page.waitForTimeout(LIVE_SEC * 1000);
  const wf = await shot(page, '#lv_cv');
  log('live widefield frame');
  await page.click('#lv_live');
  await page.selectOption('select[data-pick="mode"]', mode);

  await set({ showGrid: true, perspCut: false, detailCells: VIEW.detailCells, tiltDeg: VIEW.tiltDeg, azimuthDeg: VIEW.azimuthDeg,
    perspDeg: VIEW.perspDeg, perspMist: VIEW.perspMist });
  // the cells delivered: no block packing, no cell asset in flight, at most 3 cells still loading, and the microtubule
  // count unchanged over 8 polls and 3 s (on software GL one repaint can block the page for seconds, so time alone
  // passes at the first poll after it); at most 4 min
  await page.waitForFunction(() => {
    const t = document.getElementById('hudText').textContent, n = +document.getElementById('hud').dataset.microtubules || 0;
    const m = /loading (\d+) cell/.exec(t);
    if (n !== window.__heroN || /packing/.test(t) || !n || (m && +m[1] > 3) || assetInflight.size) {
      window.__heroN = n; window.__heroT = performance.now(); window.__heroP = 0; return false;
    }
    return ++window.__heroP >= 8 && performance.now() - window.__heroT > 3000;
  }, null, { timeout: 240000, polling: 250 }).catch(async () => log('cells still arriving after 4 min, going on: '
    + await page.evaluate(() => document.getElementById('hudText').textContent.replace(/\s+/g, ' '))));
  const info = await page.evaluate(async () => {
    document.getElementById('hud').style.display = 'none';
    await new Promise(r => requestAnimationFrame(() => requestAnimationFrame(r)));
    // the camera of these settings (the frame drawn with them), not whichever camera ran last
    const cam = cameraFor({ pivot: [view.cx, view.cy, +els.pivotZ.value || 0], azimuthDeg: +els.azimuthDeg.value, tiltDeg: +els.tiltDeg.value,
      perspDeg: +els.perspDeg.value, perspCut: els.perspCut.checked, scale: view.scale }, cv.width, cv.height);
    const r = cameraRectUm(), cvr = cv.getBoundingClientRect(), host = document.getElementById('canvasHost').getBoundingClientRect();
    const k = devicePixelRatio, ox = (cvr.left - host.left) * k, oy = (cvr.top - host.top) * k;
    return { mt: +document.getElementById('hud').dataset.microtubules,
      pts: [[r[0], r[1]], [r[2], r[1]], [r[2], r[3]], [r[0], r[3]]].map(([x, y]) => { const [sx, sy] = cam.project(x, y, 0); return [sx + ox, sy + oy]; }) };
  });
  await page.waitForTimeout(500);
  const world = await shot(page, '#canvasHost');
  log(`world view: ${info.mt} microtubules`);

  // the composite, drawn on a canvas in the page
  const jpg = await page.evaluate(async ([world, wf, paint, pts]) => {
    const load = b => new Promise((ok, no) => { const i = new Image(); i.onload = () => ok(i); i.onerror = no; i.src = 'data:image/png;base64,' + b; });
    const [W, F, P] = await Promise.all([load(world), load(wf), load(paint)]);
    const c = document.createElement('canvas'); c.width = W.width; c.height = W.height;
    const g = c.getContext('2d'), k = W.width / 1588;   // the layout below is in px of a 1588 px wide image
    g.drawImage(W, 0, 0);
    const S = Math.round(418 * k), GAP = Math.round(24 * k), YI = Math.round(812 * k), X0 = W.width - 1 - S - GAP - S;
    const insets = [[F, 'Widefield'], [P, 'SMLM (DNA-PAINT)']].map(([im, label], i) => {
      const x = X0 + i * (S + GAP);
      g.imageSmoothingQuality = 'high'; g.drawImage(im, x, YI, S, S);
      g.strokeStyle = 'rgb(235,238,245)'; g.lineWidth = 2 * k; g.strokeRect(x - 1, YI - 1, S + 1, S + 1);
      g.font = `600 ${Math.round(26 * k)}px "Segoe UI", system-ui, sans-serif`; g.textBaseline = 'top';
      const tw = g.measureText(label).width, th = 26 * k;
      g.fillStyle = 'rgba(0,0,0,0.67)'; g.beginPath(); g.roundRect(x + 4 * k, YI + 4 * k, tw + 16 * k, th + 14 * k, 6 * k); g.fill();
      g.fillStyle = 'rgb(235,238,245)'; g.fillText(label, x + 12 * k, YI + 10 * k);
      return [x, YI, x + S, YI + S];
    });
    // the box's near edge (the two lowest corners on screen) to the insets' outer top corners
    const [nl, nr] = pts.slice().sort((a, b) => b[1] - a[1]).slice(0, 2).sort((a, b) => a[0] - b[0]);
    g.strokeStyle = 'rgba(235,238,245,0.67)'; g.lineWidth = 2 * k; g.setLineDash([12 * k, 8 * k]);
    g.beginPath(); g.moveTo(...nl); g.lineTo(insets[0][0], insets[0][1]); g.moveTo(...nr); g.lineTo(insets[1][2], insets[1][1]); g.stroke();
    return c.toDataURL('image/jpeg', 0.9).split(',')[1];
  }, [world.toString('base64'), wf.toString('base64'), paint.toString('base64'), info.pts]);
  fs.mkdirSync(OUT, { recursive: true });
  fs.writeFileSync(path.join(OUT, 'overview_3d.jpg'), Buffer.from(jpg, 'base64'));
  log(`wrote ${path.join(OUT, 'overview_3d.jpg')}`);
} finally {
  await browser.close();
  server.close();
}
