#!/usr/bin/env node
// The viewer's "Make movie here" in BrightField mode, end to end in headless
// Chromium: the BF rows show, the quality slider is sent (quality 1 and 3),
// both make a movie and the info line says BrightField; the movie split
// across the workers (the default) equals the single-worker movie
// (?bfsplit=0) frame for frame; a drifting sample (Drift xy/z) renders on one
// worker and its second frame differs from the still one.
//   node tests/web/viewer_bf_movie.mjs
import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
let chromium;
try { ({ chromium } = await import('playwright')); }
catch { ({ chromium } = await import('/opt/node22/lib/node_modules/playwright/index.mjs')); }
const server = http.createServer((req, res) => {
  const f = path.join(ROOT, 'web', decodeURIComponent(req.url.split('?')[0]).replace(/^\/+/, '') || 'index.html');
  if (!f.startsWith(path.join(ROOT, 'web')) || !fs.existsSync(f)) { res.statusCode = 404; res.end(); return; }
  res.setHeader('content-type', f.endsWith('.js') ? 'text/javascript' : 'text/html');
  res.end(fs.readFileSync(f));
}).listen(0);
const port = server.address().port;
const browser = await chromium.launch();
let fail = 0;
const frames = {};
for (const [quality, want, query, drift] of [['1', /BrightField/, ''], ['3', /BrightField/, ''], ['3', /BrightField/, '?bfsplit=0'],
                                             ['3', /BrightField \(transmitted light\),/, '', '300']]) {
  const page = await browser.newPage();
  const errors = [];
  page.on('pageerror', e => errors.push(e.message));
  await page.goto(`http://localhost:${port}/index.html${query}`);
  await page.waitForFunction(() => typeof createInsiliscope === 'function' || !!self.ISC_MODULE_SRC);
  const shown = await page.evaluate(([q, d]) => {
    document.getElementById('mv_drift-xy-nm-per-sqrt-sec').value = d || '0';
    document.getElementById('mv_drift-z-nm-per-sqrt-sec').value = d || '0';
    document.getElementById('mv_modality').value = '1';
    document.getElementById('mv_modality').dispatchEvent(new Event('change'));
    document.getElementById('mv_size').value = '48';
    document.getElementById('mv_frames').value = '2';
    const s = document.getElementById('mv_bf-quality');
    s.value = q;
    s.dispatchEvent(new Event('input'));
    return [...document.querySelectorAll('.bfRow')].every(r => r.style.display === '') &&
      document.getElementById('mv_bf-quality-name').textContent.startsWith(q);
  }, [quality, drift]);
  await page.waitForTimeout(2000);
  await page.evaluate(() => document.getElementById('mv_make').click());   // the button may sit in a folded group (Focus style)
  await page.waitForFunction(() => /frames/.test(document.getElementById('mv_info').textContent), null, { timeout: 600000 });
  const info = await page.evaluate(() => document.getElementById('mv_info').textContent);
  frames[quality + query + (drift ? ' drift' : '')] = await page.evaluate(() => Array.from(movie.frames));
  const ok = shown && want.test(info) && !errors.length;
  if (!ok) fail++;
  console.log(`${ok ? 'ok  ' : 'FAIL'}  viewer BrightField quality ${quality}${query}${drift ? ' drift ' + drift : ''}: ${info}${errors.length ? ' errors: ' + errors.join('; ') : ''}`);
  await page.close();
}
{
  const a = frames['3'], b = frames['3?bfsplit=0'];
  const same = a && b && a.length === b.length && a.every((v, i) => v === b[i]);
  if (!same) fail++;
  console.log(`${same ? 'ok  ' : 'FAIL'}  viewer BrightField quality 3: the movie split across the workers = the single-worker movie, ${a ? a.length : 0} pixels${same ? ' identical' : ' DIFFER'}`);
}
{
  const a = frames['3'], b = frames['3 drift'], P = 48 * 48;
  // Frame 0 (no drift yet; only the grown margin differs) stays close, frame 1 (the sample moved) does not.
  let d0 = 0, d1 = 0;
  for (let i = 0; a && b && i < P; i++) { d0 += Math.abs(a[i] - b[i]); d1 += Math.abs(a[P + i] - b[P + i]); }
  const ok = a && b && a.length === b.length && d1 > 3 * d0 && d1 / P > 5;
  if (!ok) fail++;
  console.log(`${ok ? 'ok  ' : 'FAIL'}  viewer BrightField drift: vs the still movie, frame 0 ${(d0 / P).toFixed(1)}, frame 1 ${(d1 / P).toFixed(1)} ADU/px`);
}
await browser.close();
server.close();
process.exit(fail ? 1 : 0);
