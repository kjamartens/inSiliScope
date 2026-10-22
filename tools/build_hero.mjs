#!/usr/bin/env node
// SPDX-License-Identifier: BSD-3-Clause
// The gallery's and Home page's 3D overview: the viewer in headless Chromium at the gallery's spot (the viewer's start
// position), tilted, turned and in perspective with the distance haze, every cell detailed, the chunk grid on; then its
// live microscope's DNA-PAINT and widefield frames of the camera box as two insets on the floor under the box, tied to
// its near corners by dashed lines. The DNA-PAINT imagers are the gallery's (gallery/manifest.json defaults: a tenth of
// the cli's, bound 10x longer). Two files: overview_3d.jpg (a still, full size) and overview_3d.gif (a loop of
// LOOP.frames frames at LOOP.fps in four eased legs: at the low focus the camera box goes from the right-hand edge of
// the cell under the start position (on a line that runs to the right on screen, a little up, clear of the insets; at
// most LOOP.maxUm long) in to its nucleus, the focus rises to LOOP.zHigh there and comes down again, and the box goes
// back out; each frame with its own live widefield and DNA-PAINT frame, so the field moves, the focus goes through the
// nucleus and the single molecules blink; the box is drawn at the focal plane). Rendered by release.yml before tools/build_gallery.py, which puts it above the 2x2 overview;
// tools/build_site.sh shows it on Home; README.md shows the published GIF.
//   node tools/build_hero.mjs --out site_gallery [--channel=chrome] [--gpu]   (a few min with --gpu, longer on SwiftShader; needs web/tools' Playwright)
import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';
import { createRequire } from 'node:module';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const require = createRequire(path.join(ROOT, 'web/tools/package.json'));
const { chromium } = require('playwright');
const arg = k => { const a = process.argv.find(s => s.startsWith('--' + k)); return a ? (a.includes('=') ? a.split('=')[1] : process.argv[process.argv.indexOf(a) + 1]) : null; };
const OUT = arg('out') || 'site_gallery';
const VIEW = { tiltDeg: 72, azimuthDeg: 20, perspDeg: 16, perspMist: 70, detailCells: 0, zoomSteps: 4 };
const man = JSON.parse(fs.readFileSync(path.join(ROOT, 'gallery/manifest.json'), 'utf8')).defaults;
const PAINT = Object.fromEntries(['mt-imager-nm', 'mt-paint-on-sec', 'npc-imager-nm', 'npc-paint-on-sec'].map(k => [k, man[k]]));
const LIVE_SEC = 15;
// the loop: frames (four legs), frame rate, the longest path (um), how far towards the edge it goes (a fraction of
// the nucleus-to-edge line), its direction on screen in the final view (deg, counterclockwise from the right), the low
// and high focus (um above the coverslip), the GIF's width
const LOOP = { frames: 80, fps: 8, maxUm: 20, reach: 0.75, screenDeg: 10, zLow: 0.5, zHigh: 4, gifWidth: 720 };
const types = { '.js': 'text/javascript', '.mjs': 'text/javascript', '.html': 'text/html', '.css': 'text/css', '.wasm': 'application/wasm',
  '.json': 'application/json', '.svg': 'image/svg+xml', '.png': 'image/png', '.jpg': 'image/jpeg' };
const server = http.createServer((req, res) => {
  const f = path.join(ROOT, 'web', decodeURIComponent(req.url.split('?')[0]).replace(/^\/+/, '') || 'index.html');
  if (!f.startsWith(path.join(ROOT, 'web')) || !fs.existsSync(f) || fs.statSync(f).isDirectory()) { res.statusCode = 404; res.end(); return; }
  res.setHeader('content-type', types[path.extname(f)] || 'application/octet-stream');
  res.end(fs.readFileSync(f));
}).listen(0);

// a clipped page screenshot of an element (an element screenshot waits for a "stable" page, which a view that
// keeps redrawing on software GL never is); the clip is measured once, on the fresh page (the layout stays, and a page
// busy with a software-GL repaint can miss a 30 s timeout)
const clips = {};
const clipOf = async (page, sel) => (clips[sel] ||= await page.locator(sel).boundingBox({ timeout: 300000 }));
const shot = async (page, sel) => page.screenshot({ clip: await clipOf(page, sel), timeout: 300000 });
const t0 = Date.now(), log = s => console.log(`${s}  [${((Date.now() - t0) / 1000).toFixed(0)} s]`);
// --gpu: the machine's GPU (else SwiftShader, as on a CI runner: slow with every cell's microtubules)
const browser = await chromium.launch({ channel: arg('channel') || undefined,
  args: process.argv.includes('--gpu') ? [] : ['--use-angle=swiftshader', '--enable-unsafe-swiftshader'] });
// 1600 x 828 CSS px at 2x: the world view's canvas is square (1588 x 1588 device px with the x-z view under it)
const ctx = await browser.newContext({ viewport: { width: 1600, height: 828 }, deviceScaleFactor: 2, colorScheme: 'dark' });
const page = await ctx.newPage();
page.on('pageerror', e => console.log('pageerror', e.message));
try {
  await page.goto(`http://localhost:${server.address().port}/index.html?live=0`);
  await page.waitForFunction(() => window.liveState && typeof genWorkers !== 'undefined' && genWorkers.ok, null, { timeout: 120000 });
  await clipOf(page, '#canvasHost');
  const box = await page.locator('#cv').boundingBox();
  await page.mouse.move(box.x + box.width * 0.48, box.y + box.height * 0.5);
  for (let i = 0; i < VIEW.zoomSteps; i++) { await page.mouse.wheel(0, -300); await page.waitForTimeout(400); }
  const set = vals => page.evaluate(vals => {
    for (const [id, val] of Object.entries(vals)) {
      const e = document.getElementById(id);
      if (e.type === 'checkbox') e.checked = val; else e.value = val;
      e.dispatchEvent(new Event('input', { bubbles: true })); e.dispatchEvent(new Event('change', { bubbles: true }));
    }
  }, vals);
  // The stage's path: the cell under the start position (its outline once its asset is in), from its nucleus centre
  // to its edge; of the directions every 2 degrees, the one closest to LOOP.screenDeg on screen in the final view.
  await page.waitForFunction(() => {
    const k = typeof packCache !== 'undefined' && packCache && nearestCell(view.cx, view.cy), a = k && assets.get(k);
    return !!(a && a.entry && a.entry.mesh);
  }, null, { timeout: 120000 });
  const line = await page.evaluate(([V, L]) => {
    const inCell = (c, x, y) => {
      const a = assets.get(c.cx + ',' + c.cy), dx = x - c.x, dy = y - c.y, d = Math.hypot(dx, dy);
      if (d > c.rOuter || !(a && a.entry && a.entry.mesh)) return false;
      const tab = footprintPolar(a.entry), r = -(c.packRot || 0), lx = dx * Math.cos(r) - dy * Math.sin(r), ly = dx * Math.sin(r) + dy * Math.cos(r);
      return d <= tab[((Math.floor(Math.atan2(ly, lx) / (2 * Math.PI) * FOOT_BINS) % FOOT_BINS) + FOOT_BINS) % FOOT_BINS];
    };
    const c = [...packCache.map.values()].find(c => inCell(c, view.cx, view.cy)) || packCache.map.get(nearestCell(view.cx, view.cy));
    const cr = Math.cos(c.packRot || 0), sr = Math.sin(c.packRot || 0);
    const N = [c.x + c.nucOffX * cr - c.nucOffY * sr, c.y + c.nucOffX * sr + c.nucOffY * cr];
    const cam = cameraFor({ pivot: [N[0], N[1], 0], azimuthDeg: V.azimuthDeg, tiltDeg: V.tiltDeg, scale: view.scale }, 1000, 1000);
    const s0 = cam.project(N[0], N[1], 0);
    let best = null;
    for (let deg = 0; deg < 360; deg += 2) {
      const u = [Math.cos(deg * Math.PI / 180), Math.sin(deg * Math.PI / 180)], s1 = cam.project(N[0] + u[0], N[1] + u[1], 0);
      const off = Math.abs(((Math.atan2(s0[1] - s1[1], s1[0] - s0[0]) * 180 / Math.PI - L.screenDeg) % 360 + 540) % 360 - 180);
      if (!best || off < best.off) best = { u, off };
    }
    let len = 0;
    while (len < 60 && inCell(c, N[0] + best.u[0] * (len + 0.1), N[1] + best.u[1] * (len + 0.1))) len += 0.1;
    const a = Math.max(0, len - L.maxUm);
    return { cell: c.cx + ',' + c.cy, len, A: [N[0] + best.u[0] * a, N[1] + best.u[1] * a], B: [N[0] + best.u[0] * len, N[1] + best.u[1] * len] };
  }, [VIEW, LOOP]);
  line.B = [line.A[0] + (line.B[0] - line.A[0]) * LOOP.reach, line.A[1] + (line.B[1] - line.A[1]) * LOOP.reach];
  // [x, y, focus] per frame: edge -> nucleus (low), up, down, nucleus -> edge (low); each leg eased at both ends
  const ease = t => (1 - Math.cos(Math.PI * t)) / 2, lerp = (a, b, t) => a + (b - a) * t;
  const poses = Array.from({ length: LOOP.frames }, (_, i) => {
    const q = 4 * i / LOOP.frames, leg = Math.floor(q), t = ease(q - leg);
    const f = leg === 0 ? 1 - t : leg === 3 ? t : 0;   // 1 = the edge, 0 = the nucleus
    const z = leg === 1 ? lerp(LOOP.zLow, LOOP.zHigh, t) : leg === 2 ? lerp(LOOP.zHigh, LOOP.zLow, t) : LOOP.zLow;
    return [lerp(line.A[0], line.B[0], f), lerp(line.A[1], line.B[1], f), z];
  });
  const mid = [(line.A[0] + line.B[0]) / 2, (line.A[1] + line.B[1]) / 2, LOOP.zLow];
  log(`path: cell ${line.cell}, nucleus to edge ${line.len.toFixed(1)} um, ${Math.hypot(line.B[0] - line.A[0], line.B[1] - line.A[1]).toFixed(1)} um travelled`);
  // the stage and the focus (the live Focus field, mirrored into the movie settings)
  const moveTo = p => page.evaluate(p => {
    view.cx = p[0]; view.cy = p[1];
    const z = document.getElementById('lv_z');
    if (+z.value !== p[2]) { z.value = p[2]; z.dispatchEvent(new Event('input', { bubbles: true })); z.dispatchEvent(new Event('change', { bubbles: true })); }
    requestDraw();
  }, p);
  // the live display range: automatic while the light settles; then the widest automatic range of the loop's extremes
  // (the edge, the nucleus low and high) is fixed, so the loop neither flickers nor clips over the nucleus
  const autoRange = on => page.evaluate(on => { const a = document.getElementById('lv_auto'); a.checked = on; a.dispatchEvent(new Event('change')); }, on);
  const fixRange = async () => {
    let lo = Infinity, hi = -Infinity;
    for (const p of [poses[0], poses[LOOP.frames / 4], poses[LOOP.frames / 2]]) {
      await moveTo(p);
      const f0 = await page.evaluate(() => liveState.frames);
      await page.waitForFunction(f0 => liveState.frames >= f0 + 8, f0, { timeout: 120000 });   // the auto range is smoothed
      const r = await page.evaluate(() => [+document.getElementById('lv_lo').value, +document.getElementById('lv_hi').value]);
      lo = Math.min(lo, r[0]); hi = Math.max(hi, r[1]);
    }
    await page.evaluate(([lo, hi]) => {
      const a = document.getElementById('lv_lo'), b = document.getElementById('lv_hi');
      a.value = lo; b.value = hi; b.dispatchEvent(new Event('input'));   // turns the automatic range off
    }, [lo, hi]);
    return [lo, hi];
  };
  // One live frame per pose (a frame started after the move: two new frames), kept in the page as a canvas copy.
  const liveRun = async key => {
    await page.evaluate(k => { window[k] = []; }, key);
    for (const p of poses) {
      await moveTo(p);
      const f0 = await page.evaluate(() => liveState.frames);
      await page.waitForFunction(f0 => liveState.frames >= f0 + 2, f0, { timeout: 120000 });
      await page.evaluate(k => {
        const s = document.getElementById('lv_cv'), c = document.createElement('canvas');
        c.width = s.width; c.height = s.height; c.getContext('2d').drawImage(s, 0, 0); window[k].push(c);
      }, key);
    }
    await moveTo(poses[0]);
  };
  // First the world view, once, centred on the path and without its camera box: the composite draws the box at each
  // pose (the stage moves under a still view, so only the box and the insets change from frame to frame). It goes first,
  // on a page with nothing else to do: on software GL every cell's microtubules take minutes to arrive and seconds per
  // repaint.
  await moveTo(mid);
  await page.evaluate(() => { window.drawPlannedFov = () => {}; });
  await set({ showMt: true, showNpcs: true, showGrid: true, perspCut: false, detailCells: VIEW.detailCells, tiltDeg: VIEW.tiltDeg,
    azimuthDeg: VIEW.azimuthDeg, perspDeg: VIEW.perspDeg, perspMist: VIEW.perspMist });
  // the cells delivered: no block packing, no cell asset in flight, at most 3 cells still loading, and the microtubule
  // count unchanged over 8 polls and 3 s (on software GL one repaint can block the page for seconds, so time alone
  // passes at the first poll after it); at most 10 min
  await page.waitForFunction(() => {
    const t = document.getElementById('hudText').textContent, n = +document.getElementById('hud').dataset.microtubules || 0;
    const m = /loading (\d+) cell/.exec(t);
    if (n !== window.__heroN || /packing/.test(t) || !n || (m && +m[1] > 3) || assetInflight.size) {
      window.__heroN = n; window.__heroT = performance.now(); window.__heroP = 0; return false;
    }
    return ++window.__heroP >= 8 && performance.now() - window.__heroT > 3000;
  }, null, { timeout: 600000, polling: 250 }).catch(async () => log('cells still arriving after 10 min, going on: '
    + await page.evaluate(() => document.getElementById('hudText').textContent.replace(/\s+/g, ' '))));
  // the camera box of every pose on screen (the frame drawn with these settings), as the viewer draws it
  const info = await page.evaluate(async poses => {
    document.getElementById('hud').style.display = 'none';
    await new Promise(r => requestAnimationFrame(() => requestAnimationFrame(r)));
    // drawPlannedFov's square, moved by each pose's offset, through the camera of the frame on screen
    const n = +document.getElementById('mv_size').value, px = (+document.getElementById('mv_pixel-nm').value || 100) / 1000, size = n * px;
    const half = document.getElementById('mv_modality').value === '1' ? 0 : px / 2, x0 = -size / 2 - half, y0 = x0;
    const cvr = cv.getBoundingClientRect(), host = document.getElementById('canvasHost').getBoundingClientRect();
    const k = devicePixelRatio, ox = (cvr.left - host.left) * k, oy = (cvr.top - host.top) * k;
    return { mt: +document.getElementById('hud').dataset.microtubules, dpr: k,
      boxes: poses.map(([cx, cy, cz]) => [[x0, y0], [x0 + size, y0], [x0 + size, y0 + size], [x0, y0 + size]].map(([x, y]) => {
        const [sx, sy] = project(cx + x, cy + y, cz); return [sx + ox, sy + oy]; })) };
  }, poses);
  await page.waitForTimeout(500);
  const world = (await shot(page, '#canvasHost')).toString('base64');
  log(`world view: ${info.mt} microtubules`);
  // Then the live frames, on a light flat view without its microtubules and pores (the world view repaints with every
  // live frame, and a view of every cell's microtubules takes seconds per repaint).
  await set({ perspDeg: 0, showMt: false, showNpcs: false, ...Object.fromEntries(Object.entries(PAINT).map(([k, v]) => ['mv_' + k, v])) });
  await page.click('#lv_live');
  await page.waitForTimeout(LIVE_SEC * 1000);
  log(`range ${await fixRange()}`);
  await liveRun('__heroPaint');
  log(`live DNA-PAINT frames (${poses.length})`);
  const mode = await page.$eval('select[data-pick="mode"]', e => e.value);
  await page.selectOption('select[data-pick="mode"]', { label: 'WideField' });
  await autoRange(true);
  await page.waitForTimeout(LIVE_SEC * 1000);
  log(`range ${await fixRange()}`);
  await liveRun('__heroWf');
  log(`live widefield frames (${poses.length})`);
  await page.click('#lv_live');
  await autoRange(true);
  await page.selectOption('select[data-pick="mode"]', mode);


  // The composite, drawn on a canvas in the page: frame i at scale s (1 = the screenshot's size); the JPEG of frame 0,
  // the GIF of all of them (web/encode/gif.js: one palette, ordered dithering, only the changed rectangle per frame).
  const out = await page.evaluate(async ([world, boxes, dpr, LOOP]) => {
    const W = await new Promise((ok, no) => { const i = new Image(); i.onload = () => ok(i); i.onerror = no; i.src = 'data:image/png;base64,' + world; });
    const compose = (i, s) => {
      const F = __heroWf[i], P = __heroPaint[i], pts = boxes[i].map(q => [q[0] * s, q[1] * s]);
      const c = document.createElement('canvas'); c.width = Math.round(W.width * s); c.height = Math.round(W.height * s);
      const g = c.getContext('2d'), k = W.width / 1588 * s;   // the layout below is in px of a 1588 px wide image
      g.imageSmoothingQuality = 'high';
      g.drawImage(W, 0, 0, c.width, c.height);
      // the camera box, as the viewer draws it (drawPlannedFov): dashed, with "camera" along its far edge
      const d = dpr * s;
      g.strokeStyle = 'rgba(255,255,255,0.9)'; g.lineWidth = Math.max(1, d); g.setLineDash([4 * d, 3 * d]);
      g.beginPath(); pts.forEach(([x, y], j) => (j ? g.lineTo(x, y) : g.moveTo(x, y))); g.closePath(); g.stroke(); g.setLineDash([]);
      let e = 0;
      for (let j = 1; j < 4; j++) if (pts[j][1] + pts[(j + 1) % 4][1] < pts[e][1] + pts[(e + 1) % 4][1]) e = j;
      let a = pts[e], b = pts[(e + 1) % 4];
      if (b[0] < a[0]) [a, b] = [b, a];
      g.fillStyle = 'rgba(255,255,255,0.9)'; g.font = `${11 * d}px system-ui, "Segoe UI", sans-serif`; g.textAlign = 'left'; g.textBaseline = 'bottom';
      g.save(); g.translate(a[0], a[1]); g.rotate(Math.atan2(b[1] - a[1], b[0] - a[0])); g.fillText('camera', 2 * d, -3 * d); g.restore();
      const S = Math.round(418 * k), GAP = Math.round(24 * k), YI = Math.round(812 * k), X0 = c.width - 1 - S - GAP - S;
      const insets = [[F, 'Widefield'], [P, 'SMLM (DNA-PAINT)']].map(([im, label], j) => {
        const x = X0 + j * (S + GAP);
        g.drawImage(im, x, YI, S, S);
        g.strokeStyle = 'rgb(235,238,245)'; g.lineWidth = 2 * k; g.strokeRect(x - 1, YI - 1, S + 1, S + 1);
        g.font = `600 ${Math.round(26 * k)}px "Segoe UI", system-ui, sans-serif`; g.textBaseline = 'top';
        const tw = g.measureText(label).width, th = 26 * k;
        g.fillStyle = 'rgba(0,0,0,0.67)'; g.beginPath(); g.roundRect(x + 4 * k, YI + 4 * k, tw + 16 * k, th + 14 * k, 6 * k); g.fill();
        g.fillStyle = 'rgb(235,238,245)'; g.fillText(label, x + 12 * k, YI + 10 * k);
        return [x, YI, x + S, YI + S];
      });
      // the box's near edge (the two lowest corners on screen) to the insets' outer top corners
      const [nl, nr] = pts.slice().sort((p, q) => q[1] - p[1]).slice(0, 2).sort((p, q) => p[0] - q[0]);
      g.strokeStyle = 'rgba(235,238,245,0.67)'; g.lineWidth = 2 * k; g.setLineDash([12 * k, 8 * k]);
      g.beginPath(); g.moveTo(...nl); g.lineTo(insets[0][0], insets[0][1]); g.moveTo(...nr); g.lineTo(insets[1][2], insets[1][1]); g.stroke();
      return c;
    };
    const jpg = compose(0, 1).toDataURL('image/jpeg', 0.9).split(',')[1];
    const s = LOOP.gifWidth / W.width, frames = boxes.map((_, i) => {
      const c = compose(i, s);
      return { w: c.width, h: c.height, px: c.getContext('2d').getImageData(0, 0, c.width, c.height).data };
    });
    const { w, h } = frames[0];
    const enc = IscGif.encoder({ width: w, height: h, fps: LOOP.fps, dither: 'bayer4' });
    enc.setPalette(IscGif.buildPalette(frames.filter((_, i) => i % 4 === 0).map(f => f.px), [[0, 0, 0], [255, 255, 255], [235, 238, 245]]));
    for (const f of frames) enc.addFrame(f.px);
    const bytes = enc.finish();
    let bin = '';
    for (let i = 0; i < bytes.length; i += 0x8000) bin += String.fromCharCode.apply(null, bytes.subarray(i, i + 0x8000));
    return { jpg, gif: btoa(bin), w, h };
  }, [world, info.boxes, info.dpr, LOOP]);
  fs.mkdirSync(OUT, { recursive: true });
  fs.writeFileSync(path.join(OUT, 'overview_3d.jpg'), Buffer.from(out.jpg, 'base64'));
  const gif = Buffer.from(out.gif, 'base64');
  fs.writeFileSync(path.join(OUT, 'overview_3d.gif'), gif);
  log(`wrote ${path.join(OUT, 'overview_3d.jpg')} and overview_3d.gif (${out.w} x ${out.h}, ${LOOP.frames} frames at ${LOOP.fps} fps, ${(gif.length / 1e6).toFixed(1)} MB)`);
} finally {
  await browser.close();
  server.close();
}
