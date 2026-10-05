// SPDX-License-Identifier: BSD-3-Clause
// Data-layer computations (issue 11), DOM-free: the segmented WideField surface (in-focus texture, Otsu's
// threshold, filled from the coverslip; naive surface nets: one vertex per cell the surface crosses, at the mean of its edge crossings, and one
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
// Separable Gaussian in x and y only, per plane (sigma in voxels); edges clamped. Wide kernels (sigma > 3) are three
// box filters of the same variance (running sums), narrow ones the direct sum over 3 sigma.
function blurXY(f, nx, ny, nz, sig) {
  const bh = sig > 3 ? Math.max(1, Math.round((Math.sqrt(4 * sig * sig + 1) - 1) / 2)) : 0, bw = 2 * bh + 1;
  const r = bh ? 3 * bh + 1 : Math.max(1, Math.ceil(3 * sig)), K = new Float64Array(2 * r + 1);
  let s = 0;
  for (let i = -r; i <= r; i++) s += K[i + r] = Math.exp(-i * i / (2 * sig * sig));
  for (let i = 0; i < K.length; i++) K[i] /= s;
  const P = nx * ny, out = new Float32Array(f.length), n = Math.max(nx, ny), L = n + 2 * r;
  const line = new Float32Array(L + 1), box = new Float32Array(L), res = new Float32Array(n);
  // one line (length len, stride st from base) of src into dst, through a clamped copy padded by r: line[i + r] = src[i]
  const pass = (src, dst, base, len, st) => {
    for (let i = -r; i < len + r; i++) line[i + r] = src[base + Math.min(len - 1, Math.max(0, i)) * st];
    if (bh) {
      for (let it = 1; it <= 3; it++) {   // after pass it, line is valid on [it bh, len + 2r - it bh)
        const lo = it * bh, hi = len + 2 * r - lo;
        let acc = 0;
        for (let q = lo - bh; q <= lo + bh; q++) acc += line[q];
        for (let j = lo; j < hi; j++) { box[j] = acc / bw; acc += line[j + bh + 1] - line[j - bh]; }
        for (let j = lo; j < hi; j++) line[j] = box[j];
      }
      for (let i = 0; i < len; i++) res[i] = line[i + r];
    } else for (let i = 0; i < len; i++) { let v = 0; for (let q = 0; q <= 2 * r; q++) v += K[q] * line[i + q]; res[i] = v; }
    for (let i = 0; i < len; i++) dst[base + i * st] = res[i];
  };
  // y lines run along rows of a transposed copy (contiguous)
  const t = new Float32Array(P), u = new Float32Array(P);
  for (let k = 0; k < nz; k++) {
    const o = k * P, src = f.subarray(o, o + P), dst = out.subarray(o, o + P);
    for (let j = 0; j < ny; j++) pass(src, t, j * nx, nx, 1);
    for (let j = 0; j < ny; j++) for (let i = 0; i < nx; i++) u[i * ny + j] = t[j * nx + i];
    for (let i = 0; i < nx; i++) pass(u, t, i * ny, ny, 1);
    for (let i = 0; i < nx; i++) for (let j = 0; j < ny; j++) dst[j * nx + i] = t[i * ny + j];
  }
  return out;
}
// The segmented cell of a WideField stack. The out-of-focus light of a thin, wide cell is nearly the same in every
// plane (a uniform sheet stays uniform out of focus), so a threshold on the intensity finds a column through the whole
// stack. What changes with focus is the fine structure: each plane is band-passed (DoG 0.2 / 0.5 um), its local
// energy (0.6 um) divided by the local mean (shot noise grows with it) is the in-focus texture; Otsu's level over the
// cell's voxels (x 0.65 x the layer's level) marks the in-focus voxels. An adherent cell is filled from the coverslip:
// per column the height is the highest in-focus voxel (interpolated) less 0.2 um (half the in-focus depth), smoothed
// over 1 um within the footprint, and the surface is height - z = 0.
// o = {data (Uint16, nz planes of w*h, frames per plane F: plane p = frame p*F), w, h, nz, F, px, dz, z0, lo, level
// (x the default), mask (Uint8 w*h or null), down (xy step, 1 or 2)}. Returns the surface nets mesh plus heights (um
// above z0, 0 = no cell) on its nx x ny grid.
function isoFromStack(o) {
  const down = Math.max(1, o.down || 1), W = o.w, H = o.h, nz = o.nz, P = W * H, px = o.px;
  const f = new Float32Array(P * nz);
  let sum = 0, cnt = 0;
  for (let k = 0; k < nz; k++) for (let p = 0; p < P; p++) {
    const v = Math.max(0, o.data[k * o.F * P + p] - o.lo);
    f[k * P + p] = v;
    if (!o.mask || o.mask[p]) { sum += v; cnt++; }
  }
  const a = blurXY(f, W, H, nz, 0.2 / px), b = blurXY(f, W, H, nz, 0.5 / px);
  for (let i = 0; i < f.length; i++) { const d = a[i] - b[i]; a[i] = d * d; }
  const e = blurXY(a, W, H, nz, 0.6 / px), m = blurXY(f, W, H, nz, 0.6 / px), floor = Math.max(1, 0.2 * sum / Math.max(1, cnt));
  let hi = 0;
  for (let i = 0; i < e.length; i++) { e[i] /= m[i] + floor; if ((!o.mask || o.mask[i % P]) && e[i] > hi) hi = e[i]; }
  const th = otsu(e, 0, hi, o.mask, P) * 0.65 * (o.level || 1);
  // the height of each column (um above z0), then smoothed within the footprint (normalized convolution)
  const hgt = new Float32Array(P), wgt = new Float32Array(P);
  for (let p = 0; p < P; p++) {
    if (o.mask && !o.mask[p]) continue;
    wgt[p] = 1;
    let top = -1;
    for (let k = nz - 1; k >= 0; k--) if (e[k * P + p] > th) { top = k; break; }
    if (top < 0) continue;
    const v0 = e[top * P + p], v1 = top + 1 < nz ? e[(top + 1) * P + p] : 0;
    hgt[p] = Math.max(0, (top + Math.min(1, (v0 - th) / Math.max(1e-12, v0 - v1))) * o.dz - 0.2);
  }
  const hs = blurXY(hgt, W, H, 1, 1 / px), ws = blurXY(wgt, W, H, 1, 1 / px);
  // the field height - z on the (downsampled) grid; outside the footprint and where the cell is thinner than half a
  // plane it is below the level
  const nx = Math.floor(W / down), ny = Math.floor(H / down), g = new Float32Array(nx * ny * nz), heights = new Float32Array(nx * ny);
  for (let j = 0; j < ny; j++) for (let i = 0; i < nx; i++) {
    let s = 0, n = 0;
    for (let bb = 0; bb < down; bb++) for (let aa = 0; aa < down; aa++) {
      const p = (j * down + bb) * W + i * down + aa;
      if (o.mask && !o.mask[p]) continue;
      s += hs[p] / Math.max(1e-6, ws[p]); n++;
    }
    const h = n ? s / n : 0, ok = n && h > o.dz / 2;
    heights[i + nx * j] = ok ? h : 0;
    for (let k = 0; k < nz; k++) g[i + nx * (j + ny * k)] = ok ? h - k * o.dz : -1;
  }
  const mesh = surfaceNets(g, nx, ny, nz, 0, px * down, px * down, o.dz, o.z0, -1, o.maxTris || 3e6);
  mesh.threshold = th; mesh.down = down; mesh.heights = heights; mesh.nx = nx; mesh.ny = ny;
  return mesh;
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
return { smooth3d, blurXY, otsu, surfaceNets, isoFromStack, pcg4d, emulateLocs };
}
globalThis.IscCompute = iscComputeDefine();
