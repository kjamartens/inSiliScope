#!/usr/bin/env node
// SPDX-License-Identifier: BSD-3-Clause
// Structured illumination in the viewer (spec/ALGORITHM.md "Structured illumination") in headless Chromium: a Pattern
// pick sets the BFP position to the type's radius and the polarization to S and goes into the movie spec; the readout
// and the Beams matrix come from the setup job (the type's rows); the Microscope view's illumination panel (filling the
// left view) shows the program's period and beam count, its slider steps the frame, the height slider moves the section
// seen from below, Esc and back in shows it at once, the open panel follows a settings change; a snap's info carries the
// pattern state and the live sample's counter steps per frame; LatticeSIM3D's five beams and its section's change with
// height; Off again; the BFP light maps (RingTIRF, an edit, an error, a map image, LightSheet).
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
    document.getElementById('simBeams').value.split('\n').length === 2, null, { timeout: 90000 });
  const ro = await page.evaluate(() => ({ text: document.getElementById('patternReadout').textContent, beams: document.getElementById('simBeams').value }));
  check(/9 frames|frames per cycle|per cycle/.test(ro.text) && /^0\.95 +0 /.test(ro.beams.trim()) && /\n-0\.95 +0 /.test(ro.beams),
    `the readout (${ro.text.slice(0, 80)}) and the two rows at ±0.95, a line each`);

  // The phase matrix (Expert): per frame of the cycle the opposing beam pairs' phase differences in degrees, the
  // program's from the setup; a typed entry goes into the spec; a matrix of another shape is refused; another number
  // of phases goes back to the program; the lattice's two pairs; a Pattern pick forgets it
  const phasesBox = () => page.evaluate(() => document.getElementById('simPhases').value);
  const typePhases = t => page.evaluate(x => { const el = document.getElementById('simPhases'); el.focus(); el.value = x;
    el.dispatchEvent(new Event('change', { bubbles: true })); el.blur(); }, t);
  const waitBox = (want, ms = 90000) => page.waitForFunction(w => document.getElementById('simPhases').value === w, want, { timeout: ms }).then(() => true, () => false);
  const prog2 = '  0\n240\n480\n  0\n240\n480\n  0\n240\n480';   // (aligned on the decimal point)
  const pm0 = await waitBox(prog2);
  const tier = await page.evaluate(() => document.getElementById('simPhases').closest('.opt').className);
  check(pm0 && /t-expert/.test(tier), `the phase matrix shows SIM2D's program, the pair's difference per frame, a line each (${JSON.stringify(await phasesBox())}), an Expert row (${tier})`);
  // the Zernike, beam and phase boxes: textareas the user can stretch, as the BFP maps' box
  const boxes = await page.evaluate(() => ['zernCoeffs', 'simBeams', 'simPhases', 'mv_bfp-maps'].map(id => { const el = document.getElementById(id);
    return [id, el.tagName, getComputedStyle(el).resize]; }));
  check(boxes.every(([, tag, rz]) => tag === 'TEXTAREA' && rz === 'vertical'), `resizable textareas: ${JSON.stringify(boxes)}`);
  // a Zernike edit applies when the box is left (a change), not per keystroke
  const zern = await page.evaluate(() => { const el = document.getElementById('zernCoeffs'), v = el.value.split(/\s+/); v[5] = '0.123';
    el.focus(); el.value = v.join(' '); el.dispatchEvent(new Event('input', { bubbles: true })); const typing = / zern\.5=/.test(movieSpec());
    el.dispatchEvent(new Event('change', { bubbles: true })); el.blur(); return { typing, after: (/ zern\.5=([^ ]+)/.exec(movieSpec()) || [])[1] }; });
  check(!zern.typing && zern.after === '0.123', `a Zernike edit applies on leaving the box (while typing: ${zern.typing}, after: zern.5=${zern.after})`);
  await page.evaluate(() => ISC_ZERN.clear());
  await typePhases('0\n240\n470\n0\n240\n480\n0\n240\n480');
  const pm1 = await page.waitForFunction(() => / sim-pair-phase\.2\.0=470( |$)/.test(movieSpec()), null, { timeout: 30000 }).then(() => true, () => false);
  check(pm1 && / sim-pair-phase\.8\.0=480( |$)/.test(await page.evaluate(() => movieSpec())), 'a typed entry: every entry in the spec (sim-pair-phase.2.0=470)');
  await typePhases('0 1 2; 0 1 2; 0 1 2');
  const bad = await page.evaluate(() => ({ outline: document.getElementById('simPhases').style.outline, title: document.getElementById('simPhases').title, spec: movieSpec() }));
  check(/solid/.test(bad.outline) && / sim-pair-phase\.2\.0=470( |$)/.test(bad.spec) && /9 rows/.test(bad.title),
    `a matrix of another shape is refused, the last one kept ("${bad.title}")`);
  await set('mv_sim-phases', '4');
  const pm2 = await waitBox('  0\n180\n360\n540\n  0\n180\n360\n540\n  0\n180\n360\n540');
  check(pm2 && !/sim-pair-phase\./.test(await page.evaluate(() => movieSpec())), `Phases 4: back to the program, 12 rows (${await phasesBox()})`);
  await set('mv_sim-phases', '0');
  await set('mv_sim-type', '4');
  const lat = await page.waitForFunction(() => { const v = document.getElementById('simPhases').value.split('\n'); return v.length === 13 && /^ *110\.7692 +166\.1538$/.test(v[1]); },
    null, { timeout: 90000 }).then(() => true, () => false);
  // (the columns aligned on the decimal point: each entry's dot under the one above)
  const dots = await page.evaluate(() => document.getElementById('simPhases').value.split('\n').map(l => [...l.matchAll(/\./g)].map(m => m.index)).filter(d => d.length === 2));
  check(lat && dots.length > 5 && dots.every(d => d[0] === dots[0][0] && d[1] === dots[0][1]),
    `LatticeSIM3D: 13 lines of the x and y pairs' differences, the columns aligned (${(await phasesBox()).split('\n').slice(0, 3).join(' | ')} ...)`);
  await typePhases((await phasesBox()).replace(/110\.7692( +)166\.1538/, '110.7692$1170'));
  const pm3 = await page.waitForFunction(() => / sim-pair-phase\.1\.1=170( |$)/.test(movieSpec()) && / sim-pair-phase\.1\.0=110\.7692( |$)/.test(movieSpec()), null, { timeout: 30000 }).then(() => true, () => false);
  check(pm3, 'the lattice: the y pair moved alone (sim-pair-phase.1.1=170)');
  await set('mv_sim-type', '1');
  const pm4 = (await waitBox(prog2)) && !/sim-pair-phase\./.test(await page.evaluate(() => movieSpec()));
  check(pm4 && await page.evaluate(() => ISC_PHASES.user === null), `a Pattern pick forgets the typed matrix (${await phasesBox()})`);

  // The Microscope view's illumination panel, filling the left view: the period and beams, the frame slider, the height
  const big = () => page.evaluate(() => { const b = document.querySelector('#scopeHost > .pnl.big[data-panel=illumination]');
    return b ? { busy: b.classList.contains('busy'), cap: b.querySelector('.pnlCap').textContent, title: b.querySelector('.pnlTitle').textContent } : null; });
  const waitCap = (re, why) => page.waitForFunction(r => { const b = document.querySelector('#scopeHost > .pnl.big[data-panel=illumination]');
    return b && !b.classList.contains('busy') && new RegExp(r).test(b.querySelector('.pnlCap').textContent); }, re.source, { timeout: 90000 })
    .then(() => true, async () => { check(false, why + ': ' + JSON.stringify(await big())); return false; });
  const ctl = (name, v) => page.evaluate(([n, x]) => { const s = document.querySelector(`#scopeHost > .pnl.big .pnlCtl input[data-name=${n}]`);
    s.value = x; s.dispatchEvent(new Event('input')); }, [name, v]);
  const reset = name => page.evaluate(n => document.querySelector(`#scopeHost > .pnl.big .pnlCtl input[data-name=${n}]`).closest('.pc').querySelector('.pcV')
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
  const off = await page.evaluate(() => ({ spec: movieSpec(), rows: document.getElementById('simBeams').value.split('\n').length }));
  check(/ sim-type=0( |$)/.test(off.spec) && off.rows === 1, 'Off: one beam row, sim-type=0');

  // The BFP light maps (spec/ALGORITHM.md "BFP light maps"): RingTIRF's pick sets 1.03 and leaves the polarization;
  // the box shows its equations (the setup's), the spec carries none; the readout and the BFP panel; an edit makes the
  // Pattern Maps with the text in the spec; an error shows; a map image; LightSheet forgets the text; the BFP position
  // below 0.05 leaves a map type on; Off.
  await set('mv_sim-polarization', '2');
  await set('mv_sim-type', '7');
  await page.waitForFunction(() => /time slots/.test(document.getElementById('bfpMapsMsg').textContent) &&
    /TIRF/.test(document.getElementById('bfpReadout').textContent), null, { timeout: 90000 });
  const ring = await page.evaluate(() => ({ bfp: +document.getElementById('mv_illum-bfp').value, pol: document.getElementById('mv_sim-polarization').value,
    spec: movieSpec(), box: document.getElementById('mv_bfp-maps').value, msg: document.getElementById('bfpMapsMsg').textContent,
    readout: document.getElementById('bfpReadout').textContent }));
  check(ring.bfp === 1.03 && ring.pol === '2' && / sim-type=7( |$)/.test(ring.spec) && !/bfp-maps=/.test(ring.spec) &&
    /scanRot = 360\*t/.test(ring.box) && /^64 time slots/.test(ring.msg) && /evanescent \(TIRF\)/.test(ring.readout),
    `RingTIRF: BFP position 1.03, the polarization kept, its equations in the box, none in the spec ("${ring.msg}"; "${ring.readout}")`);
  await page.evaluate(() => scopeView.set(true));
  await page.waitForFunction(() => +document.getElementById('scopeHost').dataset.panels > 0, null, { timeout: 60000 });
  await page.evaluate(() => scopeView.open('bfp'));
  const bfpOk = await page.waitForFunction(() => { const b = document.querySelector('#scopeHost > .pnl.big[data-panel=bfp]');
    return b && !b.classList.contains('busy') && /RingTIRF/.test(b.querySelector('.pnlTitle').textContent) && /64 time slots/.test(b.textContent); },
    null, { timeout: 90000 }).then(() => true, () => false);
  const ringPx = bfpOk ? await lit() : [];
  check(bfpOk && ringPx.length >= 1 && ringPx[0] > 100, `the BFP panel: RingTIRF's maps, 64 time slots (${ringPx.join(', ')} lit px)`);
  await page.evaluate(() => { scopeView.close(); scopeView.set(false); });
  const edit = async text => { await page.evaluate(t => { const el = document.getElementById('mv_bfp-maps'); el.value = t; el.dispatchEvent(new Event('change', { bubbles: true })); }, text); };
  await edit('I = r < umax*0.9\ncoherent = 0\nscanX = 0.2*sin(2*pi*t); scanY = 0.2*cos(2*pi*t)');
  await page.waitForFunction(() => /propagating/.test(document.getElementById('bfpReadout').textContent), null, { timeout: 90000 });
  const clsm = await page.evaluate(() => ({ sim: document.getElementById('mv_sim-type').value, bfp: +document.getElementById('mv_illum-bfp').value, spec: movieSpec() }));
  check(clsm.sim === '8' && clsm.bfp === 1.03 && / bfp-maps="I = r < umax\*0\.9\\ncoherent = 0\\nscanX/.test(clsm.spec),
    `an edit (CLSM-like): the Pattern Maps, the BFP position kept, the text quoted in the spec`);
  await edit('I = foo(ux)');
  await page.waitForFunction(() => /^Not made: .*foo/.test(document.getElementById('bfpMapsMsg').textContent), null, { timeout: 90000 })
    .then(() => check(true, 'an unknown function: "Not made", the box marked'), () => check(false, 'an unknown function: no error shown'));
  // a map image: a 32 x 32 16-bit TIFF of a ring, then I = img("ring.tif")
  await page.evaluate(() => {
    const n = 32, px = new Uint16Array(n * n);
    for (let y = 0; y < n; y++) for (let x = 0; x < n; x++) px[y * n + x] = Math.abs(Math.hypot(x - 15.5, y - 15.5) / 16 - 0.6) < 0.08 ? 60000 : 0;
    const off = 8 + px.byteLength, tags = [[256, 3, n], [257, 3, n], [258, 3, 16], [259, 3, 1], [262, 3, 1], [273, 4, 8], [277, 3, 1], [278, 3, n], [279, 4, px.byteLength]];
    const buf = new ArrayBuffer(off + 2 + tags.length * 12 + 4), dv = new DataView(buf);
    dv.setUint16(0, 0x4949); dv.setUint16(2, 42, true); dv.setUint32(4, off, true);
    new Uint8Array(buf, 8, px.byteLength).set(new Uint8Array(px.buffer));
    dv.setUint16(off, tags.length, true);
    tags.forEach(([t, ty, v], i) => { const o = off + 2 + i * 12; dv.setUint16(o, t, true); dv.setUint16(o + 2, ty, true); dv.setUint32(o + 4, 1, true);
      if (ty === 3) dv.setUint16(o + 8, v, true); else dv.setUint32(o + 8, v, true); });
    iscSendBfpFile('ring.tif', new Uint8Array(buf));
  });
  await edit('I = img("ring.tif")\ncoherent = 0');
  await page.waitForFunction(() => /s to make/.test(document.getElementById('bfpMapsMsg').textContent), null, { timeout: 90000 })
    .then(() => check(true, 'a map image (img("ring.tif")): made'), async () => check(false, 'a map image: ' + await page.evaluate(() => document.getElementById('bfpMapsMsg').textContent)));
  await set('mv_sim-type', '6');
  await page.waitForFunction(() => /Inclined light sheet/.test(document.getElementById('mv_bfp-maps').value), null, { timeout: 90000 })
    .then(() => true, () => false);
  const sheet = await page.evaluate(() => ({ bfp: +document.getElementById('mv_illum-bfp').value, spec: movieSpec(), user: ISC_MAPS.user }));
  check(sheet.bfp === 0.9 && sheet.user === null && !/bfp-maps=/.test(sheet.spec) && / sim-type=6( |$)/.test(sheet.spec),
    'LightSheet: BFP position 0.9, the typed text forgotten, its own equations shown');
  await set('mv_illum-bfp', '0.02');
  const parked = await page.evaluate(() => document.getElementById('mv_sim-type').value);
  check(parked === '6', `the BFP position at 0.02 leaves LightSheet on (${parked})`);
  await set('mv_sim-type', '0');

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
