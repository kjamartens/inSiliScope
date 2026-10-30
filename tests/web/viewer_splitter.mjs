#!/usr/bin/env node
// SPDX-License-Identifier: BSD-3-Clause
// The splitter between the world view and the live panel: a width dragged there is kept over a reload, and the world
// view's canvases (the view and the x-z side view) are measured at that width, not at the default 50 %. On a sample
// without cells (here the nanorulers) nothing else re-measures them after the load, so they used to spill under the
// live panel and over the splitter.
//   node tests/web/viewer_splitter.mjs [--channel=chrome] [--swiftshader]   (~13 s)
import { runViewer } from './browser.mjs';

await runViewer('viewer splitter', { watchdogSec: 120, viewport: { width: 1300, height: 800 } }, async ({ page, check }) => {
  const meas = () => page.evaluate(() => {
    const m = id => { const r = document.getElementById(id).getBoundingClientRect(); return { left: Math.round(r.left), w: Math.round(r.width) }; };
    return { host: m('canvasHost'), cv: m('cv'), xz: m('cvXZ'), split: m('splitter') };
  });
  await page.evaluate(() => { const el = document.getElementById('mv_specimen'); el.value = '1';
    el.dispatchEvent(new Event('input', { bubbles: true })); el.dispatchEvent(new Event('change', { bubbles: true })); });
  await page.waitForFunction(() => +document.getElementById('hud').dataset.rulers > 0, null, { timeout: 60000 });
  // drag the splitter to the left: a wide live panel (62 %, the maximum)
  const b = await (await page.$('#splitter')).boundingBox();
  await page.mouse.move(b.x + 3, b.y + 300); await page.mouse.down();
  await page.mouse.move(400, b.y + 300, { steps: 5 }); await page.mouse.up();
  const dragged = await meas();
  check(dragged.split.left < b.x - 100 && dragged.cv.w === dragged.host.w, `dragged: the world view ${dragged.host.w} px, its canvas ${dragged.cv.w} px`);
  await page.reload();
  await page.waitForFunction(() => +document.getElementById('hud').dataset.rulers > 0, null, { timeout: 60000 });
  const r = await meas();
  check(Math.abs(r.split.left - dragged.split.left) <= 1, `reloaded: the splitter where it was dragged (${r.split.left} px)`);
  check(r.cv.left + r.cv.w <= r.split.left + 1, `reloaded: the world canvas (${r.cv.w} px) ends at the splitter (${r.split.left} px)`);
  check(r.xz.w === 0 || r.xz.left + r.xz.w <= r.split.left + 1, `reloaded: the x-z canvas (${r.xz.w} px) ends at the splitter`);
});
