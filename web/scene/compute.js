// SPDX-License-Identifier: BSD-3-Clause
// Data-layer computations (issue 11), DOM-free: the thresholded WideField surface (Gaussian smoothing, Otsu's
// threshold, naive surface nets: one vertex per cell the surface crosses, at the mean of its edge crossings, and one
// quad per crossed grid edge -- no case tables, closed wherever the field is closed) and the SMLM localizations of a
// multi-plane acquisition (each blink, per camera frame it overlaps, within the capture range of a focus position:
// a localization displaced by its precision, which follows the photons and the defocus). Pure functions of their
// inputs (the noise is a counter-based hash of the blink and frame), so any worker computes the same.
// iscComputeDefine() returns the API (globalThis.IscCompute on load); its source is also the compute worker's
// (web/index.html), so it works from file:// as well. tests/web/scene_compute_check.mjs checks it in Node.
'use strict';
function iscComputeDefine() {
// ---- smoothing and threshold ----
// Separable Gaussian, sigma 1 voxel (radius 2), along x, y and z; edges clamped. f: Float32Array nx*ny*nz.
function smooth3d(f, nx, ny, nz) {
  const K = [0.054488685, 0.244201342, 0.402619947, 0.244201342, 0.054488685];
  let a = f, b = new Float32Array(f.length);
  const pass = (n, stride, count, outerStride, inner) => {
    for (let o = 0; o < count; o++) {
      const base = Math.floor(o / inner) * outerStride + (o % inner);
      for (let i = 0; i < n; i++) {
        let s = 0;
        for (let k = -2; k <= 2; k++) { const j = Math.min(n - 1, Math.max(0, i + k)); s += K[k + 2] * a[base + j * stride]; }
        b[base + i * stride] = s;
      }
    }
    const t = a === f ? new Float32Array(f.length) : a; a = b; b = t;
  };
  pass(nx, 1, ny * nz, nx, 1);                 // x: lines start at (y, z) * nx
  pass(ny, nx, nx * nz, nx * ny, nx);          // y
  if (nz > 2) pass(nz, nx * ny, nx * ny, nx * ny, nx * ny);   // z
  return a;
}
// Otsu's threshold over a 256-bin histogram of the values in [lo, hi] (mask: only where mask[i] != 0).
function otsu(f, lo, hi, mask, maskStride) {
  const h = new Float64Array(256), sc = 255 / Math.max(1e-12, hi - lo);
  let n = 0;
  for (let i = 0; i < f.length; i++) {
    if (mask && !mask[maskStride ? i % maskStride : i]) continue;
    h[Math.min(255, Math.max(0, Math.floor((f[i] - lo) * sc)))]++; n++;
  }
  let sum = 0;
  for (let i = 0; i < 256; i++) sum += i * h[i];
  let wB = 0, sB = 0, best = 0, bi = 128;
  for (let t = 0; t < 256; t++) {
    wB += h[t]; if (!wB) continue;
    const wF = n - wB; if (!wF) break;
    sB += t * h[t];
    const mB = sB / wB, mF = (sum - sB) / wF, v = wB * wF * (mB - mF) * (mB - mF);
    if (v > best) { best = v; bi = t; }
  }
  return lo + (bi + 0.5) / sc;
}

// ---- naive surface nets ----
// f: values on an nx x ny x nz grid (x fastest), level: the surface; the grid is padded with `pad` (below the level)
// so the surface closes at its border. Voxel (i, j, k) sits at (i + 0.5) sx, (j + 0.5) sy, z0 + k sz.
// Returns {pos: Float32Array (x, y, z), nrm: Float32Array (unit normal, pointing to lower values), idx: Uint32Array}.
function surfaceNets(f, nx, ny, nz, level, sx, sy, sz, z0, pad, maxTris) {
  const NX = nx + 2, NY = ny + 2, NZ = nz + 2, at = (i, j, k) => (i < 1 || j < 1 || k < 1 || i > nx || j > ny || k > nz) ? pad : f[(i - 1) + nx * ((j - 1) + ny * (k - 1))];
  // cells (cx, cy, cz) in [0, NX-1) x ...: corners (cx..cx+1, ...) of the padded grid
  const CX = NX - 1, CY = NY - 1, CZ = NZ - 1, vid = new Int32Array(CX * CY * CZ).fill(-1);
  const pos = [], nrm = [], idx = [];
  const corner = new Float64Array(8);
  for (let cz = 0; cz < CZ; cz++) for (let cy = 0; cy < CY; cy++) for (let cx = 0; cx < CX; cx++) {
    let mask = 0;
    for (let c = 0; c < 8; c++) {
      const v = at(cx + (c & 1), cy + ((c >> 1) & 1), cz + (c >> 2));
      corner[c] = v - level;
      if (corner[c] > 0) mask |= 1 << c;
    }
    if (mask === 0 || mask === 255) continue;
    let px = 0, py = 0, pz = 0, ne = 0;
    for (let e = 0; e < 12; e++) {
      // the 12 edges: along x (pairs differing in bit 0), y (bit 1), z (bit 2)
      const axis = e >> 2, r = e & 3, a = axis === 0 ? (r << 1) : axis === 1 ? ((r & 1) | ((r & 2) << 1)) : r, b = a | (1 << axis);
      const va = corner[a], vb = corner[b];
      if ((va > 0) === (vb > 0)) continue;
      const t = va / (va - vb);
      px += (a & 1) + (axis === 0 ? t : 0); py += ((a >> 1) & 1) + (axis === 1 ? t : 0); pz += (a >> 2) + (axis === 2 ? t : 0);
      ne++;
    }
    px /= ne; py /= ne; pz /= ne;
    // gradient of the trilinear field at the cell centre (finite differences of the corners)
    const gx = (corner[1] + corner[3] + corner[5] + corner[7] - corner[0] - corner[2] - corner[4] - corner[6]) / sx;
    const gy = (corner[2] + corner[3] + corner[6] + corner[7] - corner[0] - corner[1] - corner[4] - corner[5]) / sy;
    const gz = (corner[4] + corner[5] + corner[6] + corner[7] - corner[0] - corner[1] - corner[2] - corner[3]) / sz;
    const gl = Math.hypot(gx, gy, gz) || 1;
    vid[cx + CX * (cy + CY * cz)] = pos.length / 3;
    // padded index p -> voxel p - 1, centred at (p - 1 + 0.5) s
    pos.push((cx + px - 0.5) * sx, (cy + py - 0.5) * sy, z0 + (cz + pz - 1) * sz);
    nrm.push(-gx / gl, -gy / gl, -gz / gl);
  }
  // quads: a grid edge from point (i, j, k) along an axis whose ends straddle the level joins the 4 cells around it
  const cell = (x, y, z) => (x < 0 || y < 0 || z < 0 || x >= CX || y >= CY || z >= CZ) ? -1 : vid[x + CX * (y + CY * z)];
  for (let k = 0; k < NZ; k++) for (let j = 0; j < NY; j++) for (let i = 0; i < NX; i++) {
    const v0 = at(i, j, k) > level;
    for (let axis = 0; axis < 3; axis++) {
      const i2 = i + (axis === 0), j2 = j + (axis === 1), k2 = k + (axis === 2);
      if (i2 >= NX || j2 >= NY || k2 >= NZ) continue;
      if (v0 === (at(i2, j2, k2) > level)) continue;
      // the four cells sharing the edge: offsets in the two other axes, cells indexed by their lower corner
      let q;
      if (axis === 0) q = [cell(i, j - 1, k - 1), cell(i, j, k - 1), cell(i, j, k), cell(i, j - 1, k)];
      else if (axis === 1) q = [cell(i - 1, j, k - 1), cell(i - 1, j, k), cell(i, j, k), cell(i, j, k - 1)];
      else q = [cell(i - 1, j - 1, k), cell(i, j - 1, k), cell(i, j, k), cell(i - 1, j, k)];
      if (q.some(v => v < 0)) continue;
      if (v0) idx.push(q[0], q[1], q[2], q[0], q[2], q[3]); else idx.push(q[0], q[2], q[1], q[0], q[3], q[2]);
      if (maxTris && idx.length / 3 > maxTris) return { pos: new Float32Array(pos), nrm: new Float32Array(nrm), idx: new Uint32Array(idx), truncated: true };
    }
  }
  return { pos: new Float32Array(pos), nrm: new Float32Array(nrm), idx: new Uint32Array(idx), truncated: false };
}
// The thresholded surface of a stack: o = {data (Uint16, nz planes of w*h, frames per plane F: plane p = frame p*F),
// w, h, nz, F, px, dz, z0, lo, hi, level (x Otsu), mask (Uint8 w*h or null), down (xy step, 1 or 2)}.
function isoFromStack(o) {
  const down = Math.max(1, o.down || 1), nx = Math.floor(o.w / down), ny = Math.floor(o.h / down), nz = o.nz, P = o.w * o.h;
  const f = new Float32Array(nx * ny * nz), valid = new Uint8Array(nx * ny);
  for (let k = 0; k < nz; k++) for (let j = 0; j < ny; j++) for (let i = 0; i < nx; i++) {
    let s = 0, n = 0;
    for (let b = 0; b < down; b++) for (let a = 0; a < down; a++) {
      const x = i * down + a, y = j * down + b;
      if (o.mask && !o.mask[y * o.w + x]) continue;
      s += o.data[k * o.F * P + y * o.w + x]; n++;
    }
    f[i + nx * (j + ny * k)] = n ? s / n - o.lo : 0;
    if (n) valid[i + nx * j] = 1;
  }
  const g = smooth3d(f, nx, ny, nz);
  let hi = 0;
  for (let i = 0; i < g.length; i++) if (g[i] > hi) hi = g[i];
  const th = otsu(g, 0, hi, valid, nx * ny) * (o.level || 1);   // the threshold from the cell's voxels only
  const m = surfaceNets(g, nx, ny, nz, th, o.px * down, o.px * down, o.dz, o.z0, -1, o.maxTris || 3e6);
  m.threshold = th + o.lo; m.down = down;
  return m;
}

// ---- localizations ----
// pcg4d (Jarzynski & Olano 2020), uint32 lanes
function pcg4d(a, b, c, d) {
  a = (Math.imul(a, 1664525) + 1013904223) >>> 0; b = (Math.imul(b, 1664525) + 1013904223) >>> 0;
  c = (Math.imul(c, 1664525) + 1013904223) >>> 0; d = (Math.imul(d, 1664525) + 1013904223) >>> 0;
  a = (a + Math.imul(b, d)) >>> 0; b = (b + Math.imul(c, a)) >>> 0; c = (c + Math.imul(a, b)) >>> 0; d = (d + Math.imul(b, c)) >>> 0;
  a = (a ^ (a >>> 16)) >>> 0; b = (b ^ (b >>> 16)) >>> 0; c = (c ^ (c >>> 16)) >>> 0; d = (d ^ (d >>> 16)) >>> 0;
  a = (a + Math.imul(b, d)) >>> 0; b = (b + Math.imul(c, a)) >>> 0; c = (c + Math.imul(a, b)) >>> 0; d = (d + Math.imul(b, c)) >>> 0;
  return [a, b, c, d];
}
// events: Float64Array stride 7 (x, y, z, tOn, tOff, brightness, id) in world um / s. o: {zs (focus positions),
// t0s (each plane's start time), N (frames per plane), exp (s), capture (um, half range), pps (photons/s ON), qe,
// sigma0 (um, in focus), zR (um), kz (sigma z / sigma xy), factor, minPhotons, seed, ox, oy (origin), cap (max)}.
// Returns {locs: Float32Array stride 8: x - ox, y - oy, z, sigmaXY, sigmaZ, photons, frame (global), plane; n}.
function emulateLocs(ev, o) {
  const out = [], E = 7, TAU = 2 * Math.PI;
  for (let p = 0; p < o.zs.length; p++) {
    const zp = o.zs[p], T0 = o.t0s[p], T1 = T0 + o.N * o.exp;
    for (let e = 0; e < ev.length; e += E) {
      const x = ev[e], y = ev[e + 1], z = ev[e + 2], tOn = ev[e + 3], tOff = ev[e + 4], br = ev[e + 5], id = ev[e + 6] >>> 0;
      const dz = z - zp;
      if (Math.abs(dz) > o.capture || tOff <= T0 || tOn >= T1) continue;
      const sPsf = o.sigma0 * Math.sqrt(1 + (dz / o.zR) * (dz / o.zR));
      const f0 = Math.max(0, Math.floor((tOn - T0) / o.exp)), f1 = Math.min(o.N - 1, Math.floor((tOff - T0) / o.exp));
      for (let f = f0; f <= f1; f++) {
        const fs = T0 + f * o.exp, ov = Math.min(tOff, fs + o.exp) - Math.max(tOn, fs);
        if (ov <= 0) continue;
        const ph = o.pps * ov * br * o.qe;
        if (ph < o.minPhotons) continue;
        const sxy = o.factor * sPsf / Math.sqrt(ph), sz = o.kz * sxy, frame = p * o.N + f;
        const r = pcg4d(id, frame >>> 0, o.seed >>> 0, (Math.floor(tOn * 1e6) >>> 0) ^ p);
        const u1 = (r[0] + 1) / 4294967297, u2 = r[1] / 4294967296, u3 = (r[2] + 1) / 4294967297, u4 = r[3] / 4294967296;
        const m1 = Math.sqrt(-2 * Math.log(u1)), m2 = Math.sqrt(-2 * Math.log(u3));
        out.push(x - o.ox + sxy * m1 * Math.cos(TAU * u2), y - o.oy + sxy * m1 * Math.sin(TAU * u2), z + sz * m2 * Math.cos(TAU * u4),
          sxy, sz, ph, frame, p);
        if (o.cap && out.length >= o.cap * 8) return { locs: new Float32Array(out), n: out.length / 8, capped: true };
      }
    }
  }
  return { locs: new Float32Array(out), n: out.length / 8, capped: false };
}
return { smooth3d, otsu, surfaceNets, isoFromStack, pcg4d, emulateLocs };
}
globalThis.IscCompute = iscComputeDefine();
