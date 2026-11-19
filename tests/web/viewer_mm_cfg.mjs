#!/usr/bin/env node
// SPDX-License-Identifier: BSD-3-Clause
// The ".cfg from the viewer" export (web/live/mm_cfg.js, ☰ › Settings file › Micro-Manager .cfg…):
//  - every option the viewer can send (each settings control, each world parameter, each key of its spec in the
//    scenarios below) has a Micro-Manager property, a writer of its own or a line in tools/mm_cfg_map.py VIEWER_ONLY --
//    a new viewer setting without a property fails here;
//  - the menu's dialog saves a .cfg;
//  - with --out <dir>: each scenario's spec (movieSpec()) and .cfg, for tools/test_viewer_cfg.py, which loads the .cfg in
//    Micro-Manager and compares Renderer.WriteScopeSpecTo with the spec.
//   node tests/web/viewer_mm_cfg.mjs [--out dir]   (~26 s)
import { runViewer } from './browser.mjs';
import fs from 'node:fs';
import path from 'node:path';

const outArg = process.argv.indexOf('--out'), OUT = outArg > 0 ? process.argv[outArg + 1] : null;
// Scenarios: [id, value] in order (a control's id; 'click:<id>' clicks; 'zern' / 'maps' / 'beams' type the matrices).
const SCENARIOS = {
  defaults: [],
  storm_ixon: [['mv_mt-mode', '0'], ['mv_mt-dye', '0'], ['mv_mt-dye.qy', '0.31'], ['mv_mt-dye.on-sec', '0.02'],
    ['mv_camera-preset', '1'], ['mv_drift-xy-speed-nm-per-sec', '3'], ['mv_drift-z-direction', '-1'], ['cellBlob', '2.2'],
    ['nucLongMin', '7'], ['allowPackRotation', false], ['mv_fid-density', '0.05'], ['mv_fid-type', '1'],
    ['mv_laser-405', '0.02'], ['mv_exposure-ms', '30'], ['mv_size', '96'], ['mv_bf-n-cytoplasm', '1.36'],
    ['mv_mean-field-slab-nm', '400'], ['seed', '7']],
  rulers_sim: [['mv_specimen', '1'], ['mv_ruler-mode', '3'], ['mv_sim-type', '1'], ['mv_illum-bfp', '0.9'],
    ['mv_psf-zernike-preset', '2'], ['zern', '0 0 0 0 0.02 0.1 0 0.03 0 0 0 0 0.05 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0'],
    ['origamiSpacingNm', '40'], ['mv_na', '1.45'], ['mv_qe', '0.8'], ['qeFlat', true]],
  slide_maps: [['mv_specimen', '3'], ['mv_slide-dye', '4'], ['mv_slide-conc-micromolar', '3'], ['click:addUserDye'],
    ['mv_dye1.qy', '0.5'], ['mv_sim-type', '6'], ['mv_sheet-thickness-um', '2'],
    ['maps', 'I = spot(ux - 0.9, uy, 0.15)\n# a comment, with a comma\ncoherent = 0\npol = unpolarized'],
    ['mv_dichroic', '5'], ['mv_em-filter', '3']],
};

await runViewer('viewer mm cfg', { watchdogSec: 180, viewport: { width: 1300, height: 800 } }, async ({ page, check }) => {
  await page.waitForFunction(() => self.iscMmCfg && self.ISC_MM_CFG && self.ISC_ENGINE_OPTIONS && self.ISC_ENGINE_OPTIONS.size, null, { timeout: 60000 });
  const keysSeen = new Set();
  // the options the viewer can send: every control and world parameter, whether at its default or not
  const base = await page.evaluate(() => [...MOVIE_KEYS, ...Object.keys(params()).map(k => 'p.' + k)]);
  base.forEach(k => keysSeen.add(k));
  if (OUT) fs.mkdirSync(OUT, { recursive: true });
  for (const [name, steps] of Object.entries(SCENARIOS)) {
    // a fresh page at the defaults (the viewer keeps its settings in localStorage)
    await page.evaluate(() => { try { localStorage.clear(); } catch (e) { /* private window */ } });
    await page.reload();
    await page.waitForFunction(() => self.iscMmCfg && self.ISC_ENGINE_OPTIONS && self.ISC_ENGINE_OPTIONS.size && window.iscScene && iscScene.packingIdle(), null, { timeout: 60000 });
    for (const [id, v] of steps) {
      await page.evaluate(([id, v]) => {
        const fire = el => { el.dispatchEvent(new Event('input', { bubbles: true })); el.dispatchEvent(new Event('change', { bubbles: true })); };
        if (id.startsWith('click:')) { document.getElementById(id.slice(6)).click(); return; }
        if (id === 'zern') { const el = document.getElementById('zernCoeffs'); el.value = v; fire(el); return; }
        if (id === 'maps') { const el = document.getElementById('mv_bfp-maps'); el.value = v; fire(el); return; }
        const el = document.getElementById(id);
        if (!el) throw new Error('no control ' + id);
        if (el.type === 'checkbox') el.checked = v; else el.value = v;
        fire(el);
      }, [id, v]);
      await page.waitForTimeout(150);
    }
    await page.waitForTimeout(400);
    const r = await page.evaluate(n => { const out = iscSettings.mmCfg(n); return { spec: movieSpec(), text: out.text, settings: out.settings.length, warnings: out.warnings, shutter: out.shutter }; }, name);
    for (const k of (await page.evaluate(s => [...iscSettingsCore.parseSpec(s).keys()], r.spec))) keysSeen.add(k);
    check(r.settings > 150 && /ConfigGroup,System,Startup,Core,Shutter,/.test(r.text),
      `${name}: a .cfg of ${r.settings} settings, light ${r.shutter}${r.warnings.length ? ', notes: ' + r.warnings.join(' | ') : ''}`);
    if (OUT) {
      fs.writeFileSync(path.join(OUT, name + '.spec.txt'), r.spec);
      fs.writeFileSync(path.join(OUT, name + '.cfg'), r.text);
    }
  }
  const missing = await page.evaluate(keys => iscMmCfg.uncovered(keys, ISC_MM_CFG), [...keysSeen]);
  check(!missing.length, missing.length
    ? `viewer options with no Micro-Manager property (add a row, spec/MM_DEVICES.md, or a VIEWER_ONLY line in tools/mm_cfg_map.py): ${missing.join(', ')}`
    : `every one of the viewer's ${keysSeen.size} options has a Micro-Manager property, a writer of its own or is viewer-only`);
  // the menu: ☰ › Micro-Manager .cfg… › Save
  await page.click('#menuBtn');
  await page.click('#lv_setCfg');
  const shown = await page.evaluate(() => !document.getElementById('setDlg').hidden && /Micro-Manager configuration/.test(document.querySelector('#setDlg h3').textContent));
  const [dl] = await Promise.all([page.waitForEvent('download', { timeout: 10000 }), page.click('#setGo')]);
  const file = await dl.path(), text = fs.readFileSync(file, 'utf8');
  check(shown && /\.cfg$/.test(dl.suggestedFilename()) && /\r\nConfigGroup,System,Startup,Camera,Exposure,/.test(text) && /^# inSiliScope: /.test(text),
    `the menu's dialog saves ${dl.suggestedFilename()} (${text.split('\n').length} lines, CRLF)`);
});
