#!/usr/bin/env node
// SPDX-License-Identifier: BSD-3-Clause
// The viewer's renderer in headless Chromium (web/index.html renderFrame / captureFrame): clip intervals cut exactly
// at their heights (tilt 90: lit rows only inside each band, each band in its own colour), a turned view is the
// unturned one rotated, a frame is pixel-identical when captured twice, the detail budget keeps N cells detailed, and
// a data layer's z-stack plane equals a stand-alone movie job with the same settings, and an opaque data slice hides
// only what lies behind it.
//   node tests/web/viewer_scene.mjs [--channel=chrome] [--gpu]
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
const browser = await chromium.launch({ channel: arg('channel') || undefined,
  args: arg('gpu') ? ['--enable-gpu', '--ignore-gpu-blocklist'] : ['--use-angle=swiftshader', '--enable-unsafe-swiftshader'] });
const page = await browser.newPage({ viewport: { width: 1000, height: 700 } });
const errors = [];
page.on('pageerror', e => errors.push(e.message));
let fail = 0;
const check = (ok, what) => { if (!ok) fail++; console.log(`${ok ? 'ok  ' : 'FAIL'}  ${what}`); };
await page.goto(`http://localhost:${server.address().port}/index.html`);
await page.waitForFunction(() => window.iscScene && /cells [1-9]/.test(document.getElementById('hud').textContent) && iscScene.packingIdle(), null, { timeout: 300000 });

// helpers in the page: a state for the centre cell, a capture that waits until everything is in, lit-pixel masks
await page.evaluate(() => {
  window.__key = iscScene.nearestCell(view.cx, view.cy);
  window.__st = o => Object.assign({ camera: { pivot: [0, 0, 0], azimuthDeg: 0, tiltDeg: 0, fovUm: 60 }, scope: 'target', target: __key, detailCells: 1,
    theme: 'fluo', zClip: null, sweep: null, layers: {}, grid: false, xz: false, ui: false }, o);
  window.__cap = async (st, W = 400, H = 300) => {
    for (let i = 0; i < 300; i++) {
      const r = iscScene.captureFrame(st, { width: W, height: H, pxScale: 1 });
      if (!r.info.loading && !r.info.dyes && !r.info.data.length) return { data: r.canvas.getContext('2d').getImageData(0, 0, W, H).data, cam: r.cam };
      await new Promise(res => setTimeout(res, 100));
    }
    throw new Error('frame never complete');
  };
});
// 1. clip bands at tilt 90: lit rows only inside [z0, z1], two intervals in their own colours
const bands = await page.evaluate(async () => {
  const b = iscScene.cellBounds(__key), pv = [b.center[0], b.center[1], 0];
  const st = __st({ camera: { pivot: pv, azimuthDeg: 0, tiltDeg: 90, fovUm: b.diam * 1.2 }, sweep: { axis: 'z' },
    layers: { 'mt.lines': { intervals: [{ lo: 0.6, hi: 1.0, style: { color: '#ff0000', opacity: 1 } }, { lo: 1.6, hi: 2.0, style: { color: '#00ff00', opacity: 1 } }] } } });
  const { data, cam } = await __cap(st);
  const W = 400, H = 300, rowOf = z => cam.H2 - (z - cam.pz) * cam.S;   // tilt 90: screen y = H/2 - (z - pz) S
  const tol = 3 + cam.S * 0.05, out = { red: 0, green: 0, redOut: 0, greenOut: 0 };
  for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) {
    const i = 4 * (y * W + x), r = data[i], g = data[i + 1];
    if (r > 120 && g < 60) { out.red++; if (y < rowOf(1.0) - tol || y > rowOf(0.6) + tol) out.redOut++; }
    if (g > 120 && r < 60) { out.green++; if (y < rowOf(2.0) - tol || y > rowOf(1.6) + tol) out.greenOut++; }
  }
  return out;
});
check(bands.red > 50 && bands.green > 50 && bands.redOut === 0 && bands.greenOut === 0,
  `clip: two intervals at tilt 90 lit only inside their bands (red ${bands.red} px, ${bands.redOut} outside; green ${bands.green}, ${bands.greenOut} outside)`);

// 2. a turned view = the unturned one rotated (top-down, the cytoplasm's mask), IoU
const iou = await page.evaluate(async () => {
  const b = iscScene.cellBounds(__key), pv = [b.center[0], b.center[1], 0], th = 37, W = 400, H = 300;
  const mk = async az => { const { data } = await __cap(__st({ camera: { pivot: pv, azimuthDeg: az, tiltDeg: 0, fovUm: b.diam * 1.3 },
    layers: { 'cyto.surface': { style: { color: '#ffffff', opacity: 1 } } } }), W, H); const m = new Uint8Array(W * H); for (let i = 0; i < W * H; i++) m[i] = data[4 * i] > 128 ? 1 : 0; return m; };
  const a = await mk(0), r = await mk(th), t = th * Math.PI / 180;
  // a world point at screen offset (u, v) unturned shows at R(-th) (u, v) turned (screen y down = turned v)
  let inter = 0, uni = 0;
  for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) {
    const u = x + 0.5 - W / 2, v = y + 0.5 - H / 2, x2 = Math.round(W / 2 + u * Math.cos(t) + v * Math.sin(t) - 0.5), y2 = Math.round(H / 2 - u * Math.sin(t) + v * Math.cos(t) - 0.5);
    const A0 = a[y * W + x], B0 = x2 >= 0 && y2 >= 0 && x2 < W && y2 < H ? r[y2 * W + x2] : 0;
    if (A0 && B0) inter++; if (A0 || B0) uni++;
  }
  return inter / Math.max(1, uni);
});
check(iou > 0.97, `rotation: the turned image is the unturned one rotated (IoU ${iou.toFixed(4)})`);

// 3. the same state twice: identical pixels
const same = await page.evaluate(async () => {
  const st = __st({ camera: { pivot: [0, 0, 1], azimuthDeg: 23, tiltDeg: 40, fovUm: 50 }, scope: 'ghosts', layers: { 'mt.lines': {}, 'cyto.surface': {}, 'nucleus.surface': {} } });
  const a = (await __cap(st)).data, b = (await __cap(st)).data;
  let n = 0; for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) n++;
  return n;
});
check(same === 0, `determinism: a frame captured twice is identical (${same} values differ)`);

// 4. the detail budget
const det = await page.evaluate(async () => {
  els.detailCells.value = 3; els.detailCells.dispatchEvent(new Event('input', { bubbles: true }));
  view.scale = 3; draw(); await new Promise(r => setTimeout(r, 3000)); draw();
  const t = document.getElementById('hud').textContent;
  els.detailCells.value = 5; els.detailCells.dispatchEvent(new Event('input', { bubbles: true })); view.scale = 9; draw();
  return t;
});
check(/, 3 detailed\)/.test(det), 'detail budget: 3 detailed cells in a zoomed-out view (' + (/cells [^\n]*detailed\)/.exec(det) || [''])[0] + ')');

// 5. a z-stack plane = the stand-alone movie job of its spec (WideField, one frame, plane 2)
const eq = await page.evaluate(async () => {
  const over = { wfAverage: 1, stepUm: 0.5 };
  const [st] = await iscScene.acquireData(['mt.wfSlice'], __key, over, null);
  const p = 2, spec = movieSpec(Object.assign({}, st.ov, { z: st.zs[p], seed: (+document.getElementById('mv_seed').value | 0) + 1000 * p, 'start-sec': 0 }));
  const d = await movieJob(spec), P = st.size * st.size;
  let n = 0; for (let i = 0; i < P; i++) if (d.frames[i] !== st.data[p * P + i]) n++;
  return { n, P, nz: st.zs.length };
});
check(eq.n === 0, `data: z-stack plane 2 of ${eq.nz} = a stand-alone movie job (${eq.n} of ${eq.P} pixels differ)`);

// 6. a WideField slice draws into the frame where the stack is
const lit = await page.evaluate(async () => {
  const b = iscScene.cellBounds(__key), pv = [b.center[0], b.center[1], 1];
  const st = __st({ camera: { pivot: pv, azimuthDeg: 0, tiltDeg: 0, fovUm: b.diam * 1.2 }, slice: { axis: 'z', pos: 1 }, data: { wfAverage: 1, stepUm: 0.5 },
    layers: { 'mt.wfSlice': { style: { color: '#ffffff', opacity: 1 } } } });
  const { data } = await __cap(st);
  let n = 0; for (let i = 0; i < data.length; i += 4) if (data[i] > 40) n++;
  return n;
});
check(lit > 1000, `data: the WideField slice is drawn (${lit} lit px)`);

// 7. the thresholded WideField surface (same stack) and the localizations of a short multi-plane acquisition
const more = await page.evaluate(async () => {
  const b = iscScene.cellBounds(__key), pv = [b.center[0], b.center[1], 1], data = { wfAverage: 1, stepUm: 0.5, locFrames: 300 };
  const cam = { pivot: pv, azimuthDeg: 30, tiltDeg: 45, fovUm: b.diam * 1.2 };
  const count = d => { let n = 0; for (let i = 0; i < d.length; i += 4) if (d[i] + d[i + 1] + d[i + 2] > 90) n++; return n; };
  const iso = await __cap(__st({ camera: cam, data, layers: { 'mt.wfIso': { style: { color: '#ffffff', opacity: 1 } } } }));
  const [L] = await iscScene.acquireData(['mt.locs'], __key, data, null);
  const loc = await __cap(__st({ camera: cam, data, layers: { 'mt.locs': { style: { color: '#ffffff', opacity: 1, colorBy: 'flat' } } } }));
  // every localization lies within the capture range of a focus position and inside the cell's box
  let inside = true;
  for (let i = 0; i < L.n; i += 97) { const x = L.locs[8 * i] + L.ox, y = L.locs[8 * i + 1] + L.oy; if (x < b.x[0] - 1 || x > b.x[1] + 1 || y < b.y[0] - 1 || y > b.y[1] + 1) inside = false; }
  return { iso: count(iso.data), loc: count(loc.data), n: L.n, inside };
});
check(more.iso > 1000, `data: the thresholded WideField surface is drawn (${more.iso} lit px)`);
check(more.n > 1000 && more.inside && more.loc > 500, `data: ${more.n} localizations from 300 frames per plane, in the cell's box, drawn (${more.loc} lit px)`);
if (errors.length) { fail++; console.log('page errors: ' + errors.join('; ')); }
// 8. an opaque slice (SMLM frames) hides only what is behind it: the cytoplasm surface above the plane stays visible
const ahead = await page.evaluate(async () => {
  const b = iscScene.cellBounds(__key), pv = [b.center[0], b.center[1], 1], data = { wfAverage: 1, stepUm: 0.5, srFrames: 2 };
  await iscScene.acquireData(['mt.srFrames'], __key, data, null);
  const green = d => { let n = 0; for (let i = 0; i < d.length; i += 4) if (d[i + 1] > 60 && d[i + 1] > 2 * d[i] && d[i + 1] > 2 * d[i + 2]) n++; return n; };
  const cyto = { style: { color: '#00ff00', opacity: 0.6 }, intervals: [{ lo: 0.6, hi: 1e30 }] };
  const st = layers => __st({ camera: { pivot: pv, azimuthDeg: 0, tiltDeg: 0, fovUm: b.diam * 1.2 }, data, sweep: { axis: 'z' }, slice: { axis: 'z', pos: 0.4 }, layers });
  return { alone: green((await __cap(st({ 'cyto.surface': cyto }))).data), withSr: green((await __cap(st({ 'cyto.surface': cyto, 'mt.srFrames': {} }))).data) };
});
check(ahead.alone > 1000 && ahead.withSr >= 0.95 * ahead.alone, `data: the cytoplasm above an SMLM slice stays visible (${ahead.withSr} of ${ahead.alone} px)`);

await browser.close();
server.close();
process.exit(fail ? 1 : 0);
