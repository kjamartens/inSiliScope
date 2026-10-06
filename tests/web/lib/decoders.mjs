// SPDX-License-Identifier: BSD-3-Clause
// Test-only readers for the viewer's encoders (tests/web/encode_unit.mjs, viewer_anim_export.mjs): an animated GIF
// decoder (composited RGB frames, delays, loop flag), an MP4 box tree and an EBML (WebM) element tree.

// GIF -> {width, height, loop, frames: [{rgb: Uint8Array(w*h*3), delay}]} (frames composited with disposal 1)
export function decodeGif(b) {
  const sig = String.fromCharCode(...b.subarray(0, 6));
  if (sig !== 'GIF89a' && sig !== 'GIF87a') throw new Error('not a GIF');
  const W = b[6] | (b[7] << 8), H = b[8] | (b[9] << 8), flags = b[10];
  let p = 13, gct = null, loop = false, delay = 0;
  if (flags & 0x80) { const n = 2 << (flags & 7); gct = b.subarray(p, p + 3 * n); p += 3 * n; }
  const canvas = new Uint8Array(W * H * 3), frames = [];
  const subBlocks = () => { const parts = []; for (;;) { const n = b[p++]; if (!n) break; parts.push(b.subarray(p, p + n)); p += n; } return Buffer.concat(parts.map(x => Buffer.from(x))); };
  while (p < b.length) {
    const t = b[p++];
    if (t === 0x3B) break;
    if (t === 0x21) {
      const label = b[p++];
      if (label === 0xF9) { delay = b[p + 2] | (b[p + 3] << 8); p += 1 + b[p] ; p++; }
      else if (label === 0xFF) { const id = String.fromCharCode(...b.subarray(p + 1, p + 12)); p += 1 + b[p]; const d = subBlocks(); if (id === 'NETSCAPE2.0' && d[0] === 1) loop = true; }
      else { p += 1 + b[p]; subBlocks(); }
      continue;
    }
    if (t !== 0x2C) throw new Error('bad block 0x' + t.toString(16) + ' at ' + (p - 1));
    const x0 = b[p] | (b[p + 1] << 8), y0 = b[p + 2] | (b[p + 3] << 8), w = b[p + 4] | (b[p + 5] << 8), h = b[p + 6] | (b[p + 7] << 8), f = b[p + 8];
    p += 9;
    let ct = gct;
    if (f & 0x80) { const n = 2 << (f & 7); ct = b.subarray(p, p + 3 * n); p += 3 * n; }
    const minCode = b[p++], data = subBlocks(), idx = lzwDecode(data, minCode, w * h);
    for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) {
      const k = idx[y * w + x], o = ((y0 + y) * W + x0 + x) * 3;
      canvas[o] = ct[3 * k]; canvas[o + 1] = ct[3 * k + 1]; canvas[o + 2] = ct[3 * k + 2];
    }
    frames.push({ rgb: canvas.slice(), delay });
  }
  return { width: W, height: H, loop, frames };
}
function lzwDecode(data, minCode, n) {
  const clear = 1 << minCode, eoi = clear + 1, out = new Uint8Array(n);
  let size = minCode + 1, next = eoi + 1, bitPos = 0, o = 0, prev = -1;
  const pre = new Int32Array(4096), suf = new Uint8Array(4096), len = new Int32Array(4096);
  for (let i = 0; i < clear; i++) { pre[i] = -1; suf[i] = i; len[i] = 1; }
  const first = c => { while (pre[c] >= 0) c = pre[c]; return suf[c]; };
  const write = c => { const l = len[c]; for (let i = l - 1, k = c; i >= 0; i--, k = pre[k]) if (o + i < n) out[o + i] = suf[k]; o += l; };
  for (;;) {
    let code = 0;
    for (let i = 0; i < size; i++, bitPos++) if (data[bitPos >> 3] & (1 << (bitPos & 7))) code |= 1 << i;
    if ((bitPos >> 3) > data.length) throw new Error('LZW data ran out');
    if (code === clear) { size = minCode + 1; next = eoi + 1; prev = -1; continue; }
    if (code === eoi) break;
    if (prev < 0) { write(code); prev = code; continue; }
    if (code < next) { write(code); if (next < 4096) { pre[next] = prev; suf[next] = first(code); len[next] = len[prev] + 1; next++; } }
    else if (code === next) { pre[next] = prev; suf[next] = first(prev); len[next] = len[prev] + 1; next++; write(code); }
    else throw new Error('bad LZW code ' + code + ' (next ' + next + ')');
    if (next === (1 << size) && size < 12) size++;
    prev = code;
  }
  if (o !== n) throw new Error(`LZW gave ${o} pixels, want ${n}`);
  return out;
}

// MP4 -> [{type, start, size, children?, body}] (containers parsed)
const CONTAINERS = new Set(['moov', 'trak', 'mdia', 'minf', 'stbl', 'dinf', 'edts']);
export function mp4Boxes(b, start = 0, end = b.length) {
  const out = [], dv = new DataView(b.buffer, b.byteOffset, b.byteLength);
  for (let p = start; p < end;) {
    const size = dv.getUint32(p), type = String.fromCharCode(...b.subarray(p + 4, p + 8));
    if (size < 8 || p + size > end) throw new Error(`bad box ${type} size ${size} at ${p}`);
    const bx = { type, start: p, size, body: b.subarray(p + 8, p + size) };
    if (CONTAINERS.has(type)) bx.children = mp4Boxes(b, p + 8, p + size);
    out.push(bx);
    p += size;
  }
  return out;
}
export function findBox(boxes, path) {
  let cur = boxes, bx = null;
  for (const t of path.split('/')) { bx = (cur || []).find(x => x.type === t); if (!bx) return null; cur = bx.children; }
  return bx;
}

// EBML -> [{id (hex), start, dataStart, size, children?}] for the WebM master elements
const MASTERS = new Set(['1a45dfa3', '18538067', '1549a966', '1654ae6b', 'ae', 'e0', '1c53bb6b', 'bb', 'b7', '1f43b675']);
export function ebml(b, start = 0, end = b.length) {
  const out = [];
  for (let p = start; p < end;) {
    let w = 1; while (w <= 4 && !(b[p] & (0x80 >> (w - 1)))) w++;
    const id = [...b.subarray(p, p + w)].map(x => x.toString(16).padStart(2, '0')).join('');
    let q = p + w, sw = 1; while (sw <= 8 && !(b[q] & (0x80 >> (sw - 1)))) sw++;
    let size = b[q] & (0xFF >> sw);
    for (let i = 1; i < sw; i++) size = size * 256 + b[q + i];
    const dataStart = q + sw;
    if (dataStart + size > end) throw new Error(`EBML element ${id} overruns its parent`);
    const e = { id, start: p, dataStart, size };
    if (MASTERS.has(id)) e.children = ebml(b, dataStart, dataStart + size);
    out.push(e);
    p = dataStart + size;
  }
  return out;
}
