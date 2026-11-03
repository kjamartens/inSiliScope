#!/usr/bin/env node
// SPDX-License-Identifier: BSD-3-Clause
// The Microscope view (the left view's Sample | Microscope switch; cli/scope_panels.h, web/live/scope_panels.js) in
// headless Chromium: the switch lays the module's panels over the world view (the light path on top, the tiles in a
// grid under it), each drawn; the light path lies down in the tile with the parts' names only, the square images of
// the tiles all one size; a pointer over a tile marks its parts in the light path; the light path filled shows its
// values, legend and clickable stations; a station fills the view with that panel at twice the size, Esc goes back;
// the PSF filled is a 2 x 2 grid with the x-z cut's angle; the wheel zooms an image (its inset, a click on it: whole); a narrow emission filter stops a dye's emission there; a
// laser switched off reaches the light path and the spectra; the choice survives a reload; Sample brings the world
// view back at its size, its cells loaded.
//   node tests/web/viewer_scope_view.mjs [--channel=chrome] [--swiftshader]
import { runViewer } from './browser.mjs';

await runViewer('viewer scope view', { watchdogSec: 180, viewport: { width: 1400, height: 900 } }, async ({ page, check }) => {
  await page.waitForFunction(() => typeof genWorkers !== 'undefined' && genWorkers.ok && typeof scopeView !== 'undefined', null, { timeout: 120000 });
  const cv0 = await page.evaluate(() => document.getElementById('cv').getBoundingClientRect().width);
  const TILES = '#scopeHost .pnl:not(.big)';
  // every tile drawn: none busy, each with its views
  const allDrawn = () => page.waitForFunction(sel => {
    const t = [...document.querySelectorAll(sel)];
    return t.length > 0 && t.every(p => !p.classList.contains('busy') && +p.dataset.views > 0 && !p.dataset.error);
  }, TILES, { timeout: 90000 });
  const bigDrawn = id => page.waitForFunction(id => { const b = document.querySelector('#scopeHost > .pnl.big');
    return b && b.dataset.panel === id && !b.classList.contains('busy') && +b.dataset.views > 0; }, id, { timeout: 60000 });
  const tiles = () => page.evaluate(sel => [...document.querySelectorAll(sel)].map(p => ({ id: p.dataset.panel, views: +p.dataset.views,
    lit: [...p.querySelectorAll('.pv-image canvas')].map(c => { const a = c.getContext('2d').getImageData(0, 0, c.width, c.height).data; let n = 0;
      for (let i = 0; i < a.length; i += 4) if (a[i] + a[i + 1] + a[i + 2] > 60) n++; return n; }) })), TILES);
  const path = () => page.evaluate(() => { const b = document.querySelector('#scopeHost .pnl[data-panel=lightpath] .pv-schematic .pvBody') || { dataset: {}, querySelectorAll: () => [] };
    return { orient: b.dataset.orient, detail: b.dataset.detail, lasers: +b.dataset.lasers, blocked: b.dataset.blocked, elements: +b.dataset.elements,
      beams: +b.dataset.beams, stations: [...b.querySelectorAll('[data-station]')].length, legend: !!b.querySelector('.pLegend, .pLeg'),
      spectra: +(document.querySelector('#scopeHost .pnl[data-panel=spectra] .pv-curves canvas') || { dataset: {} }).dataset.lasers }; });

  // the switch: the panels over the world view, which stays laid out (its cells keep loading)
  await page.evaluate(() => document.querySelector('#leftSeg button[data-left=scope]').click());
  await page.waitForFunction(() => +document.getElementById('scopeHost').dataset.panels > 0, null, { timeout: 60000 });
  await allDrawn();
  const sw = await page.evaluate(() => ({ scope: document.getElementById('worldCol').classList.contains('scope'),
    world: getComputedStyle(document.getElementById('canvasHost')).display, host: getComputedStyle(document.getElementById('scopeHost')).position,
    pressed: document.querySelector('#leftSeg button[data-left=scope]').getAttribute('aria-checked'),
    top: [...document.querySelectorAll('#scopeHost .sTop .pnl')].map(p => p.dataset.panel).join(','),
    grid: [...document.querySelectorAll('#scopeHost .sGrid .pnl')].map(p => p.dataset.panel).join(',') }));
  check(sw.scope && sw.world !== 'none' && sw.host === 'absolute' && sw.pressed === 'true' && sw.top === 'lightpath' && sw.grid.split(',').length === 4,
    `Microscope: the panels laid over the world view, the light path on top, four tiles under it ${JSON.stringify(sw)}`);
  const t1 = await tiles();
  const want = { lightpath: 1, illumination: 2, psf: 2, bfp: 2, spectra: 2 };   // the PSF tile: x-y (cropped) and x-z
  check(t1.length === 5 && t1.every(t => t.views === want[t.id] && t.lit.every(n => n > 20)),
    `five panels drawn, every image lit: ${t1.map(t => `${t.id} ${t.views} view(s)${t.lit.length ? ' [' + t.lit.join(', ') + ' px]' : ''}`).join('; ')}`);
  // the square images of the tiles (the illumination's x-y, the PSF's x-y, the BFP) one size
  const sq = await page.evaluate(sel => [...document.querySelectorAll(sel + ' .pv-image canvas')].map(c => [c.closest('.pnl').dataset.panel + '/' + c.closest('.pv').dataset.name,
    c.clientWidth, c.clientHeight]).filter(s => Math.abs(s[1] - s[2]) <= 1), TILES);
  check(sq.length === 3 && Math.max(...sq.map(s => s[1])) - Math.min(...sq.map(s => s[1])) <= 1 && sq[0][1] >= 80,
    `the tiles' square images one size: ${sq.map(s => s[0] + ' ' + s[1] + ' px').join(', ')}`);
  const p1 = await path();
  check(p1.orient === 'h' && !p1.detail && p1.stations === 0 && !p1.legend && p1.elements >= 8 && p1.beams >= 3 && p1.lasers === 2 && p1.spectra === 2,
    `the light path tile: lying down, names only (no stations, no legend); ${p1.elements} parts, ${p1.beams} beams, lasers ${p1.lasers} / spectra ${p1.spectra}`);

  // a pointer over a tile marks its parts in the light path
  const hl = id => page.evaluate(id => { const t = document.querySelector(`#scopeHost .pnl:not(.big)[data-panel=${id}]`);
    t.dispatchEvent(new PointerEvent('pointerenter')); const k = [...new Set([...document.querySelectorAll('#scopeHost .pDraw g.pHl')].map(e => e.dataset.kind))].sort().join(',');
    t.dispatchEvent(new PointerEvent('pointerleave')); return k; }, id);
  const hPsf = await hl('psf'), hSpec = await hl('spectra');
  const left = await page.evaluate(() => document.querySelectorAll('#scopeHost .pDraw .pHl').length);
  check(hPsf === 'camera,objective' && /sample/.test(hSpec) && /dichroic/.test(hSpec) && /camera/.test(hSpec) && left === 0,
    `hover: the PSF marks ${hPsf}; the spectra mark ${hSpec}; none after the pointer leaves`);

  // the light path filled: its values, legend and stations; a station fills the view with its panel, Esc goes back
  await page.evaluate(() => document.querySelector('#scopeHost .pnl[data-panel=lightpath] .pnlBtn').click());
  await bigDrawn('lightpath');
  const pb = await page.evaluate(() => { const b = document.querySelector('#scopeHost > .pnl.big .pv-schematic .pvBody');
    return { detail: b.dataset.detail, legend: b.children.length > 1, stations: [...new Set([...b.querySelectorAll('[data-station]')].map(e => e.dataset.station))].sort().join(',') }; });
  check(pb.detail === '1' && pb.legend && /illumination/.test(pb.stations) && /psf/.test(pb.stations) && /spectra/.test(pb.stations) && /bfp/.test(pb.stations),
    `the light path filled: values and legend, stations ${pb.stations}`);
  await page.evaluate(() => document.querySelector('#scopeHost > .pnl.big [data-station=illumination]').dispatchEvent(new MouseEvent('click', { bubbles: true })));
  await bigDrawn('illumination');
  const f = await page.evaluate(() => { const h = document.getElementById('scopeHost'), b = h.querySelector(':scope > .pnl.big');
    return { filled: h.dataset.filled, w: b._pLast[0].header.views.find(v => v.name === 'xy').w, tiles: [...h.querySelectorAll('.sTop, .sGrid')].filter(c => getComputedStyle(c).display !== 'none').length,
      cap: b.querySelector('.pnlCap').textContent, width: Math.round(b.getBoundingClientRect().width), hostW: Math.round(h.clientWidth) }; });
  check(f.filled === 'illumination' && f.w === 320 && f.tiles === 0 && f.width > f.hostW - 40 && /the section at z/.test(f.cap),
    `a click on the light path's sample fills the view with the illumination at twice the size ${JSON.stringify(f)}`);
  await page.keyboard.press('Escape');
  const back = await page.evaluate(sel => ({ filled: document.getElementById('scopeHost').classList.contains('filled'), tiles: document.querySelectorAll(sel).length }), TILES);
  check(!back.filled && back.tiles === 5, `Esc: every panel again ${JSON.stringify(back)}`);

  // the PSF filled: a 2 x 2 grid (x-y and x-z, the kernel above the camera pixels), the rows' images level, the cut's angle
  await page.evaluate(() => document.querySelector('#scopeHost .pnl[data-panel=psf] .pnlBtn').click());
  await bigDrawn('psf');
  const psf = await page.evaluate(() => { const b = document.querySelector('#scopeHost > .pnl.big'), r = n => b.querySelector(`.pv[data-name=${n}] canvas`).getBoundingClientRect();
    return { views: [...b.querySelectorAll('.pv')].map(f => f.dataset.name).join(','), grid: b.querySelector('.pnlViews').classList.contains('grid'),
      ctl: [...b.querySelectorAll('.pnlCtl input')].map(i => i.dataset.name).join(','), top: [r('kernel').bottom, r('xz').bottom].map(Math.round),
      bottom: [r('camera').bottom, r('camxz').bottom].map(Math.round), left: [r('kernel').left, r('camera').left].map(Math.round) }; });
  check(psf.views === 'kernel,xz,camera,camxz' && psf.grid && psf.ctl === 'dz,angle' && Math.abs(psf.top[0] - psf.top[1]) <= 4 &&
    Math.abs(psf.bottom[0] - psf.bottom[1]) <= 4 && Math.abs(psf.left[0] - psf.left[1]) <= 1 && psf.bottom[0] > psf.top[0],
    `the PSF filled: 2 x 2 ${JSON.stringify(psf)}`);
  const cap0 = await page.evaluate(() => document.querySelector('#scopeHost > .pnl.big .pv[data-name=xz] figcaption').textContent);
  await page.evaluate(() => { const i = document.querySelector('#scopeHost > .pnl.big .pnlCtl input[data-name=angle]'); i.value = 45; i.dispatchEvent(new Event('input')); });
  const cap1 = await page.waitForFunction(() => { const c = document.querySelector('#scopeHost > .pnl.big .pv[data-name=xz] figcaption'); return c && /45/.test(c.textContent) && c.textContent; },
    null, { timeout: 30000 }).then(h => h.jsonValue(), () => '');
  check(/^x.z, oversampled/.test(cap0) && /45/.test(cap1), `the x-z cut turns: "${cap0}" -> "${cap1}"`);
  // the wheel over an image zooms it about the pointer, the inset shows the part shown; a click on the inset: the whole image
  const kr = await page.evaluate(() => { const r = document.querySelector('#scopeHost > .pnl.big .pv[data-name=kernel] canvas').getBoundingClientRect();
    return { x: r.left + r.width / 2, y: r.top + r.height / 2 }; });
  await page.mouse.move(kr.x, kr.y);
  await page.mouse.wheel(0, -400);
  const zoomed = await page.waitForFunction(() => { const f = document.querySelector('#scopeHost > .pnl.big .pv[data-name=kernel]'), c = f.querySelector('canvas');
    const cam = document.querySelector('#scopeHost > .pnl.big .pv[data-name=camera] canvas');   // linked: the camera's x-y zooms with it
    return c._zoom && c._zoom.z > 2 && !f.querySelector('.pvNav').hidden && Math.abs(cam._zoom.z - c._zoom.z) < 1e-9 &&
      { z: +c._zoom.z.toFixed(2), x: +c._zoom.x.toFixed(3), y: +c._zoom.y.toFixed(3), cam: +cam._zoom.z.toFixed(2), camX: +cam._zoom.x.toFixed(3) }; },
    null, { timeout: 10000 }).then(h => h.jsonValue(), () => null);
  const unzoomed = await page.evaluate(() => { const f = document.querySelector('#scopeHost > .pnl.big .pv[data-name=kernel]'); f.querySelector('.pvNav').click();
    return f.querySelector('canvas')._zoom.z === 1 && f.querySelector('.pvNav').hidden &&
      document.querySelector('#scopeHost > .pnl.big .pv[data-name=camera] canvas')._zoom.z === 1; });
  check(!!zoomed && Math.abs(zoomed.x + 0.5 / zoomed.z - 0.5) < 0.02 && unzoomed,
    `the wheel zooms the kernel about the pointer, the camera's x-y with it ${JSON.stringify(zoomed)}; the inset's click shows both whole (${unzoomed})`);
  await page.keyboard.press('Escape');

  // a narrow emission filter (600/37) stops a far-red dye's emission there
  const em0 = await page.evaluate(() => document.getElementById('mv_em-filter').value);
  const narrow = await page.evaluate(() => { const e = document.getElementById('mv_em-filter'), o = [...e.options].find(o => /FF01-600\/37/.test(o.text));
    if (!o) return ''; e.value = o.value; e.dispatchEvent(new Event('change', { bubbles: true })); return o.text; });
  const blocked = narrow && await page.waitForFunction(() => { const b = document.querySelector('#scopeHost .pnl[data-panel=lightpath] .pv-schematic .pvBody');
    return b && /emission@emfilter/.test(b.dataset.blocked || '') && b.dataset.blocked; }, null, { timeout: 60000 }).then(h => h.jsonValue(), () => '');
  await page.evaluate(v => { const e = document.getElementById('mv_em-filter'); e.value = v; e.dispatchEvent(new Event('change', { bubbles: true })); }, em0);
  check(!!blocked, `the emission filter ${narrow}: the light path stops the emission there (${blocked || (await path()).blocked})`);

  // a laser off reaches the light path and the spectra
  const off = await page.evaluate(() => { const el = document.getElementById('mv_laser-561'); if (!el) return false;
    el.value = 0; el.dispatchEvent(new Event('input', { bubbles: true })); el.dispatchEvent(new Event('change', { bubbles: true })); return true; });
  const l2 = off && await page.waitForFunction(() => {
    const p = document.querySelector('#scopeHost .pnl[data-panel=lightpath] .pv-schematic .pvBody'), s = document.querySelector('#scopeHost .pnl[data-panel=spectra] .pv-curves canvas');
    return p && s && +p.dataset.lasers === 1 && +s.dataset.lasers === 1 && { path: 1, spectra: 1 };
  }, null, { timeout: 60000 }).then(h => h.jsonValue(), () => null);
  check(!!l2, `the 561 nm laser off: one laser in the light path and the spectra (${JSON.stringify(l2 || await path())})`);

  // kept over a reload
  await page.reload();
  await page.waitForFunction(() => typeof scopeView !== 'undefined' && scopeView.on && +document.getElementById('scopeHost').dataset.panels === 5, null, { timeout: 120000 });
  await allDrawn();
  check(true, 'the Microscope view comes back after a reload, every panel drawn');

  // Sample: the world view back at its size, its cells loaded (they load under the panels too)
  await page.evaluate(() => document.querySelector('#leftSeg button[data-left=sample]').click());
  await page.waitForFunction(() => getComputedStyle(document.getElementById('scopeHost')).display === 'none' && +document.getElementById('hud').dataset.cells > 0,
    null, { timeout: 90000 });
  await new Promise(r => setTimeout(r, 300));
  const w = await page.evaluate(() => ({ cv: document.getElementById('cv').getBoundingClientRect().width, host: getComputedStyle(document.getElementById('scopeHost')).display,
    cells: +document.getElementById('hud').dataset.cells, kept: localStorage.getItem('isc.ui.leftView'), left: document.querySelectorAll('#worldCol .pnl:not([hidden])').length,
    visible: [...document.querySelectorAll('#scopeHost .pnl')].filter(p => p.getBoundingClientRect().width > 0).length }));
  check(Math.abs(w.cv - cv0) < 2 && w.host === 'none' && w.visible === 0 && w.cells > 0 && w.kept === 'sample',
    `Sample: the world view back at ${w.cv.toFixed(0)} px (was ${cv0.toFixed(0)}), ${w.cells} cells, no panel left on screen ${JSON.stringify(w)}`);
});
