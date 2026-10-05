// SPDX-License-Identifier: BSD-3-Clause
// A small animated-GIF encoder (no dependencies; globalThis.IscGif). One global palette for the whole animation
// (median cut over sample frames, with the given key colours kept exact, so nothing flickers between frames),
// optional ordered (Bayer 4x4, stable over time) or Floyd-Steinberg dithering, LZW, and only the changed rectangle of
// each frame written (a frame identical to the previous one adds its time to the previous frame's delay). Output is a
// pure function of the frames: encoding twice gives the same bytes.
//   const enc = IscGif.encoder({ width, height, fps, dither: 'bayer4' });
//   enc.setPalette(IscGif.buildPalette([rgba, rgba, ...], [[0, 0, 0], ...]));
//   for (const rgba of frames) enc.addFrame(rgba);   // Uint8(Clamped)Array, width * height * 4
//   const bytes = enc.finish();                       // Uint8Array
'use strict';
(function () {

// ---- palette: median cut over an RGB555 histogram ----
function buildPalette(samples, keys, n) {
  n = Math.min(256, n || 256);
  keys = (keys || []).slice(0, n);
  const cnt = new Float64Array(32768), sr = new Float64Array(32768), sg = new Float64Array(32768), sb = new Float64Array(32768);
  for (const px of samples) {
    const step = Math.max(1, Math.floor(px.length / 4 / 200000));   // at most ~200k samples per frame
    for (let i = 0; i < px.length; i += 4 * step) {
      const r = px[i], g = px[i + 1], b = px[i + 2], k = ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3);
      cnt[k]++; sr[k] += r; sg[k] += g; sb[k] += b;
    }
  }
  const bins = [];
  for (let k = 0; k < 32768; k++) if (cnt[k]) bins.push(k);
  const want = n - keys.length, boxes = [{ bins, pop: bins.reduce((s, k) => s + cnt[k], 0) }];
  const ch = (k, c) => c === 0 ? k >> 10 : c === 1 ? (k >> 5) & 31 : k & 31;
  while (boxes.length < want) {
    // split the box with the largest population x range (ties: the earlier box)
    let bi = -1, best = 0;
    boxes.forEach((B, i) => {
      if (B.bins.length < 2) return;
      let score = 0;
      for (let c = 0; c < 3; c++) { let lo = 31, hi = 0; for (const k of B.bins) { const v = ch(k, c); if (v < lo) lo = v; if (v > hi) hi = v; } score = Math.max(score, hi - lo); }
      score *= B.pop;
      if (score > best) { best = score; bi = i; }
    });
    if (bi < 0) break;
    const B = boxes[bi];
    let axis = 0, span = -1;
    for (let c = 0; c < 3; c++) { let lo = 31, hi = 0; for (const k of B.bins) { const v = ch(k, c); if (v < lo) lo = v; if (v > hi) hi = v; } if (hi - lo > span) { span = hi - lo; axis = c; } }
    const sorted = B.bins.slice().sort((a, b) => ch(a, axis) - ch(b, axis) || a - b);
    let acc = 0, cut = 1;
    for (let i = 0; i < sorted.length - 1; i++) { acc += cnt[sorted[i]]; if (acc >= B.pop / 2) { cut = i + 1; break; } cut = i + 1; }
    const a = sorted.slice(0, cut), b = sorted.slice(cut);
    boxes.splice(bi, 1, { bins: a, pop: a.reduce((s, k) => s + cnt[k], 0) }, { bins: b, pop: b.reduce((s, k) => s + cnt[k], 0) });
  }
  const pal = keys.map(c => [c[0] | 0, c[1] | 0, c[2] | 0]);
  for (const B of boxes) {
    if (!B.pop) continue;
    let r = 0, g = 0, b = 0;
    for (const k of B.bins) { r += sr[k]; g += sg[k]; b += sb[k]; }
    pal.push([Math.round(r / B.pop), Math.round(g / B.pop), Math.round(b / B.pop)]);
  }
  while (pal.length < 2) pal.push([0, 0, 0]);
  return pal;
}

// ---- LZW (GIF variant: variable code size up to 12 bits, a clear code when the table is full) ----
function lzw(indices, minCode) {
  const clear = 1 << minCode, eoi = clear + 1, out = [];
  let size = minCode + 1, next = eoi + 1, cur = 0, bits = 0;
  const emit = code => { cur |= code << bits; bits += size; while (bits >= 8) { out.push(cur & 255); cur >>>= 8; bits -= 8; } };
  const dict = new Int16Array(4096 * 256);   // dict[prefix * 256 + byte] = code + 1 (0: none)
  emit(clear);
  let prefix = indices.length ? indices[0] : 0;
  for (let i = 1; i < indices.length; i++) {
    const c = indices[i], key = prefix * 256 + c, hit = dict[key];
    if (hit) { prefix = hit - 1; continue; }
    emit(prefix);
    if (next < 4096) {
      dict[key] = next + 1;
      if (next === (1 << size) && size < 12) size++;
      next++;
    } else {
      emit(clear); dict.fill(0); size = minCode + 1; next = eoi + 1;
    }
    prefix = c;
  }
  if (indices.length) emit(prefix);
  emit(eoi);
  if (bits > 0) out.push(cur & 255);
  return out;
}

// ---- the encoder ----
const BAYER4 = [0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5];
function encoder(opt) {
  const W = opt.width, H = opt.height, fps = opt.fps || 20, dither = opt.dither || 'bayer4';
  let pal = null, cache = null, prev = null, pending = null, frameNo = 0;
  const bytes = [];
  const u16 = v => [v & 255, (v >> 8) & 255];
  const nearest = (r, g, b) => {
    let bi = 0, bd = Infinity;
    for (let i = 0; i < pal.length; i++) { const p = pal[i], d = (p[0] - r) ** 2 + (p[1] - g) ** 2 + (p[2] - b) ** 2; if (d < bd) { bd = d; bi = i; } }
    return bi;
  };
  // palette index of a colour: exact palette colours map to themselves, the rest via an RGB555 cache
  let exact = null;
  const indexOf = (r, g, b) => {
    const e = exact.get((r << 16) | (g << 8) | b);
    if (e !== undefined) return e;
    const k = ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3);
    let v = cache[k];
    if (v < 0) v = cache[k] = nearest(((r >> 3) << 3) + 4, ((g >> 3) << 3) + 4, ((b >> 3) << 3) + 4);
    return v;
  };
  function setPalette(p) {
    pal = p.slice(0, 256);
    while (pal.length & (pal.length - 1) || pal.length < 2) pal.push([0, 0, 0]);   // a power of two
    cache = new Int16Array(32768).fill(-1);
    exact = new Map();
    pal.forEach((c, i) => { const k = (c[0] << 16) | (c[1] << 8) | c[2]; if (!exact.has(k)) exact.set(k, i); });
    const bitsCt = Math.log2(pal.length) - 1;
    bytes.push(...[71, 73, 70, 56, 57, 97], ...u16(W), ...u16(H), 0x80 | 0x70 | bitsCt, 0, 0);
    for (const c of pal) bytes.push(c[0], c[1], c[2]);
    if (opt.loop !== false) bytes.push(0x21, 0xFF, 0x0B, ...[...'NETSCAPE2.0'].map(c => c.charCodeAt(0)), 0x03, 0x01, 0, 0, 0x00);
  }
  function quantize(px) {
    const idx = new Uint8Array(W * H);
    if (dither === 'fs') {
      const err = new Float32Array((W + 2) * 3 * 2);
      for (let y = 0; y < H; y++) {
        const cur = (y & 1) * (W + 2) * 3, nxt = ((y + 1) & 1) * (W + 2) * 3;
        err.fill(0, nxt, nxt + (W + 2) * 3);
        const ltr = !(y & 1);
        for (let s = 0; s < W; s++) {
          const x = ltr ? s : W - 1 - s, i = (y * W + x) * 4, e = cur + (x + 1) * 3;
          const r = Math.min(255, Math.max(0, Math.round(px[i] + err[e]))), g = Math.min(255, Math.max(0, Math.round(px[i + 1] + err[e + 1]))), b = Math.min(255, Math.max(0, Math.round(px[i + 2] + err[e + 2])));
          const k = indexOf(r, g, b), p = pal[k];
          idx[y * W + x] = k;
          const dr = r - p[0], dg = g - p[1], db = b - p[2], d = ltr ? 3 : -3;
          for (const [o, w] of [[e + d, 7 / 16], [nxt + (x + 1) * 3 - d, 3 / 16], [nxt + (x + 1) * 3, 5 / 16], [nxt + (x + 1) * 3 + d, 1 / 16]]) {
            err[o] += dr * w; err[o + 1] += dg * w; err[o + 2] += db * w;
          }
        }
      }
      return idx;
    }
    const amp = dither === 'bayer4' ? 10 : 0;
    for (let y = 0, i = 0, j = 0; y < H; y++) for (let x = 0; x < W; x++, i += 4, j++) {
      const r = px[i], g = px[i + 1], b = px[i + 2];
      if (amp) {
        const k0 = exact.get((r << 16) | (g << 8) | b);
        if (k0 !== undefined) { idx[j] = k0; continue; }   // key colours (the background) stay clean
        const t = (BAYER4[(y & 3) * 4 + (x & 3)] / 16 - 0.5) * amp;
        idx[j] = indexOf(Math.min(255, Math.max(0, Math.round(r + t))), Math.min(255, Math.max(0, Math.round(g + t))), Math.min(255, Math.max(0, Math.round(b + t))));
      } else idx[j] = indexOf(r, g, b);
    }
    return idx;
  }
  const delayOf = i => Math.round((i + 1) * 100 / fps) - Math.round(i * 100 / fps);
  function writeFrame(f) {
    const { idx, x0, y0, w, h, delay } = f;
    bytes.push(0x21, 0xF9, 0x04, 0x04, ...u16(Math.max(1, delay)), 0, 0x00);   // disposal 1: leave in place
    bytes.push(0x2C, ...u16(x0), ...u16(y0), ...u16(w), ...u16(h), 0x00);
    const sub = new Uint8Array(w * h);
    for (let y = 0; y < h; y++) sub.set(idx.subarray((y0 + y) * W + x0, (y0 + y) * W + x0 + w), y * w);
    const minCode = Math.max(2, Math.log2(pal.length));
    bytes.push(minCode);
    const data = lzw(sub, minCode);
    for (let i = 0; i < data.length; i += 255) { const n = Math.min(255, data.length - i); bytes.push(n); for (let k = 0; k < n; k++) bytes.push(data[i + k]); }
    bytes.push(0x00);
  }
  function addFrame(px) {
    if (!pal) setPalette(buildPalette([px], opt.keys || []));
    const idx = quantize(px), delay = delayOf(frameNo++);
    let x0 = 0, y0 = 0, x1 = W - 1, y1 = H - 1;
    if (prev) {
      x0 = W; y0 = H; x1 = -1; y1 = -1;
      for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) if (idx[y * W + x] !== prev[y * W + x]) {
        if (x < x0) x0 = x; if (x > x1) x1 = x; if (y < y0) y0 = y; if (y > y1) y1 = y;
      }
      if (x1 < 0) { pending.delay += delay; return; }   // unchanged: the previous frame stays on longer
    }
    if (pending) writeFrame(pending);
    pending = { idx, x0, y0, w: x1 - x0 + 1, h: y1 - y0 + 1, delay };
    prev = idx;
  }
  function finish() {
    if (pending) writeFrame(pending);
    pending = null;
    bytes.push(0x3B);
    return Uint8Array.from(bytes);
  }
  return { setPalette, addFrame, finish, get palette() { return pal; } };
}

globalThis.IscGif = { buildPalette, encoder, lzw };
})();
