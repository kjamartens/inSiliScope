#!/usr/bin/env node
// SPDX-License-Identifier: BSD-3-Clause
// The viewer's renderer in headless Chromium (web/index.html renderFrame / captureFrame): clip intervals cut exactly
// at their heights (tilt 90: lit rows only inside each band, each band in its own colour), a turned view is the
// unturned one rotated, a frame is pixel-identical when captured twice, the nuclear pores' and lamina's layers, the
// detail budget keeps N cells detailed. The data layers: viewer_data.mjs; the nanorulers: viewer_nanorulers.mjs.
//   node tests/web/viewer_scene.mjs [--channel=chrome] [--swiftshader]
import { runViewer, sceneHelpers, quietMainView } from './browser.mjs';

await runViewer('viewer scene', { watchdogSec: 150 }, async ({ page, check }) => {
  await quietMainView(page);
  await sceneHelpers(page);
  // 1. clip bands at tilt 90: lit rows only inside [z0, z1], two intervals in their own colours
  const bands = await page.evaluate(async () => {
    const b = iscScene.cellBounds(__key), pv = [b.center[0], b.center[1], 0];
    const st = __st({ camera: { pivot: pv, azimuthDeg: 0, tiltDeg: 90, fovUm: b.diam * 1.2 }, sweep: { axis: 'z' },
      layers: { 'mt.lines': { intervals: [{ lo: 0.6, hi: 1.0, style: { color: '#ff0000', opacity: 1 } }, { lo: 1.6, hi: 2.0, style: { color: '#00ff00', opacity: 1 } }] } } });
    const { data, cam } = await __cap(st);
    const W = 400, H = 300, rowOf = z => cam.H2 - (z - cam.pz) * cam.S;   // tilt 90: screen y = H/2 - (z - pz) S
    const tol = 3 + cam.S * 0.05, out = { red: 0, green: 0, redOut: 0, greenOut: 0 };
    for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) {
      const i = 4 * (y * W + x), r = data[i], g = data[i + 1];
      if (r > 120 && g < 60) { out.red++; if (y < rowOf(1.0) - tol || y > rowOf(0.6) + tol) out.redOut++; }
      if (g > 120 && r < 60) { out.green++; if (y < rowOf(2.0) - tol || y > rowOf(1.6) + tol) out.greenOut++; }
    }
    return out;
  });
  check(bands.red > 50 && bands.green > 50 && bands.redOut === 0 && bands.greenOut === 0,
    `clip: two intervals at tilt 90 lit only inside their bands (red ${bands.red} px, ${bands.redOut} outside; green ${bands.green}, ${bands.greenOut} outside)`);

  // 2. a turned view = the unturned one rotated (top-down, the cytoplasm's mask), IoU
  const iou = await page.evaluate(async () => {
    const b = iscScene.cellBounds(__key), pv = [b.center[0], b.center[1], 0], th = 37, W = 400, H = 300;
    const mk = async az => { const { data } = await __cap(__st({ camera: { pivot: pv, azimuthDeg: az, tiltDeg: 0, fovUm: b.diam * 1.3 },
      layers: { 'cyto.surface': { style: { color: '#ffffff', opacity: 1 } } } }), W, H); const m = new Uint8Array(W * H); for (let i = 0; i < W * H; i++) m[i] = data[4 * i] > 128 ? 1 : 0; return m; };
    const a = await mk(0), r = await mk(th), t = th * Math.PI / 180;
    // a world point at screen offset (u, v) unturned shows at R(-th) (u, v) turned (screen y down = turned v)
    let inter = 0, uni = 0;
    for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) {
      const u = x + 0.5 - W / 2, v = y + 0.5 - H / 2, x2 = Math.round(W / 2 + u * Math.cos(t) + v * Math.sin(t) - 0.5), y2 = Math.round(H / 2 - u * Math.sin(t) + v * Math.cos(t) - 0.5);
      const A0 = a[y * W + x], B0 = x2 >= 0 && y2 >= 0 && x2 < W && y2 < H ? r[y2 * W + x2] : 0;
      if (A0 && B0) inter++; if (A0 || B0) uni++;
    }
    return inter / Math.max(1, uni);
  });
  check(iou > 0.97, `rotation: the turned image is the unturned one rotated (IoU ${iou.toFixed(4)})`);

  // 3. the same state twice: identical pixels
  const same = await page.evaluate(async () => {
    const st = __st({ camera: { pivot: [0, 0, 1], azimuthDeg: 23, tiltDeg: 40, fovUm: 50 }, scope: 'ghosts', layers: { 'mt.lines': {}, 'cyto.surface': {}, 'nucleus.surface': {} } });
    const a = (await __cap(st)).data, b = (await __cap(st)).data;
    let n = 0; for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) n++;
    return n;
  });
  check(same === 0, `determinism: a frame captured twice is identical (${same} values differ)`);

  // 4. the nuclear pores: their layer alone lights the nucleus' pores, nothing without it; the depth look colours them
  // (and the microtubules) by height, several hues
  const pores = await page.evaluate(async () => {
    const b = iscScene.cellBounds(__key), cam = { pivot: [b.center[0], b.center[1], 0], azimuthDeg: 0, tiltDeg: 30, fovUm: 20 };
    const lit = d => { let n = 0; for (let i = 0; i < d.length; i += 4) if (d[i] + d[i + 1] + d[i + 2] > 60) n++; return n; };
    const on = (await __cap(__st({ camera: cam, layers: { 'npc.lines': { style: { color: '#ffffff', opacity: 1 } } } }))).data;
    const off = (await __cap(__st({ camera: cam, layers: {} }))).data;
    const z = (await __cap(__st({ camera: cam, theme: 'fluoZ', layers: { 'npc.lines': {}, 'mt.lines': {} } }))).data, hues = new Set();
    for (let i = 0; i < z.length; i += 4) if (z[i] + z[i + 1] + z[i + 2] > 120) hues.add((z[i] >> 5) + ',' + (z[i + 1] >> 5) + ',' + (z[i + 2] >> 5));
    return { on: lit(on), off: lit(off), hues: hues.size };
  });
  check(pores.on > 200 && pores.off === 0 && pores.hues >= 6,
    `nuclear pores: ${pores.on} px lit by their layer alone, ${pores.off} without; the depth look: ${pores.hues} colour bins`);

  // 5. the nuclear lamina (View > Nuclear lamina, off by default): its layer alone lights the nuclear envelope (drawn as
  // a surface, 2026-10-28: the default 2000 points per um^2 one by one took the view to a few frames per second), nothing
  // without it; the View toggle is off on a fresh page
  const lamina = await page.evaluate(async () => {
    const toggle = els.showLamin.checked;
    const b = iscScene.cellBounds(__key), cam = { pivot: [b.center[0], b.center[1], 0], azimuthDeg: 0, tiltDeg: 30, fovUm: 20 };
    const lit = d => { let n = 0; for (let i = 0; i < d.length; i += 4) if (d[i] + d[i + 1] + d[i + 2] > 60) n++; return n; };
    const on = (await __cap(__st({ camera: cam, layers: { 'lamin.surface': { style: { color: '#ffffff', opacity: 1 } } } }))).data;
    const off = (await __cap(__st({ camera: cam, layers: {} }))).data;
    // the height scale (Lamina colours, no style colour): the dots' red channel spreads with their height; flat: one colour
    const spread = d => { const rs = []; for (let i = 0; i < d.length; i += 4) if (d[i] + d[i + 1] + d[i + 2] > 150) rs.push(d[i]);
      rs.sort((a, b) => a - b); return rs.length ? rs[Math.floor(rs.length * 0.9)] - rs[Math.floor(rs.length * 0.1)] : 0; };
    const scale = spread((await __cap(__st({ camera: cam, layers: { 'lamin.surface': { style: { color: null } } } }))).data);
    const flat = spread((await __cap(__st({ camera: cam, layers: { 'lamin.surface': {} } }))).data);
    const row = els.laminMap.closest('.opt');
    return { on: lit(on), off: lit(off), toggle, scale, flat,
      mapHidden: row ? row.style.display === 'none' : null };
  });
  check(lamina.on > 300 && lamina.off === 0 && !lamina.toggle && lamina.scale > 30 &&
    lamina.flat < lamina.scale / 2 && lamina.mapHidden !== false,
    `nuclear lamina: ${lamina.on} px lit by its layer alone, ${lamina.off} without; View toggle ${lamina.toggle ? 'on' : 'off'}, ` +
    `a surface; height scale spread ${lamina.scale} vs ${lamina.flat} flat; Lamina colours hidden while off (${lamina.mapHidden})`);

  // the lamina's density on the surface (the cell workers count its points near each vertex): about the set density
  // everywhere (the points are uniform: counting noise only), and a sparser lamina shows through (opacity x density)
  const dens = await page.evaluate(async () => {
    const b = iscScene.cellBounds(__key), cam = { pivot: [b.center[0], b.center[1], 0], azimuthDeg: 0, tiltDeg: 30, fovUm: 20 };
    const st = __st({ camera: cam, layers: { 'lamin.surface': { style: { color: '#ffffff', opacity: 1 } } } });
    const t0 = performance.now();
    let a = null;
    while (!(a = assets.get(__key)) || !a.entry.laminDens) {
      if (performance.now() - t0 > 30000) return null;
      await __cap(st); await new Promise(r => setTimeout(r, 100));
    }
    const d = [...a.entry.laminDens].sort((x, y) => x - y), mean = d.reduce((s, x) => s + x, 0) / d.length;
    const sum = img => { let s = 0; for (let i = 0; i < img.length; i += 4) s += img[i]; return s; };
    const full = sum((await __cap(st)).data);
    // every vertex at half the density: the surface at half its opacity (the back of the envelope then shows through
    // the front: 0.5 + 0.5 x 0.5 = 75 % as bright where they overlap, 50 % where one layer is seen)
    const keep = a.entry.laminDens;
    a.entry.laminDens = keep.map(() => 0.5); freeMeshGl(a.entry);
    const half = sum((await __cap(st)).data);
    a.entry.laminDens = keep; freeMeshGl(a.entry);
    return { ms: performance.now() - t0, mean, p5: d[Math.floor(d.length * 0.05)], p95: d[Math.floor(d.length * 0.95)], ratio: half / full };
  });
  check(dens && Math.abs(dens.mean - 1) < 0.03 && dens.p5 > 0.85 && dens.p95 < 1.15 && dens.ratio > 0.45 && dens.ratio < 0.8,
    dens ? `nuclear lamina density: mean ${dens.mean.toFixed(3)} of the set density, 5-95 % ${dens.p5.toFixed(2)}-${dens.p95.toFixed(2)} ` +
      `(after ${(dens.ms / 1000).toFixed(1)} s); at half the density the surface is ${(dens.ratio * 100).toFixed(0)} % as bright` : 'nuclear lamina density: never arrived');

  // 6. the detail budget
  const det = await page.evaluate(async () => {
    els.detailCells.value = 3; els.detailCells.dispatchEvent(new Event('input', { bubbles: true }));
    view.scale = 3; draw(); await new Promise(r => setTimeout(r, 3000)); draw();
    const d = document.getElementById('hud').dataset, t = `cells ${d.cells} (${d.onScreen} on screen, ${d.detailed} detailed)`;
    els.detailCells.value = 5; els.detailCells.dispatchEvent(new Event('input', { bubbles: true })); view.scale = 9; draw();
    return t;
  });
  check(/, 3 detailed\)/.test(det), 'detail budget: 3 detailed cells in a zoomed-out view (' + (/cells [^\n]*detailed\)/.exec(det) || [''])[0] + ')');

});
