#!/usr/bin/env node
// SPDX-License-Identifier: BSD-3-Clause
// web/scene/compute.js in Node: surface nets on a sphere (area and enclosed volume within 2 %, closed: every edge
// shared by two triangles, normals outward), the same for a sphere cut by the grid's border (padding closes it),
// Otsu on a bimodal mix, and the localization emulation (capture range, frame windows, the empirical spread = the
// precision model, determinism), and the WideField segmentation (in-focus texture, filled from the coverslip).
//   node tests/web/scene_compute_check.mjs
import fs from 'node:fs';
import path from 'node:path';
import vm from 'node:vm';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const ctx = vm.createContext({ Math, Object, Array, Float32Array, Float64Array, Int32Array, Uint32Array, Uint8Array, Uint16Array });
ctx.globalThis = ctx;
vm.runInContext(fs.readFileSync(path.join(ROOT, 'web/scene/compute.js'), 'utf8'), ctx, { filename: 'compute.js' });
const C = ctx.IscCompute;
let fail = 0;
const check = (ok, what) => { if (!ok) fail++; console.log(`${ok ? 'ok  ' : 'FAIL'}  ${what}`); };

function meshStats(m) {
  let area = 0, vol = 0, outward = 0, nt = m.idx.length / 3;
  const edges = new Map();
  for (let t = 0; t < m.idx.length; t += 3) {
    const [a, b, c] = [m.idx[t], m.idx[t + 1], m.idx[t + 2]], P = i => [m.pos[3 * i], m.pos[3 * i + 1], m.pos[3 * i + 2]];
    const A = P(a), B = P(b), Cc = P(c), u = B.map((v, i) => v - A[i]), w = Cc.map((v, i) => v - A[i]);
    const n = [u[1] * w[2] - u[2] * w[1], u[2] * w[0] - u[0] * w[2], u[0] * w[1] - u[1] * w[0]];
    area += Math.hypot(...n) / 2;
    vol += (A[0] * (B[1] * Cc[2] - B[2] * Cc[1]) - A[1] * (B[0] * Cc[2] - B[2] * Cc[0]) + A[2] * (B[0] * Cc[1] - B[1] * Cc[0])) / 6;
    for (const [x, y] of [[a, b], [b, c], [c, a]]) { const k = x < y ? x + ',' + y : y + ',' + x; edges.set(k, (edges.get(k) || 0) + 1); }
  }
  let closed = true;
  for (const n of edges.values()) if (n !== 2) { closed = false; break; }
  return { area, vol: Math.abs(vol), nt, closed };
}
// 1. a sphere, radius 12 voxel units in an anisotropic grid (z step 1.5)
{
  const nx = 32, ny = 32, nz = 22, sx = 1, sy = 1, sz = 1.5, R = 12, cx = 16, cy = 16;
  const f = new Float32Array(nx * ny * nz);
  for (let k = 0; k < nz; k++) for (let j = 0; j < ny; j++) for (let i = 0; i < nx; i++) {
    const x = (i + 0.5) * sx - cx, y = (j + 0.5) * sy - cy, z = k * sz - 15.75;
    f[i + nx * (j + ny * k)] = R - Math.hypot(x, y, z);
  }
  const m = C.surfaceNets(f, nx, ny, nz, 0, sx, sy, sz, 0, -100);
  const s = meshStats(m), A = 4 * Math.PI * R * R, V = 4 / 3 * Math.PI * R ** 3;
  check(s.closed && Math.abs(s.area - A) / A < 0.02 && Math.abs(s.vol - V) / V < 0.02,
    `sphere: closed, area ${(100 * (s.area / A - 1)).toFixed(2)} %, volume ${(100 * (s.vol / V - 1)).toFixed(2)} % (${s.nt} triangles)`);
  // normals point away from the centre (to lower values)
  let out = 0;
  for (let v = 0; v < m.pos.length; v += 3) { const d = [m.pos[v] - cx, m.pos[v + 1] - cy, m.pos[v + 2] - 15.75]; if (d[0] * m.nrm[v] + d[1] * m.nrm[v + 1] + d[2] * m.nrm[v + 2] > 0) out++; }
  check(out === m.pos.length / 3, 'sphere: every normal points outward');
}
// 2. a sphere cut by the grid's border closes against the padding
{
  const n = 16, f = new Float32Array(n * n * n);
  for (let k = 0; k < n; k++) for (let j = 0; j < n; j++) for (let i = 0; i < n; i++) f[i + n * (j + n * k)] = 9 - Math.hypot(i + 0.5, j + 0.5 - 8, k - 8);
  const s = meshStats(C.surfaceNets(f, n, n, n, 0, 1, 1, 1, 0, -100));
  check(s.closed && s.nt > 100, 'cut sphere: closed by the padding');
}
// 3. Otsu between two modes
{
  const v = new Float32Array(20000);
  let r = 1;
  const rnd = () => ((r = (Math.imul(r, 1103515245) + 12345) >>> 0) / 4294967296);
  for (let i = 0; i < v.length; i++) { const g = Math.sqrt(-2 * Math.log(rnd() + 1e-12)) * Math.cos(2 * Math.PI * rnd()); v[i] = i < 14000 ? 10 + 2 * g : 40 + 3 * g; }
  const t = C.otsu(v, 0, 60, null);
  check(t > 15 && t < 32, `Otsu: ${t.toFixed(2)} in the gap between the modes 10 and 40`);
}
// 4. localizations: capture range, frame windows, spread = model, deterministic
{
  const ev = [];
  for (let i = 0; i < 4000; i++) ev.push(10 + (i % 50) * 0.1, 20 + Math.floor(i / 50) * 0.1, (i % 7) * 0.2, 0.01 + (i % 13) * 0.07, 0.01 + (i % 13) * 0.07 + 0.03, 1, 1000 + i);
  const E = new Float64Array(ev);
  const o = { zs: [0, 0.6], t0s: [0, 0], N: 20, exp: 0.05, capture: 0.4, pps: 20000, qe: 1, sigma0: 0.1, zR: 0.5, kz: 2.5, factor: 1, minPhotons: 50, seed: 7, ox: 10, oy: 20 };
  const a = C.emulateLocs(E, o), b = C.emulateLocs(E, o);
  let inRange = true, inFrames = true;
  for (let i = 0; i < a.n; i++) {
    const p = a.locs[8 * i + 7], f = a.locs[8 * i + 6], ze = ev[7 * 0 + 2];
    if (f < p * o.N || f >= (p + 1) * o.N) inFrames = false;
  }
  // every localization comes from a blink within the capture range of its plane: compare with the noise-free z
  const zOf = new Map(); for (let e = 0; e < ev.length; e += 7) zOf.set(Math.round((ev[e] - 10) * 10) + ',' + Math.round((ev[e + 1] - 20) * 10), ev[e + 2]);
  let spread = 0, model = 0;
  for (let i = 0; i < a.n; i++) {
    const L = a.locs.subarray(8 * i, 8 * i + 8), key = Math.round(L[0] * 10) + ',' + Math.round(L[1] * 10), z = zOf.get(key), zp = o.zs[L[7]];
    if (z === undefined || Math.abs(z - zp) > o.capture + 1e-6) inRange = false;
    const dx = L[0] - Math.round(L[0] * 10) / 10; spread += dx * dx; model += L[3] * L[3];
  }
  check(a.n > 1000 && inRange && inFrames, `localizations: ${a.n}, all within the capture range and their plane's frames`);
  const ratio = Math.sqrt(spread / model);
  check(Math.abs(ratio - 1) < 0.05, `localizations: empirical x spread / model sigma = ${ratio.toFixed(3)}`);
  check(Buffer.from(a.locs.buffer).equals(Buffer.from(b.locs.buffer)), 'localizations: deterministic');
}
// 5. the WideField segmentation: a cell 1.6 um tall in its centre, 0.4 um at its rim; its in-focus planes are textured
// (spots, fading over 0.3 um above the cell), out of focus only their mean shows (as for a thin, wide cell), so the
// intensity alone is the same in every plane. The heights follow the texture; nothing outside the footprint.
{
  const W = 96, nz = 16, px = 0.1, dz = 0.2, P = W * W, data = new Uint16Array(P * nz), mask = new Uint8Array(P);
  let r = 3;
  const rnd = () => ((r = (Math.imul(r, 1103515245) + 12345) >>> 0) / 4294967296);
  const spots = new Float32Array(P);
  for (let p = 0; p < P; p++) spots[p] = rnd() < 0.08 ? 60 : 0;
  const tex = C.blurXY(spots, W, W, 1, 1);
  let mean = 0; for (let p = 0; p < P; p++) mean += tex[p] / P;
  const R = p => Math.hypot((p % W + 0.5) * px - 4.8, (Math.floor(p / W) + 0.5) * px - 4.8), hTrue = p => (R(p) < 2 ? 1.6 : 0.4);
  for (let p = 0; p < P; p++) mask[p] = R(p) < 4.2 ? 1 : 0;
  for (let k = 0; k < nz; k++) for (let p = 0; p < P; p++)
    data[k * P + p] = 120 + mean + (mask[p] ? (tex[p] - mean) * Math.exp(-0.5 * (Math.max(0, k * dz - hTrue(p)) / 0.3) ** 2) : 0);
  const m = C.isoFromStack({ data, w: W, h: W, nz, F: 1, px, dz, z0: 0, lo: 100, level: 1, mask, down: 1 });
  const med = sel => { const v = []; for (let p = 0; p < P; p++) if (sel(p)) v.push(m.heights[p]); v.sort((a, b) => a - b); return v[v.length >> 1]; };
  const hi = med(p => R(p) < 1), lo = med(p => R(p) > 3 && R(p) < 3.8);
  let out = 0; for (let p = 0; p < P; p++) if (!mask[p] && m.heights[p] > 0) out++;
  check(Math.abs(hi - 1.6) < 0.35 && Math.abs(lo - 0.4) < 0.3 && out === 0 && m.idx.length > 0,
    `WideField segmentation: centre ${hi.toFixed(2)} um (1.6), rim ${lo.toFixed(2)} um (0.4), ${out} columns outside the footprint`);
}
process.exit(fail ? 1 : 0);
