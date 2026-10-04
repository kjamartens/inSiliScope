#!/usr/bin/env node
// The viewer's "Make movie here" in BrightField mode, end to end in headless
// Chromium: the BF rows show, the quality slider is sent (quality 1 and 3),
// both make a movie and the info line says BrightField; the movie split
// across the workers (the default) equals the single-worker movie
// (?bfsplit=0) frame for frame.
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
for (const [quality, want, query] of [['1', /BrightField/, ''], ['3', /BrightField/, ''], ['3', /BrightField/, '?bfsplit=0']]) {
  const page = await browser.newPage();
  const errors = [];
  page.on('pageerror', e => errors.push(e.message));
  await page.goto(`http://localhost:${port}/index.html${query}`);
  await page.waitForFunction(() => typeof createInsiliscope === 'function' || !!self.ISC_MODULE_SRC);
  const shown = await page.evaluate(q => {
    document.getElementById('mv_modality').value = '2';
    document.getElementById('mv_modality').dispatchEvent(new Event('change'));
    document.getElementById('mv_size').value = '48';
    document.getElementById('mv_frames').value = '2';
    const s = document.getElementById('mv_bf-quality');
    s.value = q;
    s.dispatchEvent(new Event('input'));
    return [...document.querySelectorAll('.bfRow')].every(r => r.style.display === '') &&
      document.getElementById('mv_bf-quality-name').textContent.startsWith(q);
  }, quality);
  await page.waitForTimeout(2000);
  await page.click('#mv_make', { force: true });
  await page.waitForFunction(() => /frames/.test(document.getElementById('mv_info').textContent), null, { timeout: 600000 });
  const info = await page.evaluate(() => document.getElementById('mv_info').textContent);
  frames[quality + query] = await page.evaluate(() => Array.from(movie.frames));
  const ok = shown && want.test(info) && !errors.length;
  if (!ok) fail++;
  console.log(`${ok ? 'ok  ' : 'FAIL'}  viewer BrightField quality ${quality}${query}: ${info}${errors.length ? ' errors: ' + errors.join('; ') : ''}`);
  await page.close();
}
{
  const a = frames['3'], b = frames['3?bfsplit=0'];
  const same = a && b && a.length === b.length && a.every((v, i) => v === b[i]);
  if (!same) fail++;
  console.log(`${same ? 'ok  ' : 'FAIL'}  viewer BrightField quality 3: the movie split across the workers = the single-worker movie, ${a ? a.length : 0} pixels${same ? ' identical' : ' DIFFER'}`);
}
await browser.close();
server.close();
process.exit(fail ? 1 : 0);
