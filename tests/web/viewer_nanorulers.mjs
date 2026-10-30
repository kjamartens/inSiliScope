#!/usr/bin/env node
// SPDX-License-Identifier: BSD-3-Clause
// The viewer's Sample select in headless Chromium: the DNA-origami nanorulers (a nanoruler world: no cells, the rods
// drawn from above with their docking-strand dyes; a movie spec carries the specimen; zoomed out, the dyes in the
// camera's field of view all drawn, and only those), then back to the cell field; then the bead sample (only the
// fiducial beads, their aggregation, light, height, diameter) and back.
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
  // The bead sample (2026-10-30): no cells or labels, mounting it sets 0.3 beads/um^2 and 40 % aggregation, OnTop beads
  // on the glass (no OnTop offered), 561 nm at 0.1 kW/cm^2 and the other lasers off, draws them and opens their group,
  // the diameter and the layer thickness Basic; the movie spec carries specimen=2 and the aggregation; the density
  // counts beads whatever the aggregation; a type pick sets the diameter.
  await set('mv_specimen', '2');
  await page.waitForFunction(() => +document.getElementById('hud').dataset.beads > 0, null, { timeout: 120000 });
  const bs = await page.evaluate(async () => {
    const hud = document.getElementById('hud'), spec = movieSpec();
    const groups = [...document.querySelectorAll('details.grp')].filter(g => g.style.display !== 'none').map(g => g.dataset.group);
    const out = { dens: +document.getElementById('mv_fid-density').value, cells: hud.dataset.cells, beads: +hud.dataset.beads,
      open: document.querySelector('details.grp[data-group="fiducials"]').open, groups, loading: !!document.getElementById('loading'),
      spec: /(^| )specimen=2( |$)/.test(spec) && /(^| )fid-density=0.3( |$)/.test(spec) && / fid-aggregation-pct=40( |$)/.test(spec) };
    const $ = id => document.getElementById(id), rowOf = id => $(id).closest('.opt');
    out.height = $('mv_fid-height').value;
    out.onTopHidden = $('mv_fid-height').querySelector('option[value="1"]').hidden;
    out.lasers = Object.fromEntries([...document.querySelectorAll('[id^="mv_laser-"]')].filter(e => /^mv_laser-\d+$/.test(e.id)).map(e => [e.id.slice(9), +e.value]));
    out.basicRows = ['mv_fid-diameter-nm', 'mv_fid-layer-um'].every(id => rowOf(id) && !rowOf(id).classList.contains('t-advanced'));
    const type = $('mv_fid-type');
    type.value = String(ISC_DYE_DATA.fiducials.types.findIndex(t => t.id === 'FluoSpheresRed'));
    type.dispatchEvent(new Event('input', { bubbles: true }));
    out.diamRed = +$('mv_fid-diameter-nm').value;
    type.value = String(ISC_DYE_DATA.fiducials.types.findIndex(t => t.id === 'Gold'));
    type.dispatchEvent(new Event('input', { bubbles: true }));
    out.diamGold = +$('mv_fid-diameter-nm').value;
    const agg = document.getElementById('mv_fid-aggregation-pct');
    agg.value = 0; agg.dispatchEvent(new Event('input', { bubbles: true }));
    const key = fidCache && fidCache.key;
    for (let i = 0; i < 300 && !(fidCache && fidCache.key !== key && +hud.dataset.beads > 0); i++) { await new Promise(r => setTimeout(r, 100)); requestDraw(); }
    out.beads0 = +hud.dataset.beads;
    out.aggSpec = / fid-aggregation-pct=0( |$)/.test(movieSpec());
    return out;
  });
  check(bs.dens === 0.3 && bs.cells === '' && bs.beads > 0 && bs.open && !bs.loading && bs.spec && bs.aggSpec &&
    !bs.groups.some(g => /label$/.test(g)) && Math.abs(bs.beads0 / bs.beads - 1) < 0.3,
    `bead sample: 0.3 beads/um^2 and 40 % set at the mount, ${bs.beads} beads, no cells or labels (groups ${bs.groups.join(' ')}), ` +
    `aggregation 0 %: ${bs.beads0} beads, the movie spec has specimen=2 and the aggregation`);
  check(bs.height === '0' && bs.onTopHidden, `bead sample: OnTop beads on the glass (height ${bs.height}), OnTop not offered`);
  check(Object.entries(bs.lasers).every(([nm, kw]) => kw === (nm === '561' ? 0.1 : 0)),
    `bead sample: 561 nm at 0.1 kW/cm^2, the others off (${JSON.stringify(bs.lasers)})`);
  check(bs.basicRows && bs.diamRed === 200 && bs.diamGold === 100,
    `bead sample: diameter and layer thickness Basic; a type pick sets the diameter (red ${bs.diamRed}, gold ${bs.diamGold} nm)`);
  await set('mv_specimen', '0');
  await page.waitForFunction(() => +document.getElementById('hud').dataset.cells > 0, null, { timeout: 120000 });
  const back = await page.evaluate(() => ({ onTopHidden: document.querySelector('#mv_fid-height option[value="1"]').hidden,
    adv: document.getElementById('mv_fid-diameter-nm').closest('.opt').classList.contains('t-advanced') }));
  check(!back.onTopHidden && back.adv, `bead sample: back to the cell field, cells again, OnTop offered, the diameter Advanced`);

});
