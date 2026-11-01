#!/usr/bin/env node
// SPDX-License-Identifier: BSD-3-Clause
// The movie worker's death (an out-of-memory abort: the WebAssembly heap has a cap and never shrinks) in headless
// Chromium: the job in flight is answered with "out of memory", the worker alone is replaced (the engine stays in the
// workers, not on the main thread), and the next movie renders. The abort is raised on the worker as the browser
// raises a worker's uncaught RuntimeError (an ErrorEvent), with a real movie job in flight.
//   node tests/web/viewer_oom.mjs [--channel=chrome] [--swiftshader]
import { runViewer } from './browser.mjs';

await runViewer('viewer oom', { watchdogSec: 150 }, async ({ page, check }) => {
  const alerts = [];
  page.on('dialog', d => { alerts.push(d.message()); d.dismiss().catch(() => {}); });
  await page.waitForFunction(() => typeof genWorkers !== 'undefined' && genWorkers.ok && self.ISC_ENGINE_OPTIONS &&
    self.ISC_ENGINE_OPTIONS.size > 100, null, { timeout: 120000 });
  await page.evaluate(() => { const el = document.getElementById('mv_size'); el.value = 32; el.dispatchEvent(new Event('input', { bubbles: true })); });

  // A movie in flight (and the PSF preload's, a movie job too), then the worker's abort.
  const before = await page.evaluate(() => {
    const w = genWorkers.dyes;
    document.getElementById('mv_make').click();
    const inflight = [...dyesInflight.keys()].filter(k => k.startsWith('movie:')).length;
    w.dispatchEvent(new ErrorEvent('error', { message: 'Uncaught RuntimeError: Aborted(). Build with -sASSERTIONS for more info.', cancelable: true }));
    return { inflight, replaced: genWorkers.dyes !== w, ok: genWorkers.ok, main: !!mainEngine };
  });
  await page.waitForFunction(() => document.getElementById('mv_make').textContent === 'Make movie here', null, { timeout: 30000 });
  check(before.inflight >= 1 && before.replaced && before.ok && !before.main,
    `the abort replaced the movie worker alone (${before.inflight} movie jobs in flight; workers on: ${before.ok}, main-thread engine: ${before.main})`);
  check(alerts.length === 1 && /movie failed: out of memory/.test(alerts[0]), `the movie answered: "${(alerts[0] || '').slice(0, 70)}…"`);

  // The new worker: its options, then a movie.
  await page.waitForFunction(() => genWorkers.dyes.answered, null, { timeout: 60000 });
  await page.evaluate(() => document.getElementById('mv_make').click());
  await page.waitForFunction(() => document.getElementById('mv_make').textContent === 'Make movie here' &&
    document.getElementById('movie').style.display === 'block', null, { timeout: 90000 });
  const info = await page.evaluate(() => document.getElementById('mv_info').textContent);
  check(alerts.length === 1 && /^\d+ frames 32x32/.test(info), `the next movie renders on the new worker (${info.slice(0, 40)}…)`);
});
