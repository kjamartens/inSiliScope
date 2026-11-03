#!/usr/bin/env node
// SPDX-License-Identifier: BSD-3-Clause
// Structured illumination in the viewer (spec/ALGORITHM.md "Structured illumination") in headless Chromium: a Pattern
// pick sets the BFP position to the type's radius and the polarization to S and goes into the movie spec; the readout
// and the Beams matrix come from the setup job (the type's rows); Preview illumination opens the dialog with the
// program's period and beam count, its slider steps the frame, the height slider moves the section seen from below
// (captions as page text), the open dialog follows a settings change; a snap's info carries the pattern state and the
// live sample's counter steps per frame; LatticeSIM3D's five beams and its section's change with height; Off again.
//   node tests/web/viewer_sim.mjs [--channel=chrome] [--swiftshader]
import { runViewer } from './browser.mjs';

await runViewer('viewer sim', { watchdogSec: 180 }, async ({ page, check }) => {
  const set = (id, v) => page.evaluate(([i, x]) => { const el = document.getElementById(i); el.value = x;
    el.dispatchEvent(new Event('input', { bubbles: true })); el.dispatchEvent(new Event('change', { bubbles: true })); }, [id, v]);
  await page.waitForFunction(() => window.liveState && typeof genWorkers !== 'undefined' && genWorkers.ok &&
    self.ISC_ENGINE_OPTIONS && self.ISC_ENGINE_OPTIONS.size > 100, null, { timeout: 120000 });

  // SIM2D: the pick's couplings and the spec
  await set('mv_sim-type', '1');
  const r = await page.evaluate(() => ({ bfp: +document.getElementById('mv_illum-bfp').value, pol: document.getElementById('mv_sim-polarization').value,
    spec: movieSpec(), level: document.body.dataset.level, basic: !document.getElementById('mv_sim-type').closest('.opt').classList.contains('t-advanced') }));
  check(r.bfp === 0.95 && r.pol === '0' && / sim-type=1( |$)/.test(r.spec) && / illum-bfp=0.95( |$)/.test(r.spec) && r.basic,
    `SIM2D pick: BFP position 0.95, polarization S, sim-type=1 in the spec (level ${r.level})`);
  await page.waitForFunction(() => /period/.test(document.getElementById('patternReadout').textContent) &&
    document.getElementById('simBeams').value.split(';').length === 2, null, { timeout: 90000 });
  const ro = await page.evaluate(() => ({ text: document.getElementById('patternReadout').textContent, beams: document.getElementById('simBeams').value }));
  check(/9 frames|frames per cycle|per cycle/.test(ro.text) && /^0\.95 0 /.test(ro.beams.trim()) && / -0\.95 0 /.test(ro.beams),
    `the readout (${ro.text.slice(0, 80)}) and the two rows at ±0.95`);

  // Preview illumination: the dialog, its period and beams, the slider
  await page.evaluate(() => document.getElementById('illumPreview').click());
  await page.waitForFunction(() => { const d = document.getElementById('illumDlg'); return d.style.display !== 'none' && d.dataset.period === '9'; }, null, { timeout: 90000 });
  const d0 = await page.evaluate(() => { const d = document.getElementById('illumDlg'); return { beams: d.dataset.beams, frame: d.dataset.frame, state: document.getElementById('il_state').textContent,
    px: ['il_bfp', 'il_xy', 'il_xz'].map(id => { const c = document.getElementById(id); const a = c.getContext('2d').getImageData(0, 0, c.width, c.height).data; let n = 0; for (let i = 0; i < a.length; i += 4) if (a[i] + a[i + 1] + a[i + 2] > 60) n++; return n; }) }; });
  check(d0.beams === '2' && d0.frame === '0' && /frame 0 of 9/.test(d0.state) && d0.px.every(n => n > 100),
    `the preview: 2 beams, ${d0.state}, the three views drawn (${d0.px.join(', ')} lit px)`);
  await page.evaluate(() => { const s = document.getElementById('il_slider'); s.value = 4; s.dispatchEvent(new Event('input')); });
  await page.waitForFunction(() => document.getElementById('illumDlg').dataset.frame === '4' && /frame 4 of 9: angle 1, phase 1/.test(document.getElementById('il_state').textContent), null, { timeout: 90000 });
  check(true, 'the slider steps the preview to frame 4 (angle 1, phase 1)');
  // the captions are page text; the section starts at the focus (0.5 um) and the height slider moves it
  const cap = await page.evaluate(() => ({ z: document.getElementById('illumDlg').dataset.z, t: document.getElementById('il_xy_t').textContent,
    zv: document.getElementById('il_zval').textContent, n: document.getElementById('il_xz_n').textContent }));
  check(cap.z === '0.50' && /Section at z = 0\.50 µm/.test(cap.t) && /the focus/.test(cap.zv) && /dashed line/.test(cap.n),
    `the captions: "${cap.t}", "${cap.zv}"; the side note "${cap.n.slice(0, 40)}…"`);
  await page.evaluate(() => { const s = document.getElementById('il_z'); s.value = 1.2; s.dispatchEvent(new Event('input')); });
  await page.waitForFunction(() => document.getElementById('illumDlg').dataset.z === '1.20' && /z = 1\.20/.test(document.getElementById('il_xy_t').textContent), null, { timeout: 90000 });
  check(true, 'the height slider moves the section to 1.20 um (its caption follows)');
  // closed and clicked again with the same settings: the same picture shows again, no job (it used to do nothing)
  const again = await page.evaluate(async () => {
    const d = document.getElementById('illumDlg'), id0 = illumJobId;
    document.getElementById('il_close').click();
    document.getElementById('illumPreview').click();
    return { open: d.style.display === 'block', jobs: illumJobId - id0, z: d.dataset.z };
  });
  check(again.open && again.jobs === 0 && again.z === '1.20', `closed, then clicked with the same settings: open again at once (${JSON.stringify(again)})`);
  // closed while a settings change redraws it: stays closed; the next click shows that redraw
  const caught = await page.evaluate(async () => {
    const el = document.getElementById('mv_laser-640');
    el.value = 0.7; el.dispatchEvent(new Event('input', { bubbles: true }));
    for (let t = 0; t < 2000 && !illumJob; t += 5) await new Promise(r => setTimeout(r, 5));
    const inFlight = !!illumJob;
    document.getElementById('il_close').click();
    return inFlight;
  });
  check(caught, 'a settings change starts a redraw of the open dialog');
  await page.waitForFunction(() => !illumJob, null, { timeout: 90000 });
  await new Promise(r => setTimeout(r, 300));
  const late = await page.evaluate(() => {
    const d = document.getElementById('illumDlg'), shut = d.style.display !== 'block', id0 = illumJobId;
    document.getElementById('illumPreview').click();
    return { shut, open: d.style.display === 'block', jobs: illumJobId - id0, current: illumKey.startsWith(movieSpec({ frames: 1 }) + '|') };
  });
  check(late.shut && late.open && late.jobs === 0 && late.current, `closed during a redraw: stays closed, the next click shows the redraw (${JSON.stringify(late)})`);
  await page.evaluate(() => { illumZ = -1; });   // back to the focus for the rest
  await page.evaluate(() => document.getElementById('il_close').click());

  // a snap: the frame's pattern state in the info, the sample's counter stepping
  const snap = async () => {
    await page.waitForFunction(() => !liveState.pending, null, { timeout: 90000 });
    const n = await page.evaluate(() => liveState.frames);
    await page.evaluate(() => document.getElementById('lv_snap').click());
    await page.waitForFunction(k => liveState.frames > k && !liveState.pending, n, { timeout: 120000 });
    return page.evaluate(() => ({ p: liveState.pattern, info: document.getElementById('lv_info').textContent }));
  };
  const s1 = await snap(), s2 = await snap();
  check(s1.p && s1.p.period === 9 && s2.p && s2.p.frame === s1.p.frame + 1 && /pattern \d+\/9/.test(s2.info),
    `snaps: pattern state ${JSON.stringify(s1.p)} then frame ${s2.p && s2.p.frame} (info "${s2.info.split('·').pop().trim()}")`);

  // LatticeSIM3D: five beams, 13 frames, the radius 0.9 -- the open dialog follows the pick by itself
  await page.evaluate(() => document.getElementById('illumPreview').click());
  await page.waitForFunction(() => { const d = document.getElementById('illumDlg'); return d.style.display !== 'none' && d.dataset.period === '9'; }, null, { timeout: 90000 });
  await set('mv_sim-type', '4');
  await page.waitForFunction(() => { const d = document.getElementById('illumDlg'); return d.style.display !== 'none' && d.dataset.period === '13' && d.dataset.beams === '5' && d.dataset.z === '0.50'; }, null, { timeout: 90000 });
  const l = await page.evaluate(() => ({ bfp: +document.getElementById('mv_illum-bfp').value, title: document.getElementById('il_title').textContent }));
  check(l.bfp === 0.9 && /LatticeSIM3D/.test(l.title), `LatticeSIM3D: 5 beams, 13 frames, BFP position 0.9 (${l.title}), the dialog updated without a click`);
  // the lattice's section changes with height (the centre beam's fringes swap direction every quarter axial period)
  const fp = () => page.evaluate(() => { const c = document.getElementById('il_xy'), a = c.getContext('2d').getImageData(0, 0, c.width, c.height).data; let h = 0; for (let i = 0; i < a.length; i += 4) h = (h * 31 + a[i]) >>> 0; return h; });
  const f0 = await fp();
  await page.evaluate(() => { const s = document.getElementById('il_z'); s.value = 0.66; s.dispatchEvent(new Event('input')); });
  await page.waitForFunction(() => document.getElementById('illumDlg').dataset.z === '0.66', null, { timeout: 90000 });
  const f1 = await fp();
  check(f0 !== f1, `the lattice's section at 0.66 um differs from the focus's (${f0} vs ${f1})`);
  await page.evaluate(() => { illumZ = -1; });
  await page.evaluate(() => document.getElementById('il_close').click());

  // Off: one beam, no pattern in the spec
  await set('mv_sim-type', '0');
  await page.waitForFunction(() => /one beam/.test(document.getElementById('patternReadout').textContent), null, { timeout: 90000 });
  const off = await page.evaluate(() => ({ spec: movieSpec(), rows: document.getElementById('simBeams').value.split(';').length }));
  check(/ sim-type=0( |$)/.test(off.spec) && off.rows === 1, 'Off: one beam row, sim-type=0');
});
