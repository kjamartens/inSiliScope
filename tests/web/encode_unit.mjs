#!/usr/bin/env node
// SPDX-License-Identifier: BSD-3-Clause
// The viewer's export encoders in Node (web/encode/gif.js, mp4.js, webm.js; video.js needs a browser's WebCodecs and
// is checked by tests/web/viewer_anim_export.mjs): GIFs decode back to their frames (pixel-exact within the palette,
// LZW edge cases, delays, the loop flag, unchanged frames merged) and are byte-deterministic; the MP4 and WebM
// containers have the structure players need.
//   node tests/web/encode_unit.mjs
import fs from 'node:fs';
import path from 'node:path';
import vm from 'node:vm';
import { fileURLToPath } from 'node:url';
import { decodeGif, mp4Boxes, findBox, ebml } from './lib/decoders.mjs';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const ctx = vm.createContext({ Math, Object, Array, Map, Set, Uint8Array, Int16Array, Float64Array, Float32Array, Uint16Array,
  DataView, ArrayBuffer, String, Number, Error });
ctx.globalThis = ctx; ctx.self = ctx;
for (const f of ['gif.js', 'mp4.js', 'webm.js']) vm.runInContext(fs.readFileSync(path.join(ROOT, 'web/encode', f), 'utf8'), ctx, { filename: f });
const { IscGif, IscMp4, IscWebm } = ctx;
let fail = 0;
const check = (ok, what) => { if (!ok) fail++; console.log(`${ok ? 'ok  ' : 'FAIL'}  ${what}`); };

// a deterministic pseudo-random source
let seed = 12345;
const rnd = () => ((seed = (Math.imul(seed, 1664525) + 1013904223) >>> 0) / 4294967296);
function frame(W, H, fn) { const px = new Uint8Array(W * H * 4); for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) { const [r, g, b] = fn(x, y), o = 4 * (y * W + x); px[o] = r; px[o + 1] = g; px[o + 2] = b; px[o + 3] = 255; } return px; }
function encodeGif(frames, W, H, opt) {
  const enc = IscGif.encoder(Object.assign({ width: W, height: H, fps: 20, dither: 'none' }, opt));
  enc.setPalette(IscGif.buildPalette(frames, (opt && opt.keys) || []));
  for (const f of frames) enc.addFrame(f);
  return { bytes: enc.finish(), pal: enc.palette };
}
const sameRgb = (rgba, rgb) => { for (let i = 0, j = 0; i < rgba.length; i += 4, j += 3) if (rgba[i] !== rgb[j] || rgba[i + 1] !== rgb[j + 1] || rgba[i + 2] !== rgb[j + 2]) return false; return true; };

// 1. few colours: exact
{
  const W = 37, H = 23, cols = [[0, 0, 0], [255, 0, 0], [0, 255, 128], [20, 40, 60], [250, 250, 250]];
  const frames = [0, 1, 2].map(k => frame(W, H, (x, y) => cols[(x * 3 + y * 7 + k * 2) % cols.length]));
  const { bytes } = encodeGif(frames, W, H);
  const g = decodeGif(bytes);
  check(g.width === W && g.height === H && g.loop && g.frames.length === 3 && frames.every((f, i) => sameRgb(f, g.frames[i].rgb)),
    'GIF: <= 256 colours decode back pixel-exact, loop flag set');
  check(g.frames.map(f => f.delay).join() === '5,5,5', 'GIF: delays in 1/100 s (20 fps)');
}
// 2. LZW edge cases: 1x1, two colours, noise needing clear codes, an odd width
{
  const cases = [[1, 1, () => [9, 8, 7]], [5, 3, (x, y) => ((x + y) & 1 ? [0, 0, 0] : [255, 255, 255])], [257, 129, () => [rnd() * 256 | 0, rnd() * 256 | 0, 0]]];
  let ok = true;
  for (const [W, H, fn] of cases) {
    const f = frame(W, H, fn), { bytes, pal } = encodeGif([f], W, H), g = decodeGif(bytes);
    // every pixel decodes to the palette colour the encoder chose for it: compare against the nearest palette colour
    for (let i = 0, j = 0; i < f.length; i += 4, j += 3) {
      const near = pal.reduce((best, c) => { const d = (c[0] - f[i]) ** 2 + (c[1] - f[i + 1]) ** 2 + (c[2] - f[i + 2]) ** 2; return d < best[0] ? [d, c] : best; }, [Infinity, null]);
      const got = [g.frames[0].rgb[j], g.frames[0].rgb[j + 1], g.frames[0].rgb[j + 2]];
      const dGot = (got[0] - f[i]) ** 2 + (got[1] - f[i + 1]) ** 2 + (got[2] - f[i + 2]) ** 2;
      if (dGot > near[0] + 3 * 64 * 3) { ok = false; break; }   // within one RGB555 bin of the best
    }
  }
  check(ok, 'GIF: LZW edge cases (1x1, 2 colours, 257x129 noise with clear codes) decode');
}
// 3. gradients within tolerance, dithered; unchanged frames merged; deterministic; sub-rectangles
{
  const W = 64, H = 40;
  const grad = frame(W, H, (x, y) => [x * 4, y * 6, 128]);
  const moved = frame(W, H, (x, y) => (x > 30 && x < 40 && y > 10 && y < 20 ? [255, 255, 255] : [x * 4, y * 6, 128]));
  const a = encodeGif([grad, grad, grad, moved], W, H, { dither: 'bayer4', keys: [[255, 255, 255]] });
  const b = encodeGif([grad, grad, grad, moved], W, H, { dither: 'bayer4', keys: [[255, 255, 255]] });
  const g = decodeGif(a.bytes);
  let maxErr = 0;
  for (let i = 0, j = 0; i < grad.length; i += 4, j += 3) for (let k = 0; k < 3; k++) maxErr = Math.max(maxErr, Math.abs(grad[i + k] - g.frames[0].rgb[j + k]));
  check(maxErr <= 24, `GIF: a gradient within ${maxErr} levels (ordered dither)`);
  check(g.frames.length === 2 && g.frames[0].delay === 15 && g.frames[1].delay === 5, 'GIF: unchanged frames merged into one longer delay');
  let box = true;
  for (let y = 11; y < 20; y++) for (let x = 31; x < 40; x++) { const o = 3 * (y * W + x); if (g.frames[1].rgb[o] !== 255 || g.frames[1].rgb[o + 1] !== 255) box = false; }
  check(box, 'GIF: a changed rectangle composes onto the previous frame (key colour exact)');
  check(Buffer.from(a.bytes).equals(Buffer.from(b.bytes)), 'GIF: encoding twice gives the same bytes');
  const fs2 = encodeGif([grad], W, H, { dither: 'fs' }), gfs = decodeGif(fs2.bytes);
  check(gfs.frames.length === 1, 'GIF: Floyd-Steinberg dither decodes');
}
// 4. delays sum to the duration at 30 fps
{
  const W = 8, H = 8, frames = [];
  for (let i = 0; i < 30; i++) frames.push(frame(W, H, () => [i * 8, 0, 0]));
  const g = decodeGif(encodeGif(frames, W, H, { fps: 30 }).bytes);
  check(g.frames.reduce((s, f) => s + f.delay, 0) === 100, 'GIF: 30 frames at 30 fps last 100/100 s');
}
// 5. MP4 structure
{
  const avcC = Uint8Array.from([1, 0x42, 0xE0, 0x1F, 0xFF, 0xE1, 0, 4, 0x67, 1, 2, 3, 1, 0, 2, 0x68, 9]);
  const samples = [];
  for (let i = 0; i < 25; i++) { const d = new Uint8Array(10 + i); d.fill(i); samples.push({ data: d, key: i % 10 === 0 }); }
  const b = IscMp4.mux({ width: 320, height: 240, fps: 25, avcC, samples });
  const top = mp4Boxes(b), dv = new DataView(b.buffer, b.byteOffset);
  check(top.map(x => x.type).join() === 'ftyp,moov,mdat', 'MP4: ftyp, moov, mdat');
  const stsz = findBox(top, 'moov/trak/mdia/minf/stbl/stsz'), stco = findBox(top, 'moov/trak/mdia/minf/stbl/stco');
  const stts = findBox(top, 'moov/trak/mdia/minf/stbl/stts'), stss = findBox(top, 'moov/trak/mdia/minf/stbl/stss');
  const mdhd = findBox(top, 'moov/trak/mdia/mdhd'), stsd = findBox(top, 'moov/trak/mdia/minf/stbl/stsd');
  const u = (bx, off) => dv.getUint32(bx.body.byteOffset - b.byteOffset + off);
  const n = u(stsz, 8), sizesOk = samples.every((s, i) => u(stsz, 12 + 4 * i) === s.data.length);
  const off = u(stco, 8), firstOk = b[off] === 0 && b[off + 10] === 1 && off === top[2].start + 8;
  check(n === 25 && sizesOk && firstOk, 'MP4: stsz sizes, stco points at the first sample in mdat');
  check(u(stts, 8) === 25 && u(stts, 12) * 25 === u(mdhd, 16) && u(mdhd, 12) === 90000, 'MP4: stts x count = mdhd duration (90 kHz)');
  check(u(stss, 4) === 3 && [u(stss, 8), u(stss, 12), u(stss, 16)].join() === '1,11,21', 'MP4: stss lists the keyframes');
  const avcBox = Buffer.from(stsd.body).indexOf('avcC');
  check(avcBox > 0 && Buffer.from(stsd.body.subarray(avcBox + 4, avcBox + 4 + avcC.length)).equals(Buffer.from(avcC)), 'MP4: avcC is the description, verbatim');
  // reordered timestamps: ctts + edit list
  const re = samples.map((s, i) => Object.assign({}, s, { pts: [0, 3, 1, 2][i % 4] * 40000 + Math.floor(i / 4) * 160000 }));
  const t2 = mp4Boxes(IscMp4.mux({ width: 320, height: 240, fps: 25, avcC, samples: re }));
  check(!!findBox(t2, 'moov/trak/mdia/minf/stbl/ctts') && !!findBox(t2, 'moov/trak/edts/elst'), 'MP4: reordered frames get ctts and an edit list');
  // Annex B
  const ab = Uint8Array.from([0, 0, 0, 1, 0x67, 0x42, 0xE0, 0x1F, 5, 0, 0, 1, 0x68, 9, 0, 0, 0, 1, 0x65, 1, 2, 3]);
  const conv = IscMp4.annexBToAvcc(ab), rec = IscMp4.avcCFromAnnexB(ab);
  check(conv.length === 8 && conv[3] === 4 && conv[4] === 0x65 && rec[1] === 0x42 && rec[3] === 0x1F, 'MP4: Annex B to length-prefixed NAL units and an avcC record');
}
// 6. WebM structure
{
  const samples = [];
  for (let i = 0; i < 70; i++) samples.push({ data: new Uint8Array(20).fill(i), key: i % 30 === 0 });
  const b = IscWebm.mux({ width: 320, height: 240, fps: 30, codec: 'V_VP9', samples });
  const top = ebml(b), seg = top[1];
  const ids = seg.children.map(e => e.id);
  check(top[0].id === '1a45dfa3' && seg.id === '18538067' && seg.dataStart + seg.size === b.length, 'WebM: EBML header, the Segment spans the rest');
  check(ids[0] === '1549a966' && ids[1] === '1654ae6b' && ids[2] === '1c53bb6b' && ids.slice(3).every(i => i === '1f43b675') && ids.length === 6,
    'WebM: Info, Tracks, Cues, then one cluster per keyframe run (3)');
  const cues = seg.children[2].children, clusters = seg.children.slice(3);
  const posOk = cues.every((cp, i) => { const ctp = cp.children.find(e => e.id === 'b7'), pos = ctp.children.find(e => e.id === 'f1'); let v = 0; for (let k = 0; k < pos.size; k++) v = v * 256 + b[pos.dataStart + k]; return seg.dataStart + v === clusters[i].start; });
  check(posOk, 'WebM: cue positions land on the clusters');
  const keyOk = clusters.every(c => { const sb = c.children.find(e => e.id === 'a3'); return (b[sb.dataStart + 3] & 0x80) !== 0; });
  check(keyOk, 'WebM: every cluster starts with a keyframe');
  const dur = new DataView(b.buffer).getFloat64(seg.children[0].children.find(e => e.id === '4489').dataStart);
  check(Math.abs(dur - 70 * 1000 / 30) < 1e-9, 'WebM: Duration (ms)');
}
process.exit(fail ? 1 : 0);
