#!/usr/bin/env node
// Pan benchmark for the viewer: 300 synthetic pan frames, each timing one
// synchronous draw() (the main-thread cost of a frame; generation runs in the
// workers), with a requestAnimationFrame between frames so worker results can
// land. Headless Chromium, so WebGL is software (SwiftShader).
//
//   node web/tools/bench_pan.mjs [page.html] [--frames=300] [--step=0.8]
//
// page defaults to web/index.html; --step is um per frame (default 0.8 um at
// 9 px/um = 7 px/frame, ~240 um over the run). Prints p50/p95/max draw time
// and how many frames drew with cells still loading (pop-in).
import path from 'node:path';
import { createRequire } from 'node:module';
import { fileURLToPath, pathToFileURL } from 'node:url';

const require = createRequire(import.meta.url);
let chromium;
try { ({ chromium } = require('playwright')); }
catch { ({ chromium } = require(path.join(require('child_process').execSync('npm root -g').toString().trim(), 'playwright'))); }

const HERE = path.dirname(fileURLToPath(import.meta.url));
const args = process.argv.slice(2);
const opt = k => { const a = args.find(s => s.startsWith(`--${k}=`)); return a ? +a.split('=')[1] : null; };
const page0 = args.find(a => !a.startsWith('--')) || path.join(HERE, '..', 'index.html');
const FRAMES = opt('frames') || 300, STEP = opt('step') || 0.8;

const browser = await chromium.launch({ executablePath: process.env.CHROMIUM_PATH || undefined });
const page = await browser.newPage({ viewport: { width: 1280, height: 800 } });
const errors = [];
page.on('pageerror', e => errors.push(e.message));
page.on('console', m => { if ((m.type() === 'error' || m.type() === 'warning') && !/GL Driver|software WebGL/.test(m.text())) errors.push(m.text()); });
await page.goto(pathToFileURL(path.resolve(page0)).href);

const hudIdle = () => page.evaluate(() => {
  const t = document.getElementById('hud').textContent;
  return /cells \d+/.test(t) && !/\[loading|\[packing/.test(t);
});
const t0 = Date.now();
while (!(await hudIdle())) {
  if (Date.now() - t0 > 180000) throw new Error('viewer never finished loading: ' + (await page.evaluate(() => document.getElementById('hud').textContent)));
  await page.waitForTimeout(200);
}
const loadMs = Date.now() - t0;

const r = await page.evaluate(async ({ FRAMES, STEP }) => {
  const raf = () => new Promise(res => requestAnimationFrame(res));
  const hud = document.getElementById('hud');
  const times = [];
  let loading = 0;
  for (let i = 0; i < FRAMES; i++) {
    view.cx += STEP;
    const t = performance.now();
    draw();
    times.push(performance.now() - t);
    if (/\[loading|\[packing/.test(hud.textContent)) loading++;
    await raf();
  }
  return { times, loading, hud: hud.textContent };
}, { FRAMES, STEP });

const s = [...r.times].sort((a, b) => a - b);
const q = f => s[Math.min(s.length - 1, Math.floor(f * s.length))];
console.log(`${path.relative(process.cwd(), path.resolve(page0))}: initial load ${(loadMs / 1000).toFixed(1)} s; ` +
  `${FRAMES} frames x ${STEP} um: draw p50 ${q(0.5).toFixed(1)} ms, p95 ${q(0.95).toFixed(1)} ms, ` +
  `max ${s[s.length - 1].toFixed(1)} ms; ${r.loading} frames with cells still loading`);
console.log('hud: ' + r.hud.replace(/\n/g, ' | '));
if (errors.length) console.log('page errors/warnings:\n  ' + errors.slice(0, 10).join('\n  '));
await browser.close();
