#!/usr/bin/env node
// SPDX-License-Identifier: BSD-3-Clause
// A multi-D Record in headless Chromium (2026-11-23, web/live/mda.js, mda_ui.js, live.js):
//   - the Record box's "multi-D plan" row is hidden at Basic, shown at Advanced; a refused plan (a for block over the
//     image size) says why in the box;
//   - the editor opens with the plan's rows and keeps it on Done;
//   - a plan of 2 positions x 2 relative z x (A: 3 frames at 10 ms; B: 2 frames at 20 ms with a laser's power) runs as
//     20 frames, each chunk's spec with its block's x, y, z, exposure and setting; the stage and focus stay where
//     they were;
//   - Save: the .zip of each format (OME-TIFF, ImageJ per position, flat) with index.csv, read by Python (zipfile,
//     tifffile): the OME series (C, Z, T, Y, X) = (2, 2, 3, 64, 64) per position, the ImageJ hyperstacks, the flat
//     stack, a row per page, the blank planes marked; the settings file carries the plan;
//   - settings by name: typing part of one lists it with its label and its own help (the settings' tooltip), a pick
//     fills in its value now (the phase matrix under SIM2D), hovering shows the help; a loop over two phase matrices
//     runs with each block's entries in its spec; a matrix of the wrong shape is refused before Start.
//   node tests/web/viewer_mda.mjs [--channel=chrome] [--swiftshader]
import http from 'node:http';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { execFileSync } from 'node:child_process';
import { createRequire } from 'node:module';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const require = createRequire(path.join(ROOT, 'web/tools/package.json'));
let chromium;
try { ({ chromium } = require('playwright')); }
catch { ({ chromium } = await import('/opt/node22/lib/node_modules/playwright/index.mjs')); }
const arg = k => { const a = process.argv.find(s => s.startsWith('--' + k)); return a ? (a.split('=')[1] || true) : null; };
const server = http.createServer((req, res) => {
  const f = path.join(ROOT, 'web', decodeURIComponent(req.url.split('?')[0]).replace(/^\/+/, '') || 'index.html');
  if (!f.startsWith(path.join(ROOT, 'web')) || !fs.existsSync(f) || fs.statSync(f).isDirectory()) { res.statusCode = 404; res.end(); return; }
  res.setHeader('content-type', f.endsWith('.js') ? 'text/javascript' : f.endsWith('.css') ? 'text/css' : 'text/html');
  res.end(fs.readFileSync(f));
}).listen(0);
let py = null;
for (const exe of ['python3', 'python']) {
  try { execFileSync(exe, ['-c', 'import tifffile'], { stdio: 'pipe' }); py = exe; break; } catch (e) { /* next */ }
}

let fail = 0;
const t0 = Date.now(), check = (ok, what) => { if (!ok) fail++; console.log(`${ok ? 'ok  ' : 'FAIL'}  ${what}  [${((Date.now() - t0) / 1000).toFixed(0)} s]`); };
const browser = await chromium.launch({ channel: arg('channel') || undefined, args: arg('swiftshader') ? ['--use-angle=swiftshader', '--enable-unsafe-swiftshader'] : ['--enable-gpu', '--ignore-gpu-blocklist'] });
const ctx = await browser.newContext({ viewport: { width: 1200, height: 800 }, acceptDownloads: true });
const page = await ctx.newPage();
const errors = [];
page.on('pageerror', e => errors.push(e.message));
const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'isc-mda-'));
try {
  await page.goto(`http://localhost:${server.address().port}/index.html?live=0`);
  await page.waitForFunction(() => window.liveState && liveState.recPlan && typeof genWorkers !== 'undefined' && genWorkers.ok && self.ISC_ENGINE_OPTIONS, null, { timeout: 120000 });
  const shown = () => page.evaluate(() => { const r = document.getElementById('lv_recPlanRow'); return !!(r.offsetParent || r.getClientRects().length); });
  await page.evaluate(() => {
    document.querySelector('#levelSeg button[data-level=basic]').click();
    document.getElementById('mv_size').value = 64; document.getElementById('mv_size').dispatchEvent(new Event('input', { bubbles: true }));
    document.getElementById('mv_size').dispatchEvent(new Event('change', { bubbles: true }));
    document.getElementById('lv_rec').click();
  });
  const basic = await shown();
  await page.evaluate(() => document.querySelector('#levelSeg button[data-level=advanced]').click());
  const adv = await shown();
  check(!basic && adv, `the multi-D plan row: hidden at Basic, shown at Advanced (${basic} / ${adv})`);
  const laser = await page.evaluate(() => [...self.ISC_ENGINE_OPTIONS.keys()].find(k => /^laser-\d+$/.test(k)));
  // a refused plan says why in the box
  const bad = await page.evaluate(() => {
    const L = liveState.recPlan;
    L.set({ positions: [], root: [{ kind: 'loop', axis: 'setting', key: 'size', values: '64, 128', children: [{ kind: 'record', name: 'A', frames: 1, exposureMs: 10, settings: {} }] }] });
    const box = document.getElementById('lv_recPlan');
    if (!box.checked) box.click();
    const n = document.getElementById('lv_recPlanNote');
    return { bad: n.classList.contains('bad'), text: n.textContent, nHidden: getComputedStyle(document.getElementById('lv_recN')).display === 'none' };
  });
  check(bad.bad && /size cannot be set here/.test(bad.text) && bad.nHidden, `a plan over the image size refused in the box: "${bad.text}"; the frames field gives way to the plan`);
  // the plan
  const before = await page.evaluate(() => ({ x: view.cx, y: view.cy, z: +document.getElementById('mv_z').value }));
  await page.evaluate(({ b, laser }) => liveState.recPlan.set({
    positions: [{ name: 'P1', x: b.x, y: b.y, z: b.z }, { name: 'P2', x: b.x + 3, y: b.y, z: b.z }],
    root: [{ kind: 'loop', axis: 'position', values: 'P1, P2', children: [
      { kind: 'loop', axis: 'z', values: '0, 0.5', relative: true, children: [
        { kind: 'record', name: 'A', frames: 3, exposureMs: 10, settings: {} },
        { kind: 'record', name: 'B', frames: 2, exposureMs: 20, settings: { [laser]: 7 } }] }] }] }), { b: before, laser });
  const note = await page.evaluate(() => document.getElementById('lv_recPlanNote').textContent);
  check(/^8 blocks · 20 frames of 64 × 64 px/.test(note), `the box: "${note}"`);
  // the editor: its rows, Done keeps the plan
  const ed = await page.evaluate(() => {
    document.getElementById('lv_recPlanEdit').click();
    const box = document.querySelector('#setDlg .box');
    const r = { pos: box.querySelectorAll('.mdaPos').length, nodes: box.querySelectorAll('.mdaNode').length, recs: box.querySelectorAll('.mdaNode.rec').length,
      check: box.querySelector('#mdaCheck').textContent, open: !document.getElementById('setDlg').hidden };
    box.querySelector('#mdaDone').click();
    r.closed = document.getElementById('setDlg').hidden;
    r.same = liveState.recPlan.check().frames;
    return r;
  });
  check(ed.open && ed.pos === 2 && ed.nodes === 4 && ed.recs === 2 && /^8 blocks · 20 frames/.test(ed.check) && ed.closed && ed.same === 20,
    `the editor: 2 positions, 4 block rows (2 recordings), its check "${ed.check.slice(0, 60)}…", Done keeps the plan`);
  // run it
  await page.evaluate(() => document.getElementById('lv_recGo').click());
  await page.waitForFunction(() => liveState.play.on, null, { timeout: 240000 });
  const run = await page.evaluate(() => ({ n: liveState.play.n, chunks: liveState.rec.chunks.map(c => ({ spec: c.spec, n: c.n })),
    x: view.cx, y: view.cy, z: +document.getElementById('mv_z').value, place: liveState.play.plan.place.length }));
  const opt = (s, k) => { const m = new RegExp('(?:^| )' + k.replace(/\./g, '\\.') + '=(\\S+)').exec(s); return m ? +m[1] : NaN; };
  const per = run.chunks.map(c => `${c.n}:${opt(c.spec, 'x') - before.x}:${+(opt(c.spec, 'z') - before.z).toFixed(3)}:${opt(c.spec, 'exposure-ms')}:${opt(c.spec, laser) === 7 ? 'L' : '-'}`);
  check(run.n === 20 && run.place === 20 && per.join(' ') === '3:0:0:10:- 2:0:0:20:L 3:0:0.5:10:- 2:0:0.5:20:L 3:3:0:10:- 2:3:0:20:L 3:3:0.5:10:- 2:3:0.5:20:L',
    `20 frames, a chunk per block with its x, z, exposure, setting: ${per.join(' ')}`);
  check(run.x === before.x && run.y === before.y && run.z === before.z, 'the stage and focus where they were');
  // Save each format
  const save = async (format, settings) => {
    await page.evaluate(({ format, settings }) => {
      self.iscLastSave = null;
      document.getElementById('lv_playSave').click();
      const box = document.querySelector('#setDlg .box');
      box.querySelector('#svFmt').value = format;
      box.querySelector('#svSet').checked = settings;
      box.querySelector('#svGo').click();
    }, { format, settings });
    await page.waitForFunction(() => self.iscLastSave && self.iscLastSave.blob, null, { timeout: 60000 });
    const b64 = await page.evaluate(async () => {
      const b = new Uint8Array(await self.iscLastSave.blob.arrayBuffer());
      let s = ''; for (let i = 0; i < b.length; i += 32768) s += String.fromCharCode(...b.subarray(i, i + 32768));
      return btoa(s);
    });
    const f = path.join(dir, format + '.zip');
    fs.writeFileSync(f, Buffer.from(b64, 'base64'));
    return f;
  };
  const zips = { ome: await save('ome', true), imagej: await save('imagej', false), flat: await save('flat', false) };
  if (!py) console.log('skip  the saved files (no Python with tifffile)');
  else {
    const script = [
      'import sys, json, zipfile, io, csv, tifffile',
      'out = {}',
      'for f in sys.argv[1:]:',
      '    z = zipfile.ZipFile(f); names = sorted(z.namelist()); r = {"names": names}',
      '    rows = list(csv.DictReader(io.StringIO(z.read("index.csv").decode())))',
      '    r["rows"] = len(rows); r["pad"] = sum(1 for x in rows if x["padding"] == "1"); r["chan"] = sorted({x["channel_name"] for x in rows})',
      '    r["tifs"] = []',
      '    for n in names:',
      '        if n.endswith(".tif"):',
      '            t = tifffile.TiffFile(io.BytesIO(z.read(n)))',
      '            r["tifs"].append([n, [[s.axes, list(s.shape)] for s in t.series], (t.imagej_metadata or {}).get("channels")])',
      '    if "settings.isc.json" in names:',
      '        s = json.loads(z.read("settings.isc.json")); r["plan"] = len(s["parts"]["measurement"]["viewer"]["recordPlan"]["root"])',
      '    out[f.split("/")[-1].split("\\\\")[-1]] = r',
      'print(json.dumps(out))'].join('\n');
    let o = null, txt = '';
    try { txt = execFileSync(py, ['-I', '-c', script, zips.ome, zips.imagej, zips.flat], { encoding: 'utf8' }); o = JSON.parse(txt); } catch (e) { txt = String(e.stderr || e); }
    const O = o && o['ome.zip'], I = o && o['imagej.zip'], F = o && o['flat.zip'];
    check(O && O.names.join() === 'README.txt,index.csv,movie.ome.tif,settings.isc.json' && O.tifs[0][1].length === 2 &&
      O.tifs[0][1].every(s => s[0] === 'CZTYX' && s[1].join() === '2,2,3,64,64') && O.rows === 24 && O.pad === 4 && O.plan === 1,
      `OME: ${JSON.stringify(O)}`);
    check(O && O.chan.join('|') === 'A|B', `channels ${O && O.chan.join(' / ')}`);
    check(I && I.names.join() === 'P1.tif,P2.tif,README.txt,index.csv' && I.tifs.every(t => t[2] === 2 && t[1][0][1].join() === '3,2,2,64,64') && I.rows === 24 && I.pad === 4,
      `ImageJ: ${JSON.stringify(I)}`);
    check(F && F.names.join() === 'README.txt,index.csv,movie.tif' && F.tifs[0][1][0][1].join() === '20,64,64' && F.rows === 20 && F.pad === 0, `flat: ${JSON.stringify(F)}`);
    if (!o) console.log(txt);
  }
  // ---- settings by the names their help shows, matrices as one value (2026-11-23): the phase matrix under SIM2D ----
  await page.evaluate(() => {
    const s = document.getElementById('mv_sim-type');
    s.value = [...s.options].find(o => o.textContent.trim() === 'SIM2D').value;
    s.dispatchEvent(new Event('input', { bubbles: true })); s.dispatchEvent(new Event('change', { bubbles: true }));
  });
  await page.waitForFunction(() => self.ISC_PHASES && ISC_PHASES.shape && ISC_PHASES.shape[0] === 9 && document.getElementById('simPhases').value.trim(), null, { timeout: 60000 });
  await page.evaluate(() => liveState.recPlan.set({ positions: [], root: [{ kind: 'record', name: 'A', frames: 1, exposureMs: 10, settings: {} }] }));
  await page.evaluate(() => document.getElementById('lv_recPlanEdit').click());
  const field = page.locator('#setDlg .mdaNode.rec [data-f=settings]');
  await field.click();
  await field.pressSequentially('PatternPhaseM');
  await page.waitForFunction(() => { const p = document.querySelector('.mdaPick'); return p && !p.hidden && p.querySelector('.it'); }, null, { timeout: 5000 });
  const sug = await page.evaluate(() => ({ first: document.querySelector('.mdaPick .it').textContent, tip: document.getElementById('tip').style.display === 'block' ? document.getElementById('tip').textContent : '' }));
  check(/^Lasers\.PatternPhaseMatrix/.test(sug.first) && /Phase matrix/.test(sug.first) && /phase difference between\s+opposing beams/.test(sug.tip) && /Lasers\.PatternPhaseMatrix/.test(sug.tip),
    `typing a name lists the setting with its label, and shows its own help ("${sug.first}"; help "${sug.tip.slice(0, 60)}…")`);
  await page.keyboard.press('Enter');
  const filled = await field.inputValue();
  check(filled === 'Lasers.PatternPhaseMatrix=0\n  240\n  480\n  0\n  240\n  480\n  0\n  240\n  480', `picking it fills in its value now, a row per line: ${JSON.stringify(filled)}`);
  const hgt = await field.evaluate(el => ({ tag: el.tagName, h: el.offsetHeight, s: el.scrollHeight, c: el.clientHeight }));
  check(hgt.tag === 'TEXTAREA' && hgt.s <= hgt.c + 1 && hgt.h > 100, `the settings box grows with its text (${JSON.stringify(hgt)})`);
  await page.mouse.move(0, 0);
  await field.hover();
  const hov = await page.evaluate(() => document.getElementById('tip').style.display === 'block' ? document.getElementById('tip').textContent : '');
  check(/phase difference between\s+opposing beams/.test(hov), 'hovering a filled settings field shows its settings\' help');
  // copy below: the same recording under the next name, its settings to tweak; Done keeps both
  await page.evaluate(() => document.querySelector('#setDlg .mdaNode.rec button[data-a=dup]').click());
  const dup = await page.evaluate(() => [...document.querySelectorAll('#setDlg .mdaNode.rec')].map(r => [r.querySelector('[data-f=name]').value, r.querySelector('[data-f=settings]').value]));
  check(dup.length === 2 && dup[1][0] === 'A2' && dup[1][1] === dup[0][1], `copy below: ${JSON.stringify(dup.map(d => d[0]))}, the same settings`);
  await page.evaluate(() => document.querySelector('#setDlg #mdaDone').click());
  const kept = await page.evaluate(() => { const c = liveState.recPlan.check(); return { n: c.blocks ? c.blocks.length : 0, errs: c.errs, r: liveState.recPlan.get().root.map(x => x.name) }; });
  check(kept.n === 2 && !kept.errs.length && kept.r.join() === 'A,A2', `the plan keeps both, ready to run (${JSON.stringify(kept)})`);
  // a loop over two matrices: each block's chunk carries its entries; a matrix of the wrong shape is refused before Start
  await page.evaluate(() => liveState.recPlan.set({ positions: [], root: [{ kind: 'loop', axis: 'setting', key: 'Lasers.PatternPhaseMatrix',
    values: '"0; 240; 480; 0; 240; 480; 0; 240; 480", "0; 240; 470; 0; 240; 480; 0; 240; 480"', children: [{ kind: 'record', name: 'A', frames: 1, exposureMs: 10, settings: {} }] }] }));
  const shapeOk = await page.evaluate(() => liveState.recPlan.check().errs);
  check(!shapeOk.length, `a plan over two phase matrices by name is ready (${shapeOk.join('; ')})`);
  await page.evaluate(() => document.getElementById('lv_recGo').click());
  await page.waitForFunction(() => liveState.play.on && liveState.rec.chunks.length === 2, null, { timeout: 120000 });
  const pc = await page.evaluate(() => liveState.rec.chunks.map(c => c.spec));
  check(opt(pc[0], 'sim-pair-phase.2.0') === 480 && opt(pc[1], 'sim-pair-phase.2.0') === 470 && opt(pc[1], 'sim-pair-phase.8.0') === 480 && opt(pc[0], 'sim-type') === 1,
    `the blocks' specs carry their matrices (row 2: ${opt(pc[0], 'sim-pair-phase.2.0')}, ${opt(pc[1], 'sim-pair-phase.2.0')})`);
  const wrong = await page.evaluate(() => {
    liveState.recPlan.set({ positions: [], root: [{ kind: 'record', name: 'A', frames: 1, exposureMs: 10, settings: { 'Lasers.PatternPhaseMatrix': '0; 240' } }] });
    return liveState.recPlan.check().errs.join(' | ');
  });
  check(/needs 9 rows/.test(wrong), `a matrix of the wrong shape: "${wrong}"`);
  check(!errors.length, `no page errors ${errors.slice(0, 3).join(' | ')}`);
} catch (e) {
  fail++;
  console.log('FAIL  ' + (e.stack || e));
} finally {
  await browser.close();
  server.close();
  fs.rmSync(dir, { recursive: true, force: true });
}
console.log(`viewer mda: ${fail ? fail + ' FAILED' : 'all ok'}, ${((Date.now() - t0) / 1000).toFixed(1)} s`);
process.exit(fail ? 1 : 0);
