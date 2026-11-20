#!/usr/bin/env node
// SPDX-License-Identifier: BSD-3-Clause
// The viewer's TIFF writer (writeTiff16 in web/index.html: Record's Save .tif, Make movie's) under Node, in well under a
// second (2026-11-19):
//   - a classic file is byte for byte the writer's before BigTIFF and parts (its code kept below), from one array and
//     from parts, with and without the pixel size;
//   - a BigTIFF (forced) read back here: the IFD chain, each page's offset, size and pixels, the description, the
//     pixel size (a RATIONAL in its entry); with Python and tifffile at hand, tifffile reads it the same;
//   - a 4.4 GB movie's layout, made with a Blob that only counts its parts (the frames are one array 520 times): a
//     BigTIFF by itself, every page's offset where its frame is, past 4 GB too;
//   - a Record written straight to a file (2026-11-20, tiffHead with dataAt): room kept in front of the frames, the
//     tags written last for the 4 frames it has, classic and BigTIFF: read back here and by tifffile, the pixels the
//     same as the plain file's.
//   node tests/web/tiff_unit.mjs
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const t0 = performance.now();
let fail = 0;
const check = (ok, what) => { if (!ok) fail++; console.log(`${ok ? 'ok  ' : 'FAIL'}  ${what}`); };
const html = fs.readFileSync(path.join(ROOT, 'web/index.html'), 'utf8');
const a = html.indexOf('function tiffHeadBytes('), b = html.indexOf('\nfunction saveMovieTiff(');
const [writeTiff16, tiffHead, tiffHeadBytes] = new Function(html.slice(a, b) + '; return [writeTiff16, tiffHead, tiffHeadBytes];')();

// The writer before 2026-11-19 (one array, one buffer, classic only), verbatim.
function oldWriteTiff16(frames, w, h, n, desc, pxUm) {
  const enc = new TextEncoder().encode(desc + '\0'), px = w * h, res = pxUm > 0;
  const entries = p => (p === 0 ? 10 : 9) + (res ? 3 : 0), ifdBytes = p => 2 + 12 * entries(p) + 4 + (res ? 16 : 0);
  const ppu = res ? Math.round(1e6 / pxUm) : 0;
  let head = 8 + enc.length + (enc.length & 1);
  for (let p = 0; p < n; p++) head += ifdBytes(p);
  const buf = new ArrayBuffer(head + n * px * 2), dv = new DataView(buf), u8 = new Uint8Array(buf);
  dv.setUint16(0, 0x4949, true); dv.setUint16(2, 42, true);
  new Uint16Array(buf, head, n * px).set(frames.subarray ? frames.subarray(0, n * px) : frames.slice(0, n * px));
  let pos = 8, link = 4;
  for (let p = 0; p < n; p++) {
    const data = head + p * px * 2;
    let descOff = 0;
    if (p === 0) { descOff = pos; u8.set(enc, pos); pos += enc.length + (enc.length & 1); }
    let resOff = 0;
    if (res) { resOff = pos; for (let k = 0; k < 2; k++) { dv.setUint32(pos, ppu, true); dv.setUint32(pos + 4, 1000000, true); pos += 8; } }
    dv.setUint32(link, pos, true);
    const E = [[256, 4, 1, w], [257, 4, 1, h], [258, 3, 1, 16], [259, 3, 1, 1], [262, 3, 1, 1]];
    if (p === 0) E.push([270, 2, enc.length, descOff]);
    E.push([273, 4, 1, data], [277, 3, 1, 1], [278, 4, 1, h], [279, 4, 1, px * 2]);
    if (res) E.push([282, 5, 1, resOff], [283, 5, 1, resOff + 8], [296, 3, 1, 1]);
    dv.setUint16(pos, E.length, true); pos += 2;
    for (const [tag, type, count, value] of E) {
      dv.setUint16(pos, tag, true); dv.setUint16(pos + 2, type, true); dv.setUint32(pos + 4, count, true);
      if (type === 3) dv.setUint16(pos + 8, value, true); else dv.setUint32(pos + 8, value, true);
      pos += 12;
    }
    link = pos; dv.setUint32(pos, 0, true); pos += 4;
  }
  return new Blob([buf], { type: 'image/tiff' });
}

// A reader of what the writer makes: { big, pages: [{ w, h, bits, offset, bytes, desc, xres }] } (only the head needs
// to be there: the frames' bytes are read by the caller).
function readTiff(u8) {
  const v = new DataView(u8.buffer, u8.byteOffset, u8.byteLength);
  const u64 = i => Number(v.getBigUint64(i, true));
  if (v.getUint16(0, true) !== 0x4949) throw new Error('not little-endian');
  const big = v.getUint16(2, true) === 43;
  if (!big && v.getUint16(2, true) !== 42) throw new Error('not a TIFF');
  let ifd = big ? u64(8) : v.getUint32(4, true);
  const pages = [];
  while (ifd) {
    const n = big ? u64(ifd) : v.getUint16(ifd, true);
    const p = {};
    for (let k = 0; k < n; k++) {
      const e = ifd + (big ? 8 + 20 * k : 2 + 12 * k), tag = v.getUint16(e, true), type = v.getUint16(e + 2, true);
      const count = big ? u64(e + 4) : v.getUint32(e + 4, true), at = e + (big ? 12 : 8);
      const val = type === 3 ? v.getUint16(at, true) : type === 4 ? v.getUint32(at, true) : big ? u64(at) : v.getUint32(at, true);
      if (tag === 256) p.w = val;
      if (tag === 257) p.h = val;
      if (tag === 258) p.bits = val;
      if (tag === 273) p.offset = val;
      if (tag === 279) p.bytes = val;
      if (tag === 270) p.desc = new TextDecoder().decode(u8.subarray(val, val + count - 1));
      if (tag === 282) {   // a RATIONAL: in its entry in a BigTIFF (8 bytes fit), at an offset in a classic file
        const r = big ? at : v.getUint32(at, true);
        p.xres = v.getUint32(r, true) / v.getUint32(r + 4, true);
      }
    }
    pages.push(p);
    ifd = big ? u64(ifd + 8 + 20 * n) : v.getUint32(ifd + 2 + 12 * n, true);
  }
  return { big, pages };
}

const w = 37, h = 11, n = 6, px = w * h;   // odd sizes: the description's pad byte, frames not on 4-byte bounds
const movie = new Uint16Array(n * px).map((_, i) => (i * 2654435761) >>> 16);
const parts = [movie.subarray(0, 2 * px), movie.subarray(2 * px, 5 * px), movie.subarray(5 * px)];
const desc = 'ImageJ=1.11a\nimages=6\nframes=6\nunit=micron\ninsiliscope.spec=size=37 frames=6';
const bytes = async blob => new Uint8Array(await blob.arrayBuffer());
const same = (x, y) => x.length === y.length && x.every((c, i) => c === y[i]);

for (const pxUm of [0, 0.1]) {
  const want = await bytes(oldWriteTiff16(movie, w, h, n, desc, pxUm));
  check(same(await bytes(writeTiff16(movie, w, h, n, desc, pxUm)), want) &&
        same(await bytes(writeTiff16(parts, w, h, n, desc, pxUm)), want),
    `classic${pxUm ? ' with the pixel size' : ''}: byte for byte the old writer's, from one array and from parts (${want.length} bytes)`);
}
check(same(await bytes(writeTiff16(parts, w, h, 4, desc, 0.1)), await bytes(oldWriteTiff16(movie, w, h, 4, desc, 0.1))),
  'the first 4 of 6 frames from parts = the old writer\'s');
// a recording on the disk storage: its frames file as one Blob (all frames, or the first 4)
{
  const file = new Blob([movie]);
  check(same(await bytes(writeTiff16([file], w, h, n, desc, 0.1)), await bytes(oldWriteTiff16(movie, w, h, n, desc, 0.1))) &&
        same(await bytes(writeTiff16([file], w, h, 4, desc, 0.1)), await bytes(oldWriteTiff16(movie, w, h, 4, desc, 0.1))),
    'the frames as a Blob (a frames file on the disk storage): all 6 and the first 4 = the old writer\'s');
}

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'isc-tiff-'));
const bigFile = await bytes(writeTiff16(parts, w, h, n, desc, 0.1, true));
{
  const r = readTiff(bigFile);
  const ok = r.big && r.pages.length === n && r.pages.every((p, k) => p.w === w && p.h === h && p.bits === 16 &&
    p.bytes === px * 2 && same(new Uint16Array(bigFile.slice(p.offset, p.offset + px * 2).buffer), movie.subarray(k * px, (k + 1) * px)));
  check(ok && r.pages[0].desc === desc && Math.abs(r.pages[0].xres - 10) < 1e-9,
    `a BigTIFF (forced) read back: ${r.pages.length} pages, sizes, offsets, pixels, the description, 10 px/um`);
  fs.writeFileSync(path.join(dir, 'big.tif'), bigFile);
  fs.writeFileSync(path.join(dir, 'classic.tif'), await bytes(writeTiff16(movie, w, h, n, desc, 0.1)));
}

// A Record straight to a file: room for 6 frames' tags (a BigTIFF's, the description's bytes and some), 4 frames
// written, the tags for those 4 at the start, the file cut after the 4th frame.
for (const big of [false, true]) {
  const d4 = desc.replace(/=6/g, '=4'), at = tiffHeadBytes(n, 4096, 0.1, true) + 64;
  const head = new Uint8Array(tiffHead(w, h, 4, d4, 0.1, big, at));
  const file = new Uint8Array(at + 4 * px * 2);
  file.set(head, 0);
  file.set(new Uint8Array(movie.buffer, 0, 4 * px * 2), at);
  const r = readTiff(file);
  check(head.length <= at && r.big === big && r.pages.length === 4 && r.pages.every((p, k) => p.offset === at + k * px * 2 &&
      p.bytes === px * 2 && same(new Uint16Array(file.slice(p.offset, p.offset + px * 2).buffer), movie.subarray(k * px, (k + 1) * px))) &&
      r.pages[0].desc === d4,
    `straight to a file (${big ? 'BigTIFF' : 'classic'}): tags ${head.length} of ${at} bytes kept, 4 pages where the frames are`);
  fs.writeFileSync(path.join(dir, big ? 'inplace_big.tif' : 'inplace.tif'), file);
}

// 520 frames of 2048 x 2048 px (4.4 GB) through a Blob that keeps its parts uncopied: the layout only.
{
  const Real = globalThis.Blob;
  globalThis.Blob = class { constructor(p) { this.parts = p; this.size = p.reduce((s, x) => s + (x.byteLength ?? x.length), 0); } };
  const W = 2048, N = 520, frame = new Uint16Array(W * W);
  let blob;
  try { blob = writeTiff16(new Array(N).fill(frame), W, W, N, desc, 0.1); } finally { globalThis.Blob = Real; }
  const head = new Uint8Array(blob.parts[0]), r = readTiff(head), stride = W * W * 2;
  const ok = r.big && r.pages.length === N && blob.parts.length === N + 1 &&
    r.pages.every((p, k) => p.offset === head.length + k * stride && p.bytes === stride && p.w === W && p.h === W) &&
    blob.size === head.length + N * stride;
  check(ok && r.pages[N - 1].offset > 2 ** 32 && Math.abs(r.pages[N - 1].xres - 10) < 1e-9,
    `a ${(blob.size / 1e9).toFixed(1)} GB movie is a BigTIFF by itself: ${r.pages.length} pages, the last at ` +
    `${(r.pages[N - 1].offset / 1e9).toFixed(2)} GB, each where its frame is`);
}

// tifffile, if there is one: the BigTIFF and the classic file the same movie.
let py = null;
for (const exe of ['python3', 'python']) {
  try { execFileSync(exe, ['-c', 'import tifffile'], { stdio: 'pipe' }); py = exe; break; } catch (e) { /* next */ }
}
if (!py) console.log('skip  tifffile (no Python with tifffile)');
else {
  const script = 'import sys, tifffile, numpy as np\n' +
    'a = tifffile.TiffFile(sys.argv[1]); b = tifffile.TiffFile(sys.argv[2])\n' +
    'x = np.stack([p.asarray() for p in a.pages]); y = np.stack([p.asarray() for p in b.pages])\n' +
    'print(a.is_bigtiff, b.is_bigtiff, x.shape, bool((x == y).all()), a.pages[0].tags["XResolution"].value)\n';
  let out = '';
  try { out = execFileSync(py, ['-I', '-c', script, path.join(dir, 'big.tif'), path.join(dir, 'classic.tif')], { encoding: 'utf8' }); } catch (e) { out = String(e.stderr || e); }
  check(/^True False \(6, 11, 37\) True \(10000000, 1000000\)/.test(out.trim()), `tifffile reads the BigTIFF as the classic file: ${out.trim()}`);
  const s2 = 'import sys, tifffile, numpy as np\n' +
    'y = np.stack([p.asarray() for p in tifffile.TiffFile(sys.argv[3]).pages])[:4]\n' +
    'for f in sys.argv[1:3]:\n' +
    '    t = tifffile.TiffFile(f); x = np.stack([p.asarray() for p in t.pages]); print(t.is_bigtiff, x.shape, bool((x == y).all()))\n';
  try { out = execFileSync(py, ['-I', '-c', s2, path.join(dir, 'inplace.tif'), path.join(dir, 'inplace_big.tif'), path.join(dir, 'classic.tif')], { encoding: 'utf8' }); } catch (e) { out = String(e.stderr || e); }
  check(/^False \(4, 11, 37\) True\s+True \(4, 11, 37\) True$/.test(out.trim()), `tifffile reads the files written in place: ${out.trim().replace(/\s+/g, ' ')}`);
}
fs.rmSync(dir, { recursive: true, force: true });
console.log(`tiff unit: ${fail ? fail + ' FAILED' : 'all ok'}, ${((performance.now() - t0) / 1000).toFixed(2)} s`);
process.exit(fail ? 1 : 0);
