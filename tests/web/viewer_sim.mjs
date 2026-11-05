#!/usr/bin/env node
// SPDX-License-Identifier: BSD-3-Clause
// Structured illumination in the viewer (spec/ALGORITHM.md "Structured illumination") in headless Chromium: a Pattern
// pick sets the BFP position to the type's radius and the polarization to S and goes into the movie spec; the readout
// and the Beams matrix come from the setup job (the type's rows); the Microscope view's illumination panel (filling the
// left view) shows the program's period and beam count, its slider steps the frame, the height slider moves the section
// seen from below, Esc and back in shows it at once, the open panel follows a settings change; a snap's info carries the
// pattern state and the live sample's counter steps per frame; LatticeSIM3D's five beams and its section's change with
// height; Off again.
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

  // The Microscope view's illumination panel, filling the left view: the period and beams, the frame slider, the height
  const big = () => page.evaluate(() => { const b = document.querySelector('#scopeHost > .pnl.big[data-panel=illumination]');
    return b ? { busy: b.classList.contains('busy'), cap: b.querySelector('.pnlCap').textContent, title: b.querySelector('.pnlTitle').textContent } : null; });
  const waitCap = (re, why) => page.waitForFunction(r => { const b = document.querySelector('#scopeHost > .pnl.big[data-panel=illumination]');
    return b && !b.classList.contains('busy') && new RegExp(r).test(b.querySelector('.pnlCap').textContent); }, re.source, { timeout: 90000 })
    .then(() => true, async () => { check(false, why + ': ' + JSON.stringify(await big())); return false; });
  const ctl = (name, v) => page.evaluate(([n, x]) => { const s = document.querySelector(`#scopeHost > .pnl.big .pnlCtl input[data-name=${n}]`);
    s.value = x; s.dispatchEvent(new Event('input')); }, [name, v]);
  const reset = name => page.evaluate(n => document.querySelector(`#scopeHost > .pnl.big .pnlCtl input[data-name=${n}]`).nextElementSibling
    .dispatchEvent(new MouseEvent('dblclick')), name);
  const lit = () => page.evaluate(() => [...document.querySelectorAll('#scopeHost > .pnl.big canvas')].map(c => {
    const a = c.getContext('2d').getImageData(0, 0, c.width, c.height).data; let n = 0; for (let i = 0; i < a.length; i += 4) if (a[i] + a[i + 1] + a[i + 2] > 60) n++; return n; }));
  await page.evaluate(() => scopeView.set(true));
  await page.waitForFunction(() => +document.getElementById('scopeHost').dataset.panels > 0, null, { timeout: 60000 });
  await page.evaluate(() => scopeView.open('illumination'));
  if (await waitCap(/^frame 0 of 9: angle 0, phase 0 \(2 beams\); the section at z = 0\.50 µm \(the focus\)$/, 'the illumination panel: frame 0 of 9, 2 beams, at the focus')) {
    const px = await lit(), b0 = await big();
    check(px.length === 2 && px.every(n => n > 100) && /SIM2D, BFP position 0\.95/.test(b0.title), `the illumination panel filling the view: "${b0.cap}", "${b0.title}" (${px.join(', ')} lit px)`);
  }
  await ctl('frame', 4);
  if (await waitCap(/frame 4 of 9: angle 1, phase 1/, 'the frame slider')) check(true, 'the frame slider steps the panel to frame 4 (angle 1, phase 1)');
  await ctl('z', 1.2);
  if (await waitCap(/the section at z = 1\.20 µm$/, 'the height slider')) {
    const t = await page.evaluate(() => document.querySelector('#scopeHost > .pnl.big .pv[data-name=xy] figcaption').textContent);
    check(/Section at z = 1\.20 µm/.test(t), `the height slider moves the section to 1.20 um (its view's caption: "${t}")`);
  }
  // back to every panel and in again with the same settings: shown at once, no job
  const again = await page.evaluate(async () => {
    document.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape' }));
    const closed = !scopeView.filled && !document.getElementById('scopeHost').classList.contains('filled');
    scopeView.open('illumination');
    await new Promise(r => setTimeout(r, 60));
    const b = document.querySelector('#scopeHost > .pnl.big');
    return { closed, busy: b.classList.contains('busy'), cap: b.querySelector('.pnlCap').textContent };
  });
  check(again.closed && !again.busy && /z = 1\.20/.test(again.cap), `Esc goes back; in again with the same settings: shown at once (${JSON.stringify(again)})`);
  // a settings change redraws the open panel
  const went = await page.evaluate(async () => {
    const el = document.getElementById('mv_laser-640'), b = document.querySelector('#scopeHost > .pnl.big');
    el.value = 0.7; el.dispatchEvent(new Event('input', { bubbles: true }));
    for (let t = 0; t < 3000; t += 5) { if (b.classList.contains('busy')) return true; await new Promise(r => setTimeout(r, 5)); }
    return false;
  });
  check(went, 'a settings change redraws the open panel');
  await reset('z');
  await reset('frame');
  await waitCap(/frame 0 of 9.*\(the focus\)$/, 'double-click on the values: back to frame 0 at the focus');
  await page.evaluate(() => scopeView.close());

  // a snap: the frame's pattern state in the info, the sample's counter stepping
  const snap = async () => {
    await page.waitForFunction(() => !liveState.pending, null, { timeout: 90000 });
    const n = await page.evaluate(() => liveState.frames);
    await page.evaluate(() => document.getElementById('lv_snap').click());
    await page.waitForFunction(k => liveState.frames > k && !liveState.pending, n, { timeout: 120000 });
    return page.evaluate(() => ({ p: liveState.pattern, info: document.getElementById('lv_info').title }));
  };
  const s1 = await snap(), s2 = await snap();
  check(s1.p && s1.p.period === 9 && s2.p && s2.p.frame === s1.p.frame + 1 && /pattern \d+\/9/.test(s2.info),
    `snaps: pattern state ${JSON.stringify(s1.p)} then frame ${s2.p && s2.p.frame} (info tooltip "${s2.info.split('\n').pop().trim()}")`);

  // LatticeSIM3D: five beams, 13 frames, the radius 0.9 -- the open panel follows the pick by itself
  await page.evaluate(() => scopeView.open('illumination'));
  await waitCap(/of 9/, 'the panel again');
  await set('mv_sim-type', '4');
  await waitCap(/^frame 0 of 13: .*\(5 beams\).*\(the focus\)$/, 'LatticeSIM3D in the open panel');
  const l = await page.evaluate(() => ({ bfp: +document.getElementById('mv_illum-bfp').value }));
  const lb = await big();
  check(l.bfp === 0.9 && /LatticeSIM3D/.test(lb.title), `LatticeSIM3D: 5 beams, 13 frames, BFP position 0.9 (${lb.title}), the panel updated without a click`);
  // the lattice's section changes with height (the centre beam's fringes swap direction every quarter axial period)
  const fp = () => page.evaluate(() => { const c = document.querySelector('#scopeHost > .pnl.big .pv[data-name=xy] canvas'), a = c.getContext('2d').getImageData(0, 0, c.width, c.height).data; let h = 0; for (let i = 0; i < a.length; i += 4) h = (h * 31 + a[i]) >>> 0; return h; });
  const f0 = await fp();
  await ctl('z', 0.66);
  await waitCap(/z = 0\.66 µm$/, 'the lattice at 0.66 um');
  const f1 = await fp();
  check(f0 !== f1, `the lattice's section at 0.66 um differs from the focus's (${f0} vs ${f1})`);
  await reset('z');
  await page.evaluate(() => { scopeView.close(); scopeView.set(false); });

  // Off: one beam, no pattern in the spec
  await set('mv_sim-type', '0');
  await page.waitForFunction(() => /one beam/.test(document.getElementById('patternReadout').textContent), null, { timeout: 90000 });
  const off = await page.evaluate(() => ({ spec: movieSpec(), rows: document.getElementById('simBeams').value.split(';').length }));
  check(/ sim-type=0( |$)/.test(off.spec) && off.rows === 1, 'Off: one beam row, sim-type=0');

  // The beams at the BFP's centre (below 0.05) with a pattern on: the pattern Off (epi); back above, the pattern again,
  // at the position set (not the type's radius)
  await set('mv_sim-type', '1');
  await set('mv_illum-bfp', '0.02');
  const centre = await page.evaluate(() => [document.getElementById('mv_sim-type').value, +document.getElementById('mv_illum-bfp').value]);
  await set('mv_illum-bfp', '0.4');
  const back = await page.evaluate(() => [document.getElementById('mv_sim-type').value, +document.getElementById('mv_illum-bfp').value]);
  check(centre[0] === '0' && centre[1] === 0.02 && back[0] === '1' && back[1] === 0.4,
    `SIM2D with the BFP position at 0.02: Pattern ${centre[0]} (Off); at 0.4: Pattern ${back[0]} again, BFP ${back[1]}`);
  await set('mv_sim-type', '0');
  // What moves the BFP position (2026-11-05): not a label's mode or dye, the Imaging mode, the specimen; the
  // Illumination's Epi / HILO / TIRF does
  await set('mv_illum-bfp', '0.4');
  const bfpNow = () => page.evaluate(() => +document.getElementById('mv_illum-bfp').value);
  await set('mv_mt-mode', '0');   // dSTORM
  const afterMode = await bfpNow();
  const mode = await page.evaluate(() => { const s = document.querySelector('select[data-pick="mode"]'), o = [...s.options].find(o => /dSTORM/.test(o.value));
    s.value = o.value; s.dispatchEvent(new Event('change', { bubbles: true })); return s.value; });
  await page.waitForTimeout(300);
  const afterPreset = await bfpNow(), shown = await page.evaluate(() => document.querySelector('select[data-pick="mode"]').value);
  await set('mv_specimen', '1');
  const afterSpecimen = await bfpNow();
  await set('mv_specimen', '0');
  await page.evaluate(() => { const s = document.querySelector('select[data-pick="illumination"]'); s.value = 'TIRF'; s.dispatchEvent(new Event('change', { bubbles: true })); });
  const afterTirf = await bfpNow();
  check(afterMode === 0.4 && afterPreset === 0.4 && shown === mode && afterSpecimen === 0.4 && afterTirf === 1.03,
    `the BFP position stays 0.4 through a mode change (${afterMode}), the Imaging mode "${mode}" (${afterPreset}; still shown: ${shown === mode}), the nanorulers (${afterSpecimen}); TIRF sets ${afterTirf}`);
});
