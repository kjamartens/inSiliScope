#!/usr/bin/env node
// SPDX-License-Identifier: BSD-3-Clause
// The viewer's Sample select in headless Chromium: the DNA-origami nanorulers (a nanoruler world: no cells, the rods
// drawn from above with their docking-strand dyes; a movie spec carries the specimen; zoomed out, the dyes in the
// camera's field of view all drawn, and only those), then back to the cell field.
//   node tests/web/viewer_nanorulers.mjs [--channel=chrome] [--swiftshader]
import { runViewer } from './browser.mjs';

await runViewer('viewer nanorulers', { watchdogSec: 120 }, async ({ page, check }) => {
  // The Sample select: DNA-origami nanorulers (a nanoruler world: no cells, the rods drawn from above with their
  // docking-strand dyes; a movie spec carries the specimen), then back to the cell field
  const set = (id, v) => page.evaluate(([i, x]) => { const el = document.getElementById(i); el.value = x;
    el.dispatchEvent(new Event('input', { bubbles: true })); el.dispatchEvent(new Event('change', { bubbles: true })); }, [id, v]);
  await set('mv_specimen', '1');
  await page.waitForFunction(() => +document.getElementById('hud').dataset.rulers > 0 && dyeSel && dyeSel.n > 0, null, { timeout: 120000 });
  const nr = await page.evaluate(() => {
    const d = cvBase.getContext('2d').getImageData(0, 0, cvBase.width, cvBase.height).data;
    let rod = 0;
    for (let i = 0; i < d.length; i += 4) if (d[i + 3] > 0) rod++;
    const spec = movieSpec(), groups = [...document.querySelectorAll('details.grp')].filter(g => g.style.display !== 'none').map(g => g.dataset.group);
    return { hud: 'nanorulers ' + document.getElementById('hud').dataset.rulers, rod, dyes: dyeSel.n, scale: view.scale,
      spec: /(^| )specimen=1( |$)/.test(spec) && /(^| )p\.origamiLpNm=1500( |$)/.test(spec) && /(^| )ruler-dye=/.test(spec), groups };
  });
  check(nr.rod > 100 && nr.dyes > 0 && nr.scale >= 150 && nr.spec && nr.groups.includes('origami') && nr.groups.includes('rulerlabel') &&
    !nr.groups.includes('mtlabel') && !nr.groups.includes('shape'),
    `nanorulers: ${nr.hud}, ${nr.rod} rod px, ${nr.dyes} dyes, the movie spec has specimen=1 (groups ${nr.groups.join(' ')})`);
  // zoomed out (below the dyes' zoom), the dyes in the camera's field of view are still all drawn, and only those
  const nrOut = await page.evaluate(async () => {
    const all = dyeSel.n;
    view.scale = 20; requestDraw();
    for (let i = 0; i < 300 && !(dyeSel && /\|[-0-9.]+,/.test(dyeSel.key)); i++) await new Promise(r => setTimeout(r, 100));
    const c = cameraRectUm(), s = dyeSel;
    let out = 0;
    for (let i = 0; s && i < s.pts.length; i += 4) { const x = s.pts[i] + s.ox, y = s.pts[i + 1] + s.oy; if (x < c[0] - 1e-6 || y < c[1] - 1e-6 || x > c[2] + 1e-6 || y > c[3] + 1e-6) out++; }
    const n = s ? s.n : 0;
    view.scale = 150; requestDraw();
    return { n, out, all };
  });
  check(nrOut.n > 0 && nrOut.out === 0, `nanorulers zoomed out: ${nrOut.n} dyes drawn, all in the camera (${nrOut.out} outside; ${nrOut.all} in the zoomed-in view)`);
  await set('mv_specimen', '0');
  await page.waitForFunction(() => +document.getElementById('hud').dataset.cells > 0, null, { timeout: 120000 });
  check(true, 'nanorulers: back to the cell field, cells again');

});
