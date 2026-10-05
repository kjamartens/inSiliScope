#!/usr/bin/env node
// SPDX-License-Identifier: BSD-3-Clause
// The viewer's Animation tab end to end in headless Chromium: a short sequence exported as GIF (decoded back: frame
// count, size, not blank; a second export is byte-identical), WebM and MP4 (played back in a <video>: duration, size,
// a decoded middle frame is not blank; MP4 is skipped where this Chromium cannot encode H.264), the preview draws,
// and the movie player's GIF reproduces its grey display exactly.
//   node tests/web/viewer_anim_export.mjs [--channel=chrome] [--gpu]
// --channel picks an installed browser (Playwright's bundled Chromium has no H.264 encoder); --gpu uses the hardware
// GPU instead of SwiftShader.
import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';
import { createRequire } from 'node:module';
import { fileURLToPath } from 'node:url';
import { decodeGif } from './lib/decoders.mjs';

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
const port = server.address().port;
const browser = await chromium.launch({ channel: arg('channel') || undefined,
  args: arg('gpu') ? ['--enable-gpu', '--ignore-gpu-blocklist'] : ['--use-angle=swiftshader', '--enable-unsafe-swiftshader'] });
const page = await browser.newPage({ viewport: { width: 1100, height: 760 } });
const errors = [];
page.on('pageerror', e => errors.push(e.message));
let fail = 0;
const check = (ok, what) => { if (!ok) fail++; console.log(`${ok ? 'ok  ' : 'FAIL'}  ${what}`); };
await page.goto(`http://localhost:${port}/index.html`);
await page.waitForFunction(() => window.iscAnim && /cells [1-9]/.test(document.getElementById('hud').textContent), null, { timeout: 300000 });

// a 2 s sequence: an orbit with a build-up of the microtubules of the cell at the centre
const SEQ = await page.evaluate(() => {
  const A = IscAnimSeq, s = A.defaults();
  s.name = 'test';
  s.output = Object.assign(s.output, { width: 320, height: 240, fps: 12, gif: { width: 160, fps: 6, dither: 'bayer4' }, loop: 'once' });
  s.cycles = [A.cycleFromPreset('buildUp', { structure: 'mt', duration: 2, deg: 60 })];
  return s;
});
const exportAs = format => page.evaluate(async ({ SEQ, format }) => {
  const s = JSON.parse(JSON.stringify(SEQ)); s.output.format = format;
  iscAnim.open(); iscAnim.load(s);
  const r = await iscAnim.exportBlob({});
  const b = new Uint8Array(await r.blob.arrayBuffer());
  let bin = ''; for (let i = 0; i < b.length; i += 8192) bin += String.fromCharCode.apply(null, b.subarray(i, i + 8192));
  return { ext: r.ext, frames: r.frames, w: r.width, h: r.height, fell: r.fellBack || '', b64: btoa(bin), url: URL.createObjectURL(r.blob) };
}, { SEQ, format });

// GIF: decode, not blank, deterministic
const g1 = await exportAs('gif'), g2 = await exportAs('gif');
const gif = decodeGif(Buffer.from(g1.b64, 'base64'));
const lit = f => { let n = 0; for (let i = 0; i < f.rgb.length; i += 3) if (f.rgb[i] + f.rgb[i + 1] + f.rgb[i + 2] > 60) n++; return n; };
check(g1.ext === 'gif' && gif.width === 160 && gif.height === 120 && gif.loop, `GIF: 160 x 120, looping`);
check(gif.frames.reduce((s, f) => s + f.delay, 0) >= 190 && gif.frames.reduce((s, f) => s + f.delay, 0) <= 210, `GIF: ${gif.frames.length} frames over ~2 s`);
check(lit(gif.frames[gif.frames.length - 1]) > 200, 'GIF: the last frame shows the built-up microtubules');
check(g1.b64 === g2.b64, 'GIF: a second export gives the same bytes');

// videos: play back in the page
const play = url => page.evaluate(async url => {
  const v = document.createElement('video'); v.muted = true; v.src = url;
  await new Promise((res, rej) => { v.onloadeddata = res; v.onerror = () => rej(new Error('cannot load: ' + (v.error && v.error.message))); });
  v.currentTime = v.duration / 2;
  await new Promise(res => { v.onseeked = res; });
  const c = document.createElement('canvas'); c.width = v.videoWidth; c.height = v.videoHeight;
  const g = c.getContext('2d'); g.drawImage(v, 0, 0);
  const d = g.getImageData(0, 0, c.width, c.height).data;
  let lit = 0; for (let i = 0; i < d.length; i += 4) if (d[i] + d[i + 1] + d[i + 2] > 60) lit++;
  return { duration: v.duration, w: v.videoWidth, h: v.videoHeight, lit };
}, url);
for (const fmt of ['webm', 'mp4']) {
  const r = await exportAs(fmt);
  if (r.ext !== fmt) { console.log(`skip  ${fmt}: this browser fell back to ${r.ext} (${r.fell})`); continue; }
  try {
    const p = await play(r.url);
    check(p.w === 320 && p.h === 240 && Math.abs(p.duration - 2) < 0.2 && p.lit > 100, `${fmt.toUpperCase()}: ${r.frames} frames, plays back ${p.w} x ${p.h}, ${p.duration.toFixed(2)} s, middle frame drawn`);
  } catch (e) { check(false, `${fmt.toUpperCase()}: ${e.message}`); }
}

// the preview draws into the view
const prev = await page.evaluate(async () => { iscAnim.seek(1.9); await new Promise(r => setTimeout(r, 400)); draw(); const s = previewState; iscAnim.exitPreview(); return !!(s && s.camera && s.camera.frameAspect); });
check(prev, 'preview: the view draws the animation frame (camera and frame aspect from the sequence)');

// the movie player's GIF: its grey display, exactly
await page.evaluate(() => { iscAnim.exitPreview(); document.querySelector('#tabs [data-tab=settings]').click();
  document.getElementById('mv_size').value = '32'; document.getElementById('mv_frames').value = '3'; document.getElementById('mv_make').click(); });
await page.waitForFunction(() => /frames/.test(document.getElementById('mv_info').textContent), null, { timeout: 300000 });
const [dl] = await Promise.all([page.waitForEvent('download'), page.evaluate(() => document.getElementById('mv_gif').click())]);
const mg = decodeGif(fs.readFileSync(await dl.path()));
const want = await page.evaluate(() => {
  const d = movie, k = Math.max(1, Math.ceil(512 / Math.max(d.w, d.h))), out = [];
  for (let f = 0; f < d.n; f++) { showMovieFrame(f); const g = document.getElementById('mv_cv').getContext('2d'); out.push(Array.from(g.getImageData(0, 0, d.w, d.h).data.filter((_, i) => i % 4 === 0))); }
  return { k, w: movie.w, frames: out };
});
let exact = mg.frames.length === want.frames.length;
for (let f = 0; exact && f < mg.frames.length; f++) for (let y = 0; exact && y < want.w; y++) for (let x = 0; x < want.w; x++) {
  if (mg.frames[f].rgb[3 * ((y * want.k) * mg.width + x * want.k)] !== want.frames[f][y * want.w + x]) { exact = false; break; }
}
check(exact && mg.width === want.w * want.k, `movie player GIF: ${mg.frames.length} frames, ${mg.width} px, = the display LUT`);
if (errors.length) { fail++; console.log('page errors: ' + errors.join('; ')); }
await browser.close();
server.close();
process.exit(fail ? 1 : 0);
