#!/usr/bin/env node
// SPDX-License-Identifier: BSD-3-Clause
// The live panel in headless Chromium: the performance breakdown has a fixed set of rows (no row comes and goes from
// frame to frame) with one decimal per time; ● Record saves N frames as one 16-bit ImageJ TIFF stack (the pixel size
// as XResolution, the frame interval and every setting in the header): by default a fresh sample (a movie; the live
// sample's time stays), with "continue the live sample" the live sample's time runs on by N frames.
//   node tests/web/viewer_live_record.mjs [--channel=chrome] [--swiftshader]   (the hardware GPU; --swiftshader: software GL, ~10x slower)
import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';
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
  res.setHeader('content-type', f.endsWith('.js') ? 'text/javascript' : 'text/html');
  res.end(fs.readFileSync(f));
}).listen(0);

// The pages of a little-endian TIFF: [{w, h, bits, desc, xres}] (xres = pixels per unit), and the first page's pixels.
function readTiff(buf) {
  const dv = new DataView(buf.buffer, buf.byteOffset, buf.byteLength), pages = [];
  let ifd = dv.getUint32(4, true);
  while (ifd) {
    const n = dv.getUint16(ifd, true), t = {};
    for (let i = 0; i < n; i++) {
      const o = ifd + 2 + 12 * i, tag = dv.getUint16(o, true), type = dv.getUint16(o + 2, true), cnt = dv.getUint32(o + 4, true);
      t[tag] = type === 3 ? dv.getUint16(o + 8, true) : type === 5 ? (() => { const at = dv.getUint32(o + 8, true); return dv.getUint32(at, true) / dv.getUint32(at + 4, true); })()
        : type === 2 ? new TextDecoder().decode(buf.subarray(dv.getUint32(o + 8, true), dv.getUint32(o + 8, true) + cnt - 1)) : dv.getUint32(o + 8, true);
    }
    pages.push({ w: t[256], h: t[257], bits: t[258], desc: t[270], xres: t[282], data: t[273] });
    ifd = dv.getUint32(ifd + 2 + 12 * n, true);
  }
  return pages;
}

let fail = 0;
const t0 = Date.now(), check = (ok, what) => { if (!ok) fail++; console.log(`${ok ? 'ok  ' : 'FAIL'}  ${what}  [${((Date.now() - t0) / 1000).toFixed(0)} s]`); };
const browser = await chromium.launch({ channel: arg('channel') || undefined, args: arg('swiftshader') ? ['--use-angle=swiftshader', '--enable-unsafe-swiftshader'] : ['--enable-gpu', '--ignore-gpu-blocklist'] });
const ctx = await browser.newContext({ viewport: { width: 1200, height: 800 }, acceptDownloads: true });
const page = await ctx.newPage();
const errors = [];
page.on('pageerror', e => errors.push(e.message));
try {
  await page.goto(`http://localhost:${server.address().port}/index.html?live=0`);
  await page.waitForFunction(() => window.liveState && typeof genWorkers !== 'undefined' && genWorkers.ok, null, { timeout: 120000 });
  await page.evaluate(() => {
    document.querySelector('#levelSeg button[data-level=expert]').click();
    document.getElementById('mv_size').value = 64; document.getElementById('mv_size').dispatchEvent(new Event('input', { bubbles: true }));
    document.getElementById('lv_live').click();
  });
  // the performance rows of a few frames: the same names in the same order every frame
  const seen = [];
  for (let k = 0; k < 4; k++) {
    await page.waitForFunction(n => liveState.frames >= n, 3 + k, { timeout: 180000 });
    seen.push(await page.evaluate(() => [...document.querySelectorAll('#lv_perfOut span:nth-child(2n+1)')].map(e => e.textContent).join('|') + '#' +
      [...document.querySelectorAll('#lv_perfOut span:nth-child(2n)')].map(e => e.textContent).join('|')));
  }
  const infoLine = await page.evaluate(() => { const e = document.getElementById('lv_info'); return { text: e.textContent, title: e.title, h: e.clientHeight, sh: e.scrollHeight, lh: parseFloat(getComputedStyle(e).lineHeight) }; });
  check(/^\d+\.\d fps · \d+\.\d ms\/frame$/.test(infoLine.text) && /\n64×64 px · \d+\.\d nm\/px(\n|$)/.test(infoLine.title) && infoLine.h <= infoLine.lh + 1 && infoLine.sh <= infoLine.lh + 1,
    `the info is one line, the speed only: "${infoLine.text}" (${infoLine.h} px), the sizes in its tooltip`);
  // the bar: only the info gives way, the buttons keep their places and sizes whatever its text
  const bar = await page.evaluate(() => {
    const at = () => ['lv_live', 'lv_snap', 'lv_rec', 'lv_fresh', 'levelSeg', 'menuBtn'].map(i => { const r = document.getElementById(i).getBoundingClientRect(); return r.left.toFixed(1) + '+' + r.width.toFixed(1); }).join(' ');
    const e = document.getElementById('lv_info'), b = document.getElementById('lv_busy'), keep = e.textContent, a = at();
    e.textContent = 'x'.repeat(400); b.hidden = false;
    const z = at(), over = document.getElementById('liveBar').scrollWidth - document.getElementById('liveBar').clientWidth;
    e.textContent = keep; b.hidden = true;
    return { a, z, over };
  });
  check(bar.a === bar.z && bar.over <= 0, `the bar's buttons stay put under a long info and the busy marker (overflow ${bar.over} px)`);
  // the image with the Acquire controls under it at its width, the settings (search on top) a column at the right edge
  const layout = () => page.evaluate(() => { const R = id => document.getElementById(id).getBoundingClientRect();
    const v = R('liveView'), c = R('liveCtl'), s = R('setCol'), vb = R('viewBar'), lb = R('liveBar');
    const bar = document.getElementById('liveBar');
    return { w: v.width, h: v.height, liveLeft: v.left, barOver: bar.scrollWidth - bar.clientWidth, cw: c.width, ctlBottom: c.bottom, setRight: s.right, setBottom: s.bottom, setW: s.width, W: innerWidth,
      H: innerHeight, vbH: vb.height, lbH: lb.height, illum: !!document.querySelector('#liveCtl details[data-group=illum]'),
      search: document.getElementById('sideTop').parentElement.id }; });
  const sq = await layout();
  check(Math.abs(sq.w - sq.h) < 1 && Math.abs(sq.cw - sq.w) < 2 && Math.abs(sq.ctlBottom - sq.H) < 2 && Math.abs(sq.setRight - sq.W) < 2
    && Math.abs(sq.setBottom - sq.H) < 2 && Math.abs(sq.vbH - sq.lbH) < 1 && sq.illum && sq.search === 'setCol',
    `layout: the image square (${sq.w.toFixed(0)} px), the controls as wide and to the bottom, the settings column at the right edge with the search, the bars level (${sq.vbH.toFixed(0)}/${sq.lbH.toFixed(0)} px), illumination in the controls ${JSON.stringify(sq)}`);
  // the settings column folds to a tab at the right edge (the image keeps its size and moves right) and opens again
  await page.click('#setFold');
  await page.waitForTimeout(300);
  const shut = await layout();
  await page.click('#setTab');
  await page.waitForTimeout(300);
  const open = await layout();
  check(shut.setW < 40 && Math.abs(shut.setRight - shut.W) < 2 && Math.abs(shut.w - sq.w) < 1 && Math.abs(shut.w - shut.h) < 1
    && shut.liveLeft > sq.liveLeft + 250 && shut.barOver <= 0 && Math.abs(open.w - sq.w) < 1 && Math.abs(open.liveLeft - sq.liveLeft) < 1,
    `the settings fold to a ${shut.setW.toFixed(0)} px tab (the image stays ${shut.w.toFixed(0)} px, moved ${(shut.liveLeft - sq.liveLeft).toFixed(0)} px right, the bar fits: overflow ${shut.barOver} px) and open again`);
  // where the groups sit: the instrument under the image beside the acquisition controls, the simulation's in the settings
  const where = await page.evaluate(() => Object.fromEntries(['illum', 'lightpath', 'objective', 'camera', 'perf', 'formation']
    .map(g => [g, document.querySelector(`details[data-group=${g}]`).closest('#lv_colIllum,#lv_instrHost,#panel').id])));
  check(where.illum === 'lv_colIllum' && where.lightpath === 'lv_instrHost' && where.objective === 'lv_instrHost' && where.camera === 'lv_instrHost'
    && where.perf === 'panel' && where.formation === 'panel', `the groups' places ${JSON.stringify(where)}`);
  // the stage box: a click near the joystick's right edge moves one step right, one near its lower-left corner one
  // step down and left; dragging the knob right moves the stage right while held and stops when let go (the knob back
  // in the middle); Shift + ↓ on it 10 steps; the readout shows the position; the focus wheel steps Z (a click above
  // its line one up, a 36 px drag up three up, a scroll notch down one down); a right-click opens the step box
  const st = await page.evaluate(() => ({ x0: view.cx, y0: view.cy, z0: +document.getElementById('mv_z').value,
    order: [...document.querySelectorAll('#lv_cols .lc>h5')].map(h => h.textContent).slice(0, 3).join(',') }));
  const jb = await (await page.$('#lv_joy')).boundingBox(), jc = [jb.x + jb.width / 2, jb.y + jb.height / 2], R = jb.width / 2;
  const pos = () => page.evaluate(() => [view.cx, view.cy]);
  await page.mouse.click(jc[0] + 0.8 * R, jc[1]);
  const p1 = await pos();
  await page.mouse.click(jc[0] - 0.6 * R, jc[1] + 0.6 * R);
  const p2 = await pos();
  await page.mouse.move(jc[0], jc[1]); await page.mouse.down();
  for (let i = 1; i <= 6; i++) await page.mouse.move(jc[0] + 5 * i, jc[1]);
  await page.waitForTimeout(500);
  const p3 = await pos();
  await page.mouse.up();
  await page.waitForTimeout(200);
  const p4 = await pos(), knobBack = await page.$eval('#lv_joy .jk', e => e.style.transform === '');
  await page.waitForTimeout(200);
  const p5 = await pos();
  await page.keyboard.down('Shift'); await page.keyboard.press('ArrowDown'); await page.keyboard.up('Shift');   // (the joystick has the focus from its clicks)
  const p6 = await pos();
  await page.keyboard.down('Shift'); await page.mouse.click(jc[0] - 0.8 * R, jc[1]); await page.keyboard.up('Shift');
  const p7 = await pos();
  const kb = await (await page.$('#mKnob')).boundingBox(), kz = () => page.$eval('#mv_z', e => +e.value);
  const za = await kz();
  await page.mouse.click(kb.x + kb.width / 2, kb.y + kb.height * 0.25);
  const zb = await kz();
  await page.mouse.move(kb.x + kb.width / 2, kb.y + kb.height * 0.85); await page.mouse.down();
  for (let i = 1; i <= 12; i++) { await page.mouse.move(kb.x + kb.width / 2, kb.y + kb.height * 0.85 - 3 * i); await page.waitForTimeout(16); }
  await page.mouse.up();
  const zc = await kz();
  await page.mouse.move(kb.x + kb.width / 2, kb.y + kb.height / 2); await page.mouse.wheel(0, 100); await page.waitForTimeout(50);
  const zd = await kz();
  const zShown = await page.$eval('#mZ', e => e.innerText.trim());
  await page.click('#mFocus button[data-dir="1"]');
  const ze = await kz();
  await page.click('#lv_joy', { button: 'right' });
  const pop = await page.evaluate(() => ({ open: !document.getElementById('lv_xyPop').hidden, focus: document.activeElement.id }));
  await page.mouse.click(5, 300);
  const popShut = await page.evaluate(() => document.getElementById('lv_xyPop').hidden);
  await page.evaluate(s => { view.cx = s.x0; view.cy = s.y0; requestDraw();
    const z = document.getElementById('mv_z'); z.value = s.z0; z.dispatchEvent(new Event('input', { bubbles: true })); z.dispatchEvent(new Event('change', { bubbles: true })); }, st);
  const near = (a, b, e = 1e-6) => Math.abs(a - b) < e;
  const ran = p3[0] - p2[0];
  check(near(p1[0] - st.x0, 1) && near(p1[1], st.y0) && near(p2[0] - p1[0], -1) && near(p2[1] - p1[1], 1)
    && ran > 0.3 && near(p3[1], p2[1]) && near(p5[0], p4[0]) && knobBack && near(p6[1] - p5[1], 10) && near(p7[0] - p6[0], -10),
    `the joystick: a click at the right edge +${(p1[0] - st.x0).toFixed(2)} µm in x, at the lower left (${(p2[0] - p1[0]).toFixed(2)}, +${(p2[1] - p1[1]).toFixed(2)}) µm; `
    + `the knob held right moved ${ran.toFixed(2)} µm in 0.5 s and stopped when let go (knob back: ${knobBack}); Shift+↓ +${(p6[1] - p5[1]).toFixed(2)} µm, Shift+click at the left ${(p7[0] - p6[0]).toFixed(2)} µm`);
  check(near(zb - za, 0.1) && near(zc - zb, 0.3) && near(zd - zc, -0.1) && zShown === zd.toFixed(2) && near(ze - zd, 0.1) && pop.open && pop.focus === 'lv_xyStep' && popShut && st.order === 'Stage,Display,Acquisition',
    `the focus wheel: click above +${(zb - za).toFixed(2)}, 36 px drag up +${(zc - zb).toFixed(2)}, a scroll notch down ${(zd - zc).toFixed(2)} µm, shown under it as "${zShown}", ▲ +${(ze - zd).toFixed(2)}; `
    + `right-click opens the step box (${JSON.stringify(pop)}), a click elsewhere shuts it; the boxes ${st.order}`);
  // the handle spreads the controls over the image's lower 75 %; they fold back 0.1 s after the pointer leaves them
  await page.click('#ctlUp');
  await page.waitForTimeout(200);   // (the 0.1 s slide)
  const up = await page.evaluate(() => { const v = document.getElementById('liveView').getBoundingClientRect(), c = document.getElementById('liveCtl').getBoundingClientRect();
    return { top: (c.top - v.top) / v.height, bottom: c.bottom, H: innerHeight, w: c.width - v.width }; });
  await page.mouse.move(5, 300);
  const still = await page.evaluate(() => document.getElementById('sideMain').classList.contains('ctlUp'));
  await page.waitForTimeout(400);
  const back = await page.evaluate(() => !document.getElementById('sideMain').classList.contains('ctlUp') &&
    Math.abs(document.getElementById('liveCtl').getBoundingClientRect().top - document.getElementById('liveView').getBoundingClientRect().bottom) < 2);
  check(Math.abs(up.top - 0.25) < 0.01 && Math.abs(up.bottom - up.H) < 2 && Math.abs(up.w) < 2 && still && back,
    `the handle spreads the controls from ${(100 * up.top).toFixed(0)} % of the image to the bottom; still spread as the pointer leaves, folded back by 0.4 s`);
  // resting the pointer on the handle spreads them too: not at 0.15 s, by 0.45 s
  const hb = await (await page.$('#ctlUp')).boundingBox();
  await page.mouse.move(hb.x + hb.width / 2, hb.y + hb.height / 2);
  await page.waitForTimeout(150);
  const early = await page.evaluate(() => document.getElementById('sideMain').classList.contains('ctlUp'));
  await page.waitForTimeout(300);
  const rest = await page.evaluate(() => document.getElementById('sideMain').classList.contains('ctlUp'));
  await page.mouse.move(5, 300);
  await page.waitForTimeout(400);
  const gone = await page.evaluate(() => !document.getElementById('sideMain').classList.contains('ctlUp'));
  check(!early && rest && gone, `the pointer resting on the handle spreads them (0.15 s: ${early}, 0.45 s: ${rest}), leaving folds them back (${gone})`);
  // the BFP readout, from the movie spec's C++ setup (the setup job), names a regime (the spectra: viewer_scope_view.mjs)
  const ro = await page.waitForFunction(() => {
    const b = document.getElementById('bfpReadout');
    return b && /^(Epi|HILO|TIRF|Clipped)\b/.test(b.textContent) && b.textContent;
  }, null, { timeout: 30000 }).then(h => h.jsonValue(), () => null);
  check(!!ro, `the BFP readout from the setup job: ${ro ? `"${ro}"` : 'nothing in 30 s'}`);
  await page.evaluate(() => document.getElementById('lv_live').click());
  const names = seen.map(s => s.split('#')[0]), vals = seen.map(s => s.split('#')[1].split('|'));
  check(names.every(n => n === names[0]) && /blink events \(core\)/.test(names[0]) && /BrightField image/.test(names[0]),
    `the performance rows stay put: ${names[0].split('|').length} rows in every frame`);
  check(vals.every(v => v.every(x => x === '—' || /^\d+\.\d ms$/.test(x))), 'every time has one decimal (or "—"): ' + vals[3].slice(0, 4).join(', '));

  // Record 20 frames at 128 x 128 (at 64 x 64 a frame is cheap either way)
  await page.evaluate(() => { const s = document.getElementById('mv_size'); s.value = 128; s.dispatchEvent(new Event('input', { bubbles: true })); });
  // one recording of 20 frames: [download, its TIFF pages, the buffer, the page's state after it]
  const record = async () => {
    const tRec = Date.now();
    // ● Record opens the box, its Start records
    const shown = await page.evaluate(() => { document.getElementById('lv_rec').click(); return !document.getElementById('lv_recPop').hidden; });
    let early = false;
    page.once('download', () => { early = true; });
    await page.evaluate(() => document.getElementById('lv_recGo').click());
    const hiddenAfter = await page.evaluate(() => document.getElementById('lv_recPop').hidden);
    await page.waitForFunction(() => liveState.play.on, null, { timeout: 240000 });
    const recSec = (Date.now() - tRec) / 1000;
    // no file yet: the recording plays in the live view, frame after frame
    const i0 = await page.evaluate(() => liveState.play.i);
    // read in the same call that sees it move: at 20 frames x 50 ms the loop wraps every second, so a later read can be
    // back at i0
    const play = await page.waitForFunction(i => liveState.play.i !== i &&
      { shown: !document.getElementById('lv_play').hidden, i: liveState.play.i, n: liveState.play.n }, i0, { timeout: 10000 })
      .then(h => h.jsonValue()).catch(() => page.evaluate(() => ({ shown: !document.getElementById('lv_play').hidden, i: liveState.play.i, n: liveState.play.n })));
    check(!early && play.shown && play.i !== i0 && play.n === 20, `the recording plays in the live view (no file saved by itself): frame ${i0 + 1} -> ${play.i + 1}/${play.n}`);
    const [dl] = await Promise.all([page.waitForEvent('download', { timeout: 60000 }), page.evaluate(async () => {
      // Save opens a box (viewer_save.mjs; once it knows the recording's kind); the movie alone is the .tif
      const dlg = document.getElementById('setDlg');
      dlg.hidden = true;   // (a box saved before keeps its old buttons, hidden)
      document.getElementById('lv_playSave').click();
      while (dlg.hidden || !document.getElementById('svGo')) await new Promise(r => setTimeout(r, 20));
      document.getElementById('svSet').checked = false; document.getElementById('svGt').checked = false;
      document.getElementById('svGo').click();
    })]);
    const buf = fs.readFileSync(await dl.path()), pages = readTiff(buf);
    const info = await page.evaluate(() => ({ t: liveState.t, text: document.getElementById('lv_info').textContent, title: document.getElementById('lv_info').title, btn: document.getElementById('lv_rec').textContent,
      px: +document.getElementById('mv_pixel-nm').value, exp: +document.getElementById('mv_exposure-ms').value }));
    check(shown && hiddenAfter, 'Record opens the frames box, Start closes it and records');
    return { dl, pages, buf, info, recSec };
  };
  const frameOf = (r, k) => new Uint16Array(r.buf.buffer.slice(r.buf.byteOffset + r.pages[k].data, r.buf.byteOffset + r.pages[k].data + 128 * 128 * 2));
  const fileChecks = (r, kind) => {
    const d0 = r.pages[0] || {};
    check(r.dl.suggestedFilename() === 'insiliscope_live_128x128x20.tif' && r.pages.length === 20 && r.pages.every(p => p.w === 128 && p.h === 128 && p.bits === 16),
      `${kind}: ${r.dl.suggestedFilename()}: ${r.pages.length} pages of ${d0.w}×${d0.h}, 16 bit (${(r.buf.length / 1e6).toFixed(2)} MB)`);
    check(/^ImageJ=1\.11a\nimages=20\nframes=20\nunit=micron\nfinterval=0\.05\n/.test(d0.desc || '') && /\ninsiliscope\.spec=\S/.test(d0.desc) &&
      / exposure-ms=50( |$)/.test((/insiliscope\.spec=(.*)/.exec(d0.desc) || [])[1] || ''),
      `${kind}: the header: ImageJ frames, unit, frame interval and the full engine spec`);
    check(Math.abs(d0.xres - 1000 / r.info.px) < 1e-4, `${kind}: the pixel size: ${(1000 / d0.xres).toFixed(2)} nm (viewer ${r.info.px.toFixed(2)} nm)`);
    check(frameOf(r, 0).some((v, i) => v !== frameOf(r, 19)[i]), `${kind}: the frames differ (blinks, noise)`);
    // ImageJ / Fiji read an "ImageJ=" file's frames as one block from the first page's data, not page by page
    check(r.pages.every((p, k) => p.data === d0.data + k * 128 * 128 * 2), `${kind}: the frames lie back to back from the first page's data (as Fiji reads them)`);
    check(r.info.btn === '● Record' && /saved insiliscope_live_128x128x20\.tif/.test(r.info.title), `${kind}: the button is back, the info's tooltip names the saved file`);
    return d0;
  };
  await page.evaluate(() => { const n = document.getElementById('lv_recN'); n.value = 20; n.dispatchEvent(new Event('change')); });

  // the default: a fresh sample (a movie from the Start time); the live sample is left as it was
  const tBefore = await page.evaluate(() => new Promise(r => setTimeout(() => r(liveState.t), 300)));
  const fresh = await record();
  const df = fileChecks(fresh, 'fresh');
  check(/\ninsiliscope=recording\n/.test(df.desc || '') && / frames=20( |$)/.test((/insiliscope\.spec=(.*)/.exec(df.desc) || [])[1] || '') &&
    / start-sec=0( |$)/.test((/insiliscope\.spec=(.*)/.exec(df.desc) || [])[1] || ''),
    'fresh: the header says a fresh recording, its spec is the 20-frame movie from the box\'s start time (default 0 s)');
  check(fresh.info.t === tBefore, `fresh: the live sample's time stays: ${tBefore.toFixed(2)} -> ${fresh.info.t.toFixed(2)} s`);
  console.log(`      fresh record ${fresh.recSec.toFixed(1)} s for 20 frames (wall)`);

  // "continue the live sample": the frames go on from the live sample
  await page.evaluate(() => { const c = document.getElementById('lv_recLive'); c.checked = true; c.dispatchEvent(new Event('change')); });
  const t2 = await page.evaluate(() => liveState.t);
  const cont = await record();
  const dc = fileChecks(cont, 'continue');
  check(/\ninsiliscope=live recording\n/.test(dc.desc || ''), 'continue: the header says a live recording');
  check(Math.abs(cont.info.t - (t2 + 20 * cont.info.exp / 1000)) < 1e-6, `continue: the sample's time runs on by 20 frames: ${t2.toFixed(2)} -> ${cont.info.t.toFixed(2)} s`);
  const recMs = await page.evaluate(() => liveState.rec.engineMs / 20);
  console.log(`      continue record ${cont.recSec.toFixed(1)} s for 20 frames (wall); engine ${recMs.toFixed(1)} ms/frame; info: ${cont.info.text.split('\n').pop()}`);
  // Not a check: a frame's cost depends on the light the place has had (the core's blink-event query), so a snap and a
  // recording at different moments are not comparable; LiveScope::Frames' speed is measured in ctest live_scope.
  check(!errors.length, 'no page errors' + (errors.length ? ': ' + errors.join('; ') : ''));
} catch (e) { check(false, 'error: ' + e.message); }
await browser.close();
server.close();
console.log(`${fail ? 'FAIL' : 'ok'}: viewer live record (${((Date.now() - t0) / 1000).toFixed(0)} s)`);
process.exit(fail ? 1 : 0);
