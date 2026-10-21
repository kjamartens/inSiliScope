#!/usr/bin/env node
// SPDX-License-Identifier: BSD-3-Clause
// The viewer's usage counts (web/live/usage.js) in headless Chromium, with Umami stubbed (nothing leaves the
// machine; the stub's track() keeps the events in sessionStorage, which outlives leaving the page): on localhost no request goes to Umami; served as the published site, restored settings count nothing,
// a person's level, Mode, Live and Snap clicks count, and the summary goes when the page closes.
//   node tests/web/viewer_analytics.mjs [--channel=chrome]
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
const fileOf = url => {
  const f = path.join(ROOT, 'web', decodeURIComponent(url.split('?')[0]).replace(/^\/+/, '') || 'index.html');
  return f.startsWith(path.join(ROOT, 'web')) && fs.existsSync(f) && !fs.statSync(f).isDirectory() ? f : null;
};
const type = f => (f.endsWith('.js') ? 'text/javascript' : f.endsWith('.json') ? 'application/json' : 'text/html');
const server = http.createServer((req, res) => {
  const f = fileOf(req.url);
  if (!f) { res.statusCode = 404; res.end(); return; }
  res.setHeader('content-type', type(f));
  res.end(fs.readFileSync(f));
}).listen(0);

let fail = 0;
const t0 = Date.now(), check = (ok, what) => { if (!ok) fail++; console.log(`${ok ? 'ok  ' : 'FAIL'}  ${what}  [${((Date.now() - t0) / 1000).toFixed(0)} s]`); };
const browser = await chromium.launch({ channel: arg('channel') || undefined, args: ['--use-angle=swiftshader', '--enable-unsafe-swiftshader'] });
const errors = [];
try {
  // ---- localhost: nothing goes to Umami ----
  {
    const ctx = await browser.newContext({ viewport: { width: 1200, height: 800 } });
    const hits = [];
    await ctx.route('**/*umami*/**', r => { hits.push(r.request().url()); r.abort(); });
    const page = await ctx.newPage();
    page.on('pageerror', e => errors.push(e.message));
    await page.goto(`http://localhost:${server.address().port}/index.html`);
    await page.waitForFunction(() => typeof genWorkers !== 'undefined' && genWorkers.ok, null, { timeout: 120000 });
    await page.click('#levelSeg button[data-level=advanced]');
    const off = await page.evaluate(() => typeof iscTrack === 'function' && !iscTrack._test.on);
    check(off && hits.length === 0, `localhost: iscTrack is a no-op, no request to Umami (${hits.length})`);
    await ctx.close();
  }

  // ---- the published site (served from web/), Umami stubbed ----
  const ctx = await browser.newContext({ viewport: { width: 1400, height: 900 } });
  const stray = [];
  let sent = [];
  await ctx.route('http://kjamartens.github.io/**', r => {
    const p = new URL(r.request().url()).pathname;
    if (p === '/inSiliScope/blank') return r.fulfill({ body: '<!DOCTYPE html><title>blank</title>', contentType: 'text/html' });
    const f = fileOf(p.replace(/^\/inSiliScope/, ''));
    return f ? r.fulfill({ body: fs.readFileSync(f), contentType: type(f) }) : r.fulfill({ status: 404 });
  });
  await ctx.route('https://cloud.umami.is/**', r => {
    const u = r.request().url();
    if (u.endsWith('/script.js')) return r.fulfill({ contentType: 'text/javascript',
      body: 'window.umami = { track: (n, d) => { const a = JSON.parse(sessionStorage.getItem("__sent") || "[]"); a.push([n, d || {}]); sessionStorage.setItem("__sent", JSON.stringify(a)); } };' });
    stray.push(u);
    return r.abort();
  });
  const read = async () => { sent = await page.evaluate(() => JSON.parse(sessionStorage.getItem('__sent') || '[]')); };
  // a setting from an earlier visit: the Look "fluo"
  await ctx.addInitScript(() => {
    if (!sessionStorage.getItem('seeded')) {
      sessionStorage.setItem('seeded', '1');
      localStorage.setItem('isc.set.theme', 'fluo');
    }
  });
  const page = await ctx.newPage();
  page.on('pageerror', e => errors.push(e.message));
  await page.goto('http://kjamartens.github.io/inSiliScope/');
  await page.waitForFunction(() => typeof genWorkers !== 'undefined' && genWorkers.ok && window.umami, null, { timeout: 120000 });
  await page.waitForTimeout(1500);
  await read();
  const restored = await page.evaluate(() => document.getElementById('theme').value);
  check(restored === 'fluo' && !sent.some(e => e[0] === 'pick'), `restored settings count nothing (Look ${restored}; ${sent.length} events)`);

  await page.click('#levelSeg button[data-level=expert]');
  // the Mode: a key press on the focused select (a person's change; selectOption's events are synthetic)
  const before = await page.evaluate(() => document.querySelector('select[data-pick="mode"]').value);
  await page.focus('select[data-pick="mode"]');
  await page.keyboard.press('ArrowDown');
  await page.click('#lv_live');
  await page.waitForTimeout(500);
  await page.click('#lv_live');
  await page.click('#lv_snap');
  await page.waitForTimeout(1000);
  const after = await page.evaluate(() => document.querySelector('select[data-pick="mode"]').value);
  await read();
  const names = sent.map(e => e[0] + (e[0] === 'pick' ? ':' + e[1].control : ''));
  check(names.includes('pick:level') && sent.find(e => e[1].control === 'level')[1].value === 'expert', 'a level click counts');
  check(after !== before && sent.some(e => e[1].control === 'mode' && e[1].value === after), `a Mode pick counts (${after})`);
  check(names.filter(n => n === 'live').length === 1 && names.includes('snap'), `Live (started once) and Snap count: ${names.join(' ')}`);

  await page.goto('http://kjamartens.github.io/inSiliScope/blank');
  await read();
  const sum = sent.find(e => e[0] === 'summary');
  check(sum && sum[1].live === true && sum[1].level === 'expert' && sum[1].look === 'fluo' && sum[1].mode === after && sum[1].minutes === '<=1',
    `the summary when the page closes: ${sum ? JSON.stringify(sum[1]) : 'none'}`);
  check(stray.length === 0, `no other request to Umami (${stray.join(' ')})`);
  check(errors.length === 0, `no page errors${errors.length ? ': ' + errors.join(' | ') : ''}`);
} finally {
  await browser.close();
  server.close();
}
console.log(`${fail ? 'FAIL' : 'ok'}  viewer_analytics  ${((Date.now() - t0) / 1000).toFixed(1)} s`);
process.exit(fail ? 1 : 0);
