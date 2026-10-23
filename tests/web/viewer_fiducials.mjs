#!/usr/bin/env node
// SPDX-License-Identifier: BSD-3-Clause
// Fiducial beads in the viewer (headless Chromium): the Sample's Fiducial beads group (off by default: only Density
// shows), beads at 0.2 /µm² drawn in the world view (#hud data-beads) and toggled by View > Fiducial beads, and a Snap
// with TetraSpeck beads far brighter than one without (spec/ALGORITHM.md "Fiducial beads"). On the nanorulers with
// their label off: only the beads emit, and no cell packing or dye kernel competes with the Snap (a first cell-field
// frame takes minutes in headless Chromium on a laptop).
//   node tests/web/viewer_fiducials.mjs [--channel=chrome]
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
setTimeout(() => { console.log('FAIL  timed out after 240 s'); process.exit(1); }, 240000).unref();
const browser = await chromium.launch({ channel: arg('channel') || undefined, args: ['--use-angle=swiftshader', '--enable-unsafe-swiftshader'] });
const page = await browser.newPage({ viewport: { width: 1400, height: 900 } });
const errors = [];
page.on('pageerror', e => errors.push(e.message));
const set = (id, v) => page.evaluate(([id, v]) => {
  const e = document.getElementById(id);
  if (e.type === 'checkbox') e.checked = v; else e.value = v;
  e.dispatchEvent(new Event('input', { bubbles: true }));
  e.dispatchEvent(new Event('change', { bubbles: true }));
}, [id, v]);
// A setting's row shown at its level (its row and the kids boxes over it up to its group or grid; the View pop-over
// itself may be closed).
const shown = id => page.evaluate(id => {
  for (let e = document.getElementById(id); e && !e.matches('.cells, details, body'); e = e.parentElement) if (e.style.display === 'none') return false;
  return true;
}, id);
// One Snap: its frame's max and the share of pixels above half of it.
async function snap() {
  const n = await page.evaluate(() => liveState.frames);
  await page.evaluate(() => document.getElementById('lv_snap').click());
  await page.waitForFunction(n => (liveState.frames > n && !liveState.pending) || liveState.err, n, { timeout: 120000 })
    .catch(async e => { throw new Error(e.message + ' ' + JSON.stringify(await page.evaluate(() => ({ frames: liveState.frames, pending: !!liveState.pending, err: liveState.err, on: liveState.on })))); });
  const err = await page.evaluate(() => liveState.err);
  if (err) throw new Error('snap: ' + err);
  return page.evaluate(() => { const f = liveState.frame; let mx = 0, s = 0; for (const v of f) { mx = Math.max(mx, v); s += v; } return { max: mx, mean: s / f.length }; });
}
try {
  await page.goto(`http://localhost:${server.address().port}/index.html?live=0`);
  await page.waitForFunction(() => window.liveState && typeof genWorkers !== 'undefined' && genWorkers.ok, null, { timeout: 120000 });
  await page.evaluate(() => { for (const g of document.querySelectorAll('details')) g.open = true; });
  const off = { density: await page.evaluate(() => +document.getElementById('mv_fid-density').value), type: await shown('mv_fid-type'),
    toggle: await shown('showFids'), beads: await page.evaluate(() => document.getElementById('hud').dataset.beads || '') };
  check(off.density === 0 && !off.type && !off.toggle && off.beads === '', `off by default: density 0, Type and the View toggle hidden, no beads drawn ${JSON.stringify(off)}`);

  await set('mv_specimen', '1');
  await set('mv_ruler-dye', await page.evaluate(() => [...document.getElementById('mv_ruler-dye').options].find(o => o.text.startsWith('None')).value));
  await set('mv_size', 64);
  const plain = await snap();
  await set('mv_fid-type', String(await page.evaluate(() => ISC_DYE_DATA.fiducials.types.findIndex(t => t.id === 'TetraSpeck'))));
  await set('mv_fid-density', 0.2);
  const on = { type: await shown('mv_fid-type'), toggle: await shown('showFids') };
  await page.waitForFunction(() => +(document.getElementById('hud').dataset.beads || 0) > 0, null, { timeout: 60000 });
  const nBeads = await page.evaluate(() => +document.getElementById('hud').dataset.beads);
  check(on.type && on.toggle && nBeads > 0, `beads on: Type and the View toggle show, ${nBeads} beads drawn in the world view ${JSON.stringify(on)}`);
  const nXz = await page.evaluate(() => document.getElementById('showXZ').checked ? +(document.getElementById('hud').dataset.beadsXz || 0) : -1);
  check(nXz > 0, `the x-z view draws the beads in its depth range too (${nXz})`);
  await set('showFids', false);
  await page.waitForFunction(() => (document.getElementById('hud').dataset.beads || '') === '', null, { timeout: 10000 });
  await set('showFids', true);
  check(true, 'View > Fiducial beads hides and shows them');

  const beads = await snap();
  check(beads.max > 3 * plain.max && beads.mean > plain.mean, `a Snap with TetraSpeck beads at 0.2 /µm²: max ${beads.max} vs ${plain.max} ADU without, mean ${beads.mean.toFixed(0)} vs ${plain.mean.toFixed(0)}`);
  check(!errors.length, `no page errors${errors.length ? ': ' + errors.join(' | ') : ''}`);
} catch (e) {
  check(false, 'threw: ' + (e && e.stack || e));
}
await browser.close();
server.close();
console.log(fail ? `viewer_fiducials: ${fail} FAILED (${((Date.now() - t0) / 1000).toFixed(0)} s)` : `viewer_fiducials: PASS (${((Date.now() - t0) / 1000).toFixed(0)} s)`);
process.exit(fail ? 1 : 0);
