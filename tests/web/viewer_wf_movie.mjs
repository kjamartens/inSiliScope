#!/usr/bin/env node
// The viewer's "Make movie here" in WideField mode, end to end in headless
// Chromium: once with WebGPU (?wfgpu=any: SwiftShader is a software adapter)
// and once without; both must produce a movie, the info line says which ran.
//   node tests/web/viewer_wf_movie.mjs
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
const browser = await chromium.launch({ args: ['--enable-unsafe-webgpu', '--enable-features=Vulkan', '--use-vulkan=swiftshader',
  '--use-webgpu-adapter=swiftshader', '--disable-vulkan-surface'] });
let fail = 0;
for (const [query, want] of [['?wfgpu=any', /GPU: /], ['', /\(CPU\)/]]) {
  const page = await browser.newPage();
  const errors = [];
  page.on('pageerror', e => errors.push(e.message));
  await page.goto(`http://localhost:${port}/index.html${query}`);
  await page.waitForFunction(() => typeof createInsiliscope === 'function' || !!self.ISC_MODULE_SRC);
  await page.evaluate(() => {
    document.getElementById('mv_modality').value = '1';
    document.getElementById('mv_modality').dispatchEvent(new Event('change'));
    document.getElementById('mv_size').value = '32';
    document.getElementById('mv_frames').value = '3';
  });
  await page.waitForTimeout(2000);
  await page.evaluate(() => document.getElementById('mv_make').click());   // the button may sit in a folded group (Focus style)
  await page.waitForFunction(() => /frames/.test(document.getElementById('mv_info').textContent), null, { timeout: 600000 });
  const info = await page.evaluate(() => document.getElementById('mv_info').textContent);
  const ok = want.test(info) && !errors.length;
  if (!ok) fail++;
  console.log(`${ok ? 'ok  ' : 'FAIL'}  viewer${query || ' (no GPU flag)'}: ${info}${errors.length ? ' errors: ' + errors.join('; ') : ''}`);
  await page.close();
}
await browser.close();
server.close();
process.exit(fail ? 1 : 0);
