#!/usr/bin/env node
// SPDX-License-Identifier: BSD-3-Clause
// The splitter between the world view and the live panel: a width dragged there is kept over a reload, and the world
// view's canvases (the view and the x-z side view) are measured at that width, not at the default 38 %. On a sample
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
  // drag the splitter to the left: a wide live panel (69 %; the maximum is 72 %)
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
  // the settings column (folded at launch, 2026-11-13) opened and folded: the splitter and the world view stay, the live
  // image gives the column its room; a drag follows the pointer either way
  const live = () => page.evaluate(() => Math.round(document.getElementById('liveView').getBoundingClientRect().width));
  const w0 = await live();
  await page.click('#setTab'); await page.waitForTimeout(300);
  const f = await meas(), w1 = await live();
  check(w1 < w0 - 100 && f.split.left === r.split.left && f.cv.w === r.cv.w,
    `opened: the live image ${w0} -> ${w1} px, the splitter at ${f.split.left} px, the world canvas ${f.cv.w} px (both as folded)`);
  const fb = await (await page.$('#splitter')).boundingBox();
  await page.mouse.move(fb.x + 3, fb.y + 300); await page.mouse.down();
  await page.mouse.move(900, fb.y + 300, { steps: 5 }); await page.mouse.up();
  const g = await meas();
  check(Math.abs(g.split.left + 6 - 900) <= 1, `open drag: the panel starts at the pointer (${g.split.left} px)`);
  await page.click('#setFold'); await page.waitForTimeout(300);
  const h = await meas();
  check(h.split.left === g.split.left && h.cv.w === g.cv.w, `folded again: the splitter and the world view stay (${h.split.left} px)`);
});
