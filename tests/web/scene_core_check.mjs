#!/usr/bin/env node
// SPDX-License-Identifier: BSD-3-Clause
// web/scene/core.js in Node (it is DOM-free): the camera reduces to the viewer's original oblique projection at
// rotation 0 (bit for bit), its rows are orthonormal, project/unproject/panDelta invert each other, a cell's
// local frame composes into one effective rotation, the clip tables pack as the shaders read them, and the detail
// set keeps its members under small pans and never changes under a turn.
//   node tests/web/scene_core_check.mjs
import fs from 'node:fs';
import path from 'node:path';
import vm from 'node:vm';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const ctx = vm.createContext({ Math, Object, Array, Map, Set, Float32Array, String, parseInt });
ctx.globalThis = ctx;
vm.runInContext(fs.readFileSync(path.join(ROOT, 'web/scene/core.js'), 'utf8'), ctx, { filename: 'scene/core.js' });
const S = ctx.IscScene;
let fail = 0;
const check = (ok, what) => { if (!ok) fail++; console.log(`${ok ? 'ok  ' : 'FAIL'}  ${what}`); };
const near = (a, b, tol = 1e-9) => Math.abs(a - b) <= tol * Math.max(1, Math.abs(a), Math.abs(b));

// 1. rotation 0, pivot height 0: the original projection, exactly
{
  const W = 1000, H = 700, scale = 14, dpr = 1.25, cx = 12.3, cy = -7.1, tilt = 35 * Math.PI / 180;
  const cam = S.makeCamera({ px: cx, py: cy, pz: 0, az: 0, tilt, S: scale * dpr, W, H });
  let exact = true;
  for (let i = 0; i < 1000; i++) {
    const x = cx + (Math.sin(i * 1.7) * 80), y = cy + Math.cos(i * 2.3) * 60, z = (i % 37) * 0.3;
    const sx = W / 2 + (x - cx) * scale * dpr;
    const sy = H / 2 + ((y - cy) * Math.cos(tilt) - z * Math.sin(tilt)) * scale * dpr;
    const [a, b] = cam.project(x, y, z);
    const d = (y - cy) * Math.sin(tilt) + z * Math.cos(tilt);
    if (!near(a, sx, 1e-12) || !near(b, sy, 1e-12) || cam.depth(x, y, z) !== d) exact = false;
  }
  check(exact, 'rotation 0 = the original oblique projection and depth');
}
// 2. orthonormal rows, project/unproject, pan
{
  let ok = true;
  for (const [az, tilt] of [[0.3, 0.2], [-2.1, 1.1], [3.0, 1.5707963], [1.0, 0]]) {
    const c = S.makeCamera({ px: 3, py: -4, pz: 1.5, az, tilt, S: 20, W: 800, H: 600 });
    const dot = (a, b) => a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    const R = [c.r0, c.r1, c.r2];
    for (let i = 0; i < 3; i++) for (let j = 0; j < 3; j++) if (!near(dot(R[i], R[j]), i === j ? 1 : 0, 1e-12)) ok = false;
    // the rows give the same screen position as project()
    const x = 7.5, y = 2.25, z = 3.1, d = [x - 3, y + 4, z - 1.5], [sx, sy] = c.project(x, y, z);
    if (!near(sx, 400 + dot(c.r0, d) * 20) || !near(sy, 300 + dot(c.r1, d) * 20) || !near(c.depth(x, y, z), dot(c.r2, d))) ok = false;
    if (tilt < 1.4) {
      const [ux, uy] = c.unproject(sx, sy, z);
      if (!near(ux, x, 1e-9) || !near(uy, y, 1e-9)) ok = false;
      // a drag of (dx, dy) moves the pivot so the point under the pointer follows it
      const [ddx, ddy] = c.panDelta(13, -7), c2 = S.makeCamera({ px: 3 + ddx, py: -4 + ddy, pz: 1.5, az, tilt, S: 20, W: 800, H: 600 });
      const [s2x, s2y] = c2.project(x, y, 1.5);
      const [s1x, s1y] = c.project(x, y, 1.5);
      if (!near(s2x - s1x, 13, 1e-9) || !near(s2y - s1y, -7, 1e-9)) ok = false;
    }
  }
  check(ok, 'rows orthonormal; project = rows; unproject and panDelta invert project');
}
// 3. a cell's local frame: packRot - az
{
  const c = S.makeCamera({ px: 1, py: 2, pz: 0.5, az: 0.7, tilt: 0.9, S: 10, W: 500, H: 400 });
  const cell = { x: 15, y: -6, packRot: 2.2 }, e = c.cellRot(cell.packRot), ok = [];
  for (const [lx, ly, h] of [[3, 4, 1], [-2, 0.5, 2.5], [0, -7, 0]]) {
    const wx = cell.x + lx * Math.cos(2.2) - ly * Math.sin(2.2), wy = cell.y + lx * Math.sin(2.2) + ly * Math.cos(2.2);
    const u = c.u(cell.x, cell.y) + lx * Math.cos(e) - ly * Math.sin(e), v = c.v(cell.x, cell.y) + lx * Math.sin(e) + ly * Math.cos(e);
    const [sx, sy] = c.project(wx, wy, h);
    ok.push(near(sx, 250 + u * 10) && near(sy, 200 + (v * c.ct - (h - 0.5) * c.st) * 10));
  }
  check(ok.every(Boolean), 'cell local frame = effective rotation packRot - az plus the turned offset');
}
// 4. visibleRect holds the turned view rectangle
{
  const c = S.makeCamera({ px: 0, py: 0, pz: 0, az: 0.6, tilt: 0.5, S: 5, W: 600, H: 400 });
  const [x0, y0, x1, y1] = c.visibleRect();
  let ok = true;
  for (const [sx, sy] of [[0, 0], [600, 0], [0, 400], [600, 400], [300, 200]]) {
    const [x, y] = c.unproject(sx, sy, 0);
    if (x < x0 - 1e-9 || x > x1 + 1e-9 || y < y0 - 1e-9 || y > y1 + 1e-9) ok = false;
  }
  check(ok, 'visibleRect holds every screen corner on the coverslip');
}
// 5. packZones / zoneSlot
{
  const st = { theme: 'fluo', layers: {
    'mt.lines': { intervals: [{ lo: -1e30, hi: 2 }, { lo: 2.5, hi: 4, style: { color: '#ff0000', opacity: 0.5 } }] },
    'cyto.surface': {} } };
  const Z = S.packZones(st), kM = S.KIND['mt.lines'], kC = S.KIND['cyto.surface'], IV = S.IV_MAX;
  const ok = Z.n[kM] === 2 && Z.n[kC] === 1 && Z.n[S.KIND['nucleus.surface']] === 0 &&
    S.zoneSlot(Z, kM, 1) === kM * IV && S.zoneSlot(Z, kM, 2.2) === -1 && S.zoneSlot(Z, kM, 3) === kM * IV + 1 &&
    Z.st[4 * (kM * IV + 1)] === 1 && Math.abs(Z.st[4 * (kM * IV + 1) + 3] - 0.5) < 1e-7 && S.zoneSlot(Z, kC, 1e20) === kC * IV;
  const V = S.packZones({ theme: 'viewer', layers: { 'cyto.surface': {} } });
  check(ok && V.st[4 * kC * IV] < 0, 'packZones: counts, intervals, slot styles; viewer surfaces keep their own colour');
  const cam = S.makeCamera({ px: 10, py: 20, pz: 0, az: 0.4, tilt: 0.3, S: 1, W: 2, H: 2 });
  const a = S.axisInView(cam, { axis: 'x' }), x = 13.5, y = 18.25, s = a[0] * cam.u(x, y) + a[1] * cam.v(x, y) + a[3];
  check(near(s, x, 1e-12) && S.axisInView(cam, null).join() === '0,0,1,0', 'axisInView: s along a world axis from turned coordinates');
}
// 6. detail set: nearest N, hysteresis on pans, unchanged under any turn
{
  const cells = [];
  for (let i = 0; i < 40; i++) cells.push({ cx: i % 8, cy: (i / 8) | 0, x: (i % 8) * 26 + Math.sin(i) * 5, y: ((i / 8) | 0) * 26 + Math.cos(i * 3) * 5 });
  const a = S.detailSet(cells, 100, 60, 5, null);
  const b = S.detailSet(cells, 101.5, 60.5, 5, a.set);
  let stable = true;
  for (const k of a.set) if (!b.set.has(k)) stable = false;
  check(a.set.size === 5 && stable, 'detail set: 5 nearest, kept under a small pan');
  check(S.detailSet(cells, 100, 60, 0, null).set.size === 40, 'detail set: 0 = every cell');
}
// 7. the registry: every structure's layers, ids structure.rep, GL kinds for the drawn ones
{
  const L = S.LAYERS;
  const ok = L.has('mt.lines') && L.has('mt.locs') && L.has('mt.wfSlice') && L.has('cell.bfSlice') && !L.has('nucleus.wfSlice') &&
    L.get('mt.lines').kind === S.KIND['mt.lines'] && L.get('mt.lines').implemented && !L.get('mt.locs').implemented;
  check(ok, 'layer registry: structure.rep ids, dye layers only where a dye population is');
}
process.exit(fail ? 1 : 0);
