#!/usr/bin/env node
// SPDX-License-Identifier: BSD-3-Clause
// A recording's Save in headless Chromium (web/live/live.js openSave; docs/ground-truth.md):
//   - Save opens a box: the movie, its settings, its ground truth: for a blinking label two thresholds and no dye list,
//     for WideField labels the dye list and no thresholds;
//   - the movie alone is a .tif; with its settings and ground truth one .zip: movie.tif, settings.isc.json, README.txt,
//     gt/frames.csv, gt/localizations_min<N>.csv, gt/README.txt, gt/voxelize.html;
//   - with the cli (build/msvc/cli/Release/insiliscope_cli.exe, or ISC_CLI): insiliscope_cli --settings
//     settings.isc.json renders movie.tif's frames bit for bit and --gt-out writes gt/'s tables byte for byte;
//   - voxelize.html (served beside the viewer) turns a localization table into an ImageJ stack with its voxel size.
//   node tests/web/viewer_save.mjs [--swiftshader]   (~40 s)
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import zlib from 'node:zlib';
import { execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { runViewer } from './browser.mjs';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');

// The entries of a zip (as web/live/zip.js writes it; no Zip64 here): {name: bytes}.
function unzip(buf) {
  const v = new DataView(buf.buffer, buf.byteOffset, buf.byteLength), out = {};
  let e = buf.length - 22;
  while (e >= 0 && v.getUint32(e, true) !== 0x06054b50) e--;
  const n = v.getUint16(e + 10, true);
  for (let k = 0, i = v.getUint32(e + 16, true); k < n; k++) {
    const method = v.getUint16(i + 10, true), csize = v.getUint32(i + 20, true);
    const nl = v.getUint16(i + 28, true), xl = v.getUint16(i + 30, true), cl = v.getUint16(i + 32, true), off = v.getUint32(i + 42, true);
    const name = new TextDecoder().decode(buf.subarray(i + 46, i + 46 + nl));
    const at = off + 30 + v.getUint16(off + 26, true) + v.getUint16(off + 28, true), body = buf.subarray(at, at + csize);
    out[name] = method === 8 ? new Uint8Array(zlib.inflateRawSync(body)) : body;
    i += 46 + nl + xl + cl;
  }
  return out;
}
// A little-endian TIFF's pages: [{w, h, bits, desc, xres, data (offset)}].
function readTiff(buf) {
  const dv = new DataView(buf.buffer, buf.byteOffset, buf.byteLength), pages = [];
  let ifd = dv.getUint32(4, true);
  while (ifd) {
    const n = dv.getUint16(ifd, true), t = {};
    for (let i = 0; i < n; i++) {
      const o = ifd + 2 + 12 * i, tag = dv.getUint16(o, true), type = dv.getUint16(o + 2, true), cnt = dv.getUint32(o + 4, true);
      t[tag] = type === 3 ? dv.getUint16(o + 8, true) : type === 5 ? (() => { const a = dv.getUint32(o + 8, true); return dv.getUint32(a, true) / dv.getUint32(a + 4, true); })()
        : type === 2 ? new TextDecoder().decode(buf.subarray(dv.getUint32(o + 8, true), dv.getUint32(o + 8, true) + cnt - 1)) : dv.getUint32(o + 8, true);
    }
    pages.push({ w: t[256], h: t[257], bits: t[258], desc: t[270], xres: t[282], data: t[273] });
    ifd = dv.getUint32(ifd + 2 + 12 * n, true);
  }
  return pages;
}
const same = (a, b) => a && b && a.length === b.length && a.every((x, i) => x === b[i]);

await runViewer('viewer save', { watchdogSec: 240, viewport: { width: 1200, height: 800 } }, async ({ page, check, port }) => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'isc-save-'));
  try {
    await page.waitForFunction(() => window.liveState && typeof genWorkers !== 'undefined' && genWorkers.ok, null, { timeout: 120000 });
    // a short recording at 64 px (a fresh sample, the default)
    await page.evaluate(() => {
      const s = document.getElementById('mv_size'); s.value = 64; s.dispatchEvent(new Event('input', { bubbles: true }));
      document.getElementById('lv_rec').click();
      const n = document.getElementById('lv_recN'); n.value = 8; n.dispatchEvent(new Event('change'));
      document.getElementById('lv_recGo').click();
    });
    await page.waitForFunction(() => liveState.play.on, null, { timeout: 120000 });
    // Save: the box (it asks the recording's setup first: a blinking label or not)
    const IDS = ['svName', 'svSet', 'svGt', 'svMin1', 'svMin2', 'svDyes', 'svGo'];
    const openBox = async () => {
      await page.evaluate(() => { document.getElementById('setDlg').hidden = true; document.getElementById('lv_playSave').click(); });
      await page.waitForFunction(() => !document.getElementById('setDlg').hidden && document.getElementById('svGo'), null, { timeout: 30000 });
      return page.evaluate(ids => {
        const d = document.getElementById('setDlg'), v = id => (document.getElementById(id) ? +document.getElementById(id).value : NaN);
        return { ids: ids.filter(i => document.getElementById(i)), title: d.querySelector('h3').textContent, min: [v('svMin1'), v('svMin2')],
          btn: document.getElementById('lv_playSave').textContent };
      }, IDS);
    };
    const box = await openBox();
    check(box.ids.join() === 'svName,svSet,svGt,svMin1,svMin2,svGo' && box.title === 'Save the recording' && box.min[0] === 5 && box.min[1] === 300 && box.btn === 'Save',
      `Save opens the box; a DNA-PAINT recording: movie, settings, ground truth with thresholds ${box.min.join(', ')}, no dye list (${box.ids.join(', ')})`);
    // the same recording as if its labels were WideField: the dye list, no thresholds
    const spec0 = await page.evaluate(() => {
      const s = liveState.play.spec;
      liveState.play.spec = s.replace(/(^| )(mt|npc|lamin|ruler)-mode=\S+/g, '') + ' mt-mode=3 npc-mode=3 lamin-mode=3 ruler-mode=3';
      return s;
    });
    const boxWf = await openBox();
    await page.evaluate(s => { liveState.play.spec = s; document.getElementById('setDlg').hidden = true; }, spec0);
    check(boxWf.ids.join() === 'svName,svSet,svGt,svDyes,svGo', `a WideField recording: the dye list, no thresholds (${boxWf.ids.join(', ')})`);
    await openBox();
    // the movie alone: a .tif
    const [dlTif] = await Promise.all([page.waitForEvent('download', { timeout: 60000 }), page.evaluate(() => {
      document.getElementById('svSet').checked = false; document.getElementById('svGt').checked = false; document.getElementById('svGo').click();
    })]);
    const tif = fs.readFileSync(await dlTif.path());
    check(dlTif.suggestedFilename() === 'insiliscope_live_64x64x8.tif' && readTiff(tif).length === 8, `the movie alone: ${dlTif.suggestedFilename()}, 8 pages`);
    // with the settings and the ground truth: one .zip
    const tz = Date.now();
    await openBox();
    const [dlZip] = await Promise.all([page.waitForEvent('download', { timeout: 180000 }), page.evaluate(() => {
      for (const id of ['svSet', 'svGt']) document.getElementById(id).checked = true;
      document.getElementById('svGt').dispatchEvent(new Event('change'));
      document.getElementById('svGo').click();
    })]);
    const zipSec = (Date.now() - tz) / 1000;
    const z = unzip(fs.readFileSync(await dlZip.path())), names = Object.keys(z);
    const want = ['movie.tif', 'settings.isc.json', 'README.txt', 'gt/frames.csv', 'gt/localizations_min5.csv', 'gt/localizations_min300.csv', 'gt/README.txt', 'gt/voxelize.html'];
    check(dlZip.suggestedFilename() === 'insiliscope_live_64x64x8.zip' && want.every(n => names.includes(n)) && !names.includes('gt/dyes.csv'),
      `with settings and ground truth: ${dlZip.suggestedFilename()} (${zipSec.toFixed(1)} s): ${names.join(', ')}`);
    check(same(z['movie.tif'], tif), 'its movie.tif = the .tif saved alone');
    const frames = new TextDecoder().decode(z['gt/frames.csv']).trim().split('\n');
    check(frames.length === 9 && /^frame,t_start_s,exposure_s,stage_x_um/.test(frames[0]), `gt/frames.csv: a row per frame (${frames.length - 1})`);
    const closed = await page.evaluate(() => document.getElementById('setDlg').hidden && /saved insiliscope_live_64x64x8\.zip/.test(document.getElementById('lv_info').title));
    check(closed, 'the box closes, the info names the saved zip');

    // the cli: the same frames from settings.isc.json, the same tables
    const cli = process.env.ISC_CLI || path.join(ROOT, 'build/msvc/cli/Release/insiliscope_cli.exe');
    if (!fs.existsSync(cli)) console.log(`skip  the cli's reproduction (no ${cli})`);
    else {
      fs.writeFileSync(path.join(dir, 'settings.isc.json'), z['settings.isc.json']);
      const gtDir = path.join(dir, 'gt');
      fs.mkdirSync(gtDir);
      const tc = Date.now();
      execFileSync(cli, ['--settings', path.join(dir, 'settings.isc.json'), '--out', path.join(dir, 'm.tif'), '--gt-out', gtDir], { stdio: 'pipe' });
      const m = fs.readFileSync(path.join(dir, 'm.tif')), pa = readTiff(m), pb = readTiff(z['movie.tif']);
      const px = 64 * 64 * 2;
      const framesSame = pa.length === 8 && pb.length === 8 && pa.every((p, k) => same(m.subarray(p.data, p.data + px), z['movie.tif'].subarray(pb[k].data, pb[k].data + px)));
      check(framesSame, `insiliscope_cli --settings settings.isc.json: movie.tif's 8 frames, bit for bit (${((Date.now() - tc) / 1000).toFixed(1)} s)`);
      const tables = names.filter(n => /^gt\/.*\.(csv|txt|tif)$/.test(n));
      const diff = tables.filter(n => !same(fs.readFileSync(path.join(gtDir, n.slice(3))), z[n]));
      check(!diff.length, `insiliscope_cli --gt-out: ${tables.length} files byte for byte${diff.length ? ' (differ: ' + diff.join(', ') + ')' : ''}`);
    }

    // voxelize.html: the localization table to a stack with its voxel size
    const loc = path.join(dir, 'localizations_min5.csv');
    fs.writeFileSync(loc, z['gt/localizations_min5.csv']);
    const vp = page;   // (the viewer's checks are done)
    await vp.goto(`http://localhost:${port}/voxelize.html`);
    await vp.setInputFiles('#file', loc);
    await vp.waitForFunction(() => !document.getElementById('opts').hidden && !document.getElementById('go').disabled, null, { timeout: 20000 });
    await vp.evaluate(() => { const v = document.getElementById('vxy'); v.value = 20; v.dispatchEvent(new Event('input')); });
    const [dlVox] = await Promise.all([vp.waitForEvent('download', { timeout: 20000 }), vp.click('#go')]);
    const vox = fs.readFileSync(await dlVox.path()), vpages = readTiff(vox), d0 = vpages[0] || {};
    check(vpages.length > 0 && vpages[0].bits === 32 && Math.abs(d0.xres - 50) < 1e-6 && /\nspacing=0\.025\n/.test(d0.desc || '') && /unit=micron/.test(d0.desc || ''),
      `voxelize.html: ${dlVox.suggestedFilename()}, ${vpages.length} planes of ${d0.w}×${d0.h}, 32-bit, 50 px/µm, spacing 0.025 µm`);

  } finally {
    fs.rmSync(dir, { recursive: true, force: true });
  }
});
