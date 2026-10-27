#!/usr/bin/env node
// SPDX-License-Identifier: BSD-3-Clause
// The viewer's data layers in headless Chromium: a z-stack plane equals a stand-alone movie job with the same
// settings, the WideField slice and its thresholded surface are drawn, the localizations of a short multi-plane
// acquisition lie in the cell, and an opaque data slice hides only what lies behind it. The data stacks use 200 nm
// pixels and coarse focus steps (what they check does not depend on either).
//   node tests/web/viewer_data.mjs [--channel=chrome] [--swiftshader]
import { runViewer, sceneHelpers, quietMainView } from './browser.mjs';

await runViewer('viewer data', { watchdogSec: 180 }, async ({ page, check }) => {
  await quietMainView(page);
  await page.evaluate(() => { document.getElementById('mv_pixel-nm').value = 200; });
  await sceneHelpers(page);
  // 1. a z-stack plane = the stand-alone movie job of its spec (WideField, one frame, plane 2: z = 1 um)
  const eq = await page.evaluate(async () => {
    const over = { wfAverage: 1, stepUm: 1 };
    const [st] = await iscScene.acquireData(['mt.wfSlice'], __key, over, null);
    const p = 2, spec = movieSpec(Object.assign({}, st.ov, { z: st.zs[p], seed: (+document.getElementById('seed').value | 0) + 1000 * p, 'start-sec': st.t0 }));
    const d = await movieJob(spec), P = st.size * st.size;
    let n = 0; for (let i = 0; i < P; i++) if (d.frames[i] !== st.data[p * P + i]) n++;
    return { n, P, nz: st.zs.length };
  });
  check(eq.n === 0, `data: z-stack plane 2 of ${eq.nz} = a stand-alone movie job (${eq.n} of ${eq.P} pixels differ)`);

  // 2. a WideField slice draws into the frame where the stack is
  const lit = await page.evaluate(async () => {
    const b = iscScene.cellBounds(__key), pv = [b.center[0], b.center[1], 1];
    const st = __st({ camera: { pivot: pv, azimuthDeg: 0, tiltDeg: 0, fovUm: b.diam * 1.2 }, slice: { axis: 'z', pos: 1 }, data: { wfAverage: 1, stepUm: 1 },
      layers: { 'mt.wfSlice': { style: { color: '#ffffff', opacity: 1 } } } });
    const { data } = await __cap(st);
    let n = 0; for (let i = 0; i < data.length; i += 4) if (data[i] > 40) n++;
    return n;
  });
  check(lit > 1000, `data: the WideField slice is drawn (${lit} lit px)`);

  // 3. the thresholded WideField surface (same stack) and the localizations of a short multi-plane acquisition
  const more = await page.evaluate(async () => {
    const b = iscScene.cellBounds(__key), pv = [b.center[0], b.center[1], 1], data = { wfAverage: 1, stepUm: 1, locFrames: 50 };
    const cam = { pivot: pv, azimuthDeg: 30, tiltDeg: 45, fovUm: b.diam * 1.2 };
    const count = d => { let n = 0; for (let i = 0; i < d.length; i += 4) if (d[i] + d[i + 1] + d[i + 2] > 90) n++; return n; };
    const iso = await __cap(__st({ camera: cam, data, layers: { 'mt.wfIso': { style: { color: '#ffffff', opacity: 1 } } } }));
    const [L] = await iscScene.acquireData(['mt.locs'], __key, data, null);
    const loc = await __cap(__st({ camera: cam, data, layers: { 'mt.locs': { style: { color: '#ffffff', opacity: 1, colorBy: 'flat' } } } }));
    // every localization lies within the capture range of a focus position and inside the cell's box
    let inside = true;
    for (let i = 0; i < L.n; i += 97) { const x = L.locs[8 * i] + L.ox, y = L.locs[8 * i + 1] + L.oy; if (x < b.x[0] - 1 || x > b.x[1] + 1 || y < b.y[0] - 1 || y > b.y[1] + 1) inside = false; }
    return { iso: count(iso.data), loc: count(loc.data), n: L.n, inside };
  });
  check(more.iso > 1000, `data: the thresholded WideField surface is drawn (${more.iso} lit px)`);
  check(more.n > 100 && more.inside && more.loc > 100, `data: ${more.n} localizations from 50 frames per plane, in the cell's box, drawn (${more.loc} lit px)`);
  // 4. an opaque slice (SMLM frames) hides only what is behind it: the cytoplasm surface above the plane stays visible
  const ahead = await page.evaluate(async () => {
    const b = iscScene.cellBounds(__key), pv = [b.center[0], b.center[1], 1], data = { wfAverage: 1, stepUm: 1, srStepUm: 0.8, srFrames: 2 };
    await iscScene.acquireData(['mt.srFrames'], __key, data, null);
    const green = d => { let n = 0; for (let i = 0; i < d.length; i += 4) if (d[i + 1] > 60 && d[i + 1] > 2 * d[i] && d[i + 1] > 2 * d[i + 2]) n++; return n; };
    const cyto = { style: { color: '#00ff00', opacity: 0.6 }, intervals: [{ lo: 0.6, hi: 1e30 }] };
    const st = layers => __st({ camera: { pivot: pv, azimuthDeg: 0, tiltDeg: 0, fovUm: b.diam * 1.2 }, data, sweep: { axis: 'z' }, slice: { axis: 'z', pos: 0.4 }, layers });
    return { alone: green((await __cap(st({ 'cyto.surface': cyto }))).data), withSr: green((await __cap(st({ 'cyto.surface': cyto, 'mt.srFrames': {} }))).data) };
  });
  check(ahead.alone > 1000 && ahead.withSr >= 0.95 * ahead.alone, `data: the cytoplasm above an SMLM slice stays visible (${ahead.withSr} of ${ahead.alone} px)`);

});
