#!/usr/bin/env node
// The viewer's "Make movie here" with a WideField label (mEGFP, mean-field), end
// to end in headless Chromium: once with WebGPU (?wfgpu=any: SwiftShader is a
// software adapter) and once without; both must produce a movie, the info line
// says which ran (issue 16: WideField is a label mode, its dyes mean-field).
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
// ISC_CHROME=1: the installed Chrome on the real GPU (Windows: headless Chromium has no WebGPU adapter there).
const browser = await chromium.launch(process.env.ISC_CHROME ? { channel: 'chrome', args: ['--enable-unsafe-webgpu'] } :
  { args: ['--enable-unsafe-webgpu', '--enable-features=Vulkan', '--use-vulkan=swiftshader',
    '--use-webgpu-adapter=swiftshader', '--disable-vulkan-surface'] });
let fail = 0;
// Without ?wfgpu=any the viewer skips software adapters: CPU in headless Chromium, the GPU in Chrome on a real one.
for (const [query, want] of [['?wfgpu=any', /GPU: /], ['', process.env.ISC_CHROME ? /GPU: / : /\(CPU\)/]]) {
  const page = await browser.newPage();
  const errors = [];
  page.on('pageerror', e => errors.push(e.message));
  await page.goto(`http://localhost:${port}/index.html${query}`);
  await page.waitForFunction(() => typeof createInsiliscope === 'function' || !!self.ISC_MODULE_SRC);
  await page.evaluate(() => {
    // The mode first: the Dye select lists only the dyes with data for it (DNA-PAINT by default).
    const pick = (id, test) => {
      const sel = document.getElementById(id);
      sel.value = [...sel.options].find(test).value;
      sel.dispatchEvent(new Event('input', { bubbles: true }));
      sel.dispatchEvent(new Event('change', { bubbles: true }));
    };
    pick('mv_mt-mode', o => o.textContent === 'WideField');
    pick('mv_mt-dye', o => o.textContent.includes('mEGFP'));
    document.getElementById('mv_size').value = '32';
    document.getElementById('mv_frames').value = '3';
  });
  await page.waitForTimeout(2000);
  await page.evaluate(() => document.getElementById('mv_make').click());   // the button may sit in a folded group (Focus style)
  await page.waitForFunction(() => /frames/.test(document.getElementById('mv_info').textContent), null, { timeout: 600000 });
  const info = await page.evaluate(() => document.getElementById('mv_info').textContent);
  // No WebGPU adapter at all in this browser (no SwiftShader Vulkan): the GPU case can only fall back.
  const noAdapter = query && await page.evaluate(async () => !navigator.gpu || !(await navigator.gpu.requestAdapter()));
  if (noAdapter) { console.log(`SKIP  viewer${query}: no WebGPU adapter (${info})`); await page.close(); continue; }
  const ok = want.test(info) && !errors.length;
  if (!ok) fail++;
  console.log(`${ok ? 'ok  ' : 'FAIL'}  viewer${query || ' (no GPU flag)'}: ${info}${errors.length ? ' errors: ' + errors.join('; ') : ''}`);
  await page.close();
}
await browser.close();
server.close();
process.exit(fail ? 1 : 0);
