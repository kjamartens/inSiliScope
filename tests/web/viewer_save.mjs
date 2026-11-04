#!/usr/bin/env node
// SPDX-License-Identifier: BSD-3-Clause
// A recording's Save in headless Chromium (web/live/live.js openSave; docs/ground-truth.md):
//   - Save opens a box: the movie, its settings, its ground truth: for a blinking label two thresholds and no dye list,
//     for WideField labels the dye list and no thresholds;
//   - the movie alone is a .tif; with its settings and ground truth one .zip: movie.tif, settings.isc.json, README.txt,
//     gt/frames.csv, gt/localizations_min<N>.csv, gt/README.txt (voxelize.html only beside a dye list), __main__.py
//     (web/open_in_napari.py: python <zip> opens it in napari; tests/web/napari_open_check.py runs it); ☰ › Tools
//     links voxelize.html;
//   - with the cli (build/msvc/cli/Release/insiliscope_cli.exe, or ISC_CLI): insiliscope_cli --settings
//     settings.isc.json renders movie.tif's frames bit for bit and --gt-out writes gt/'s tables byte for byte;
//   - voxelize.html (served beside the viewer) reads the saved zip itself (its tables to pick from) and turns a
//     localization table into an ImageJ stack: 8-bit by default; a compressed .tif with its voxel size, its world
//     origin (Fiji's status bar), the photons per step as its calibration; with "a channel each" a composite stack of
//     the two targets; 32-bit uncompressed, the voxels sum to the table's photons (the stack is made plane by plane);
//     the compressed .tif is the default (also for a zip dropped in); the .zip holds that uncompressed TIFF with its
//     slice labels, the same voxels.
//   node tests/web/viewer_save.mjs [--swiftshader]   (~35 s)
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
    pages.push({ w: t[256], h: t[257], bits: t[258], comp: t[259], desc: t[270], xres: t[282], data: t[273], labels: t[50839] !== undefined });
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
    const want = ['movie.tif', 'settings.isc.json', 'README.txt', 'gt/frames.csv', 'gt/localizations_min5.csv', 'gt/localizations_min300.csv', 'gt/README.txt', '__main__.py'];
    check(dlZip.suggestedFilename() === 'insiliscope_live_64x64x8.zip' && want.every(n => names.includes(n)) && !names.includes('gt/dyes.csv') &&
      !names.includes('gt/voxelize.html'),
      `with settings and ground truth: ${dlZip.suggestedFilename()} (${zipSec.toFixed(1)} s): ${names.join(', ')}`);
    check(same(z['movie.tif'], tif), 'its movie.tif = the .tif saved alone');
    const frames = new TextDecoder().decode(z['gt/frames.csv']).trim().split('\n');
    check(frames.length === 9 && /^frame,t_start_s,exposure_s,stage_x_um/.test(frames[0]), `gt/frames.csv: a row per frame (${frames.length - 1})`);
    const closed = await page.evaluate(() => document.getElementById('setDlg').hidden && /saved insiliscope_live_64x64x8\.zip/.test(document.getElementById('lv_info').title));
    check(closed, 'the box closes, the info names the saved zip');
    // python <zip>: its __main__.py opens it in napari (stood in for: tests/web/napari_open_check.py)
    {
      const zp = path.join(dir, 'recording_for_napari.zip');
      fs.writeFileSync(zp, fs.readFileSync(await dlZip.path()));
      let outN = '', okN = false;
      try { outN = execFileSync('python', [path.join(ROOT, 'tests/web/napari_open_check.py'), zp], { encoding: 'utf8' }); okN = true; }
      catch (e) { outN = String(e.stdout || '') + String(e.stderr || e.message); }
      if (/No module named '(numpy|tifffile)'|ENOENT/.test(outN)) console.log('skip  the napari opener (no python with numpy and tifffile)');
      else check(okN && /napari opener: PASS/.test(outN), `python <zip> opens it in napari (stood in for): ${outN.trim().split('\n').filter(l => /^(ok|FAIL)/.test(l)).length} checks` +
        (okN ? '' : '\n' + outN));
    }

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

    const tool = await page.evaluate(() => { const a = document.getElementById('voxelizeLink'); return a && a.closest('#menu') ? a.getAttribute('href') : null; });
    check(tool === 'voxelize.html', `☰ › Tools links the converter (${tool})`);
    // voxelize.html: the saved zip itself; its tables to pick from, the lowest threshold first
    const recZip = path.join(dir, 'recording.zip');
    fs.writeFileSync(recZip, fs.readFileSync(await dlZip.path()));
    const rows = n => new TextDecoder().decode(z[`gt/localizations_min${n}.csv`]).trim().split('\n').length - 1;
    const vp = page;   // (the viewer's checks are done)
    await vp.goto(`http://localhost:${port}/voxelize.html`);
    await vp.setInputFiles('#file', recZip);
    await vp.waitForFunction(() => !document.getElementById('opts').hidden && !document.getElementById('go').disabled, null, { timeout: 20000 });
    const picked = await vp.evaluate(() => ({ shown: !document.getElementById('pick').hidden, fmt: document.getElementById('fmt').value,
      options: [...document.getElementById('entry').options].map(o => o.textContent.split(' ')[0]), load: document.getElementById('load').textContent }));
    const n5 = rows(5), n300 = rows(300), countOf = t => +((/: ([\d.,\s ]+) rows/.exec(t) || [])[1] || '').replace(/\D/g, '');
    check(picked.shown && picked.options.join() === 'gt/localizations_min5.csv,gt/localizations_min300.csv' &&
      picked.load.startsWith('localizations_min5.csv (from recording.zip): ') && countOf(picked.load) === n5,
      `voxelize.html reads the saved zip: ${picked.options.join(', ')}; ${picked.load}`);
    check(picked.fmt === 'tif', `a zip in, a compressed .tif out by default (${picked.fmt})`);
    await vp.evaluate(() => { const s = document.getElementById('entry'); s.value = '1'; s.dispatchEvent(new Event('change')); });
    await vp.waitForFunction(() => /^localizations_min300\.csv/.test(document.getElementById('load').textContent) && !document.getElementById('opts').hidden, null, { timeout: 20000 });
    const l300 = await vp.evaluate(() => document.getElementById('load').textContent);
    check(+l300.replace(/[^\d]/g, '').slice(-String(n300).length) === n300, `the other table picked: ${l300} (${n300} rows in the zip)`);
    await vp.evaluate(() => { const s = document.getElementById('entry'); s.value = '0'; s.dispatchEvent(new Event('change')); });
    await vp.waitForFunction(() => /^localizations_min5\.csv/.test(document.getElementById('load').textContent) && !document.getElementById('opts').hidden, null, { timeout: 20000 });
    await vp.evaluate(() => {
      const v = document.getElementById('vxy'); v.value = 20;
      document.getElementById('fmt').value = 'tif'; v.dispatchEvent(new Event('input'));
    });
    const [dlVox] = await Promise.all([vp.waitForEvent('download', { timeout: 20000 }), vp.click('#go')]);
    const vox = fs.readFileSync(await dlVox.path()), vpages = readTiff(vox), d0 = vpages[0] || {}, desc = d0.desc || '';
    check(vpages.length > 0 && d0.bits === 8 && d0.comp === 8 && Math.abs(d0.xres - 50) < 1e-6 && /\nspacing=0\.025\n/.test(desc) &&
      /unit=micron/.test(desc) && /\nzorigin=-?[\d.]+\n/.test(desc) && /\nc1=[\d.e+-]+\nvunit=photons\n/.test(desc),
      `voxelize.html: ${dlVox.suggestedFilename()}, ${vpages.length} planes of ${d0.w}×${d0.h}, 8-bit, deflated, 50 px/µm, spacing 0.025 µm, ` +
      `its origin, photons per step as the calibration`);
    // a channel per target: a composite stack, planes c-fastest
    const nT = await vp.evaluate(() => document.getElementById('target').options.length - 1);
    await vp.evaluate(() => { const s = document.getElementById('split'); s.value = 'channels'; s.dispatchEvent(new Event('input')); });
    const [dlCh] = await Promise.all([vp.waitForEvent('download', { timeout: 20000 }), vp.click('#go')]);
    const ch = readTiff(fs.readFileSync(await dlCh.path())), dc = (ch[0] || {}).desc || '';
    const nc = +(/\nchannels=(\d+)\n/.exec(dc) || [])[1], nzc = +(/\nslices=(\d+)\n/.exec(dc) || [])[1];
    check(nT >= 2 && nc === nT && ch.length === nc * nzc && /hyperstack=true\nmode=composite/.test(dc),
      `voxelize.html, a channel each: ${dlCh.suggestedFilename()}, ${nc} channels × ${nzc} slices (${ch.length} planes), composite`);
    // 32-bit, uncompressed: the voxels (made plane by plane) sum to the rows' photons (summed apart, the page's "in all")
    await vp.evaluate(() => {
      const s = document.getElementById('split'); s.value = 'sum';
      document.getElementById('bits').value = '32'; document.getElementById('fmt').value = 'raw';
      s.dispatchEvent(new Event('input'));
    });
    const [dl32] = await Promise.all([vp.waitForEvent('download', { timeout: 20000 }), vp.click('#go')]);
    const b32 = fs.readFileSync(await dl32.path()), p32 = readTiff(b32);
    let vsum = 0;
    for (const p of p32) { const f = new Float32Array(b32.buffer.slice(b32.byteOffset + p.data, b32.byteOffset + p.data + 4 * p.w * p.h)); for (const x of f) vsum += x; }
    const inAll = +(/([\d.e+]+) in all/.exec(await vp.evaluate(() => document.getElementById('out').textContent)) || [])[1];
    // an uncompressed stack's planes back to back (ImageJ reads them as one block): an odd-sized 8-bit plane too
    const oddAt = await vp.evaluate(() => {   // a voxel size whose plane is odd: width and height both odd
      document.getElementById('bits').value = '8';
      const v = document.getElementById('vxy');
      for (let nm = 21; nm < 200; nm++) {
        v.value = nm; v.dispatchEvent(new Event('input'));
        const m = /→ (\d+) × (\d+) ×/.exec(document.getElementById('info').textContent);
        if (m && (+m[1] * +m[2]) % 2 === 1) return nm;
      }
      return 0;
    });
    const [dlOdd] = await Promise.all([vp.waitForEvent('download', { timeout: 20000 }), vp.click('#go')]);
    const pOdd = readTiff(fs.readFileSync(await dlOdd.path())), sz = pOdd.length ? pOdd[0].w * pOdd[0].h : 0;
    check(sz % 2 === 1 && pOdd.every((p, k) => k === 0 || p.data === pOdd[k - 1].data + sz),
      `voxelize.html, uncompressed 8-bit ${pOdd[0] && pOdd[0].w}×${pOdd[0] && pOdd[0].h} (odd, ${oddAt} nm voxels): ${pOdd.length} planes back to back`);
    await vp.evaluate(() => {
      document.getElementById('bits').value = '32'; const v = document.getElementById('vxy'); v.value = 20;
      v.dispatchEvent(new Event('input'));
    });
    check(p32.length > 0 && p32[0].bits === 32 && p32[0].comp === 1 && Math.abs(vsum - inAll) <= 1e-5 * inAll,
      `voxelize.html, 32-bit uncompressed: the voxels sum to ${vsum.toPrecision(7)}, the rows ${inAll}`);
    // the other file: a .zip holding that uncompressed TIFF, slice labels and all
    await vp.evaluate(() => { const f = document.getElementById('fmt'); f.value = 'zip'; f.dispatchEvent(new Event('input')); });
    const [dlZ] = await Promise.all([vp.waitForEvent('download', { timeout: 20000 }), vp.click('#go')]);
    const zz = unzip(fs.readFileSync(await dlZ.path())), zn = Object.keys(zz), inner = zz[zn[0]] || new Uint8Array(0);
    const pz = zn.length === 1 ? readTiff(Buffer.from(inner)) : [];
    check(/\.zip$/.test(dlZ.suggestedFilename()) && zn.length === 1 && zn[0] === dl32.suggestedFilename() && same(inner, b32),
      `voxelize.html, the .zip: ${dlZ.suggestedFilename()} holds ${zn.join()}, the uncompressed TIFF byte for byte (${(inner.length / 1e6).toFixed(1)} MB)`);
    check(pz.length === p32.length && pz[0].comp === 1 && pz[0].labels, `its ${pz.length} planes carry slice labels (Fiji's title line)`);

  } finally {
    fs.rmSync(dir, { recursive: true, force: true });
  }
});
