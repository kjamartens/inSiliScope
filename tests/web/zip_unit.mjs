#!/usr/bin/env node
// SPDX-License-Identifier: BSD-3-Clause
// web/live/zip.js (the viewer's Save .zip) under Node, in well under a second:
//   - a round trip: every entry back from the central directory, inflated (node:zlib), its CRC-32 and sizes right;
//     text deflated, a stored entry stored, UTF-8 names;
//   - Zip64 records (forced: a 4 GB file is not for a unit test) read back the same;
//   - with Python at hand, its zipfile reads both (testzip, the names, the bytes).
//   node tests/web/zip_unit.mjs
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import zlib from 'node:zlib';
import { execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const t0 = performance.now();
let fail = 0;
const check = (ok, what) => { if (!ok) fail++; console.log(`${ok ? 'ok  ' : 'FAIL'}  ${what}`); };
const self = globalThis.self || (globalThis.self = globalThis);
new Function('self', fs.readFileSync(path.join(ROOT, 'web/live/zip.js'), 'utf8'))(self);
const Z = self.iscZip;

// A reader of what zip.js writes: the end record (Zip64 when present), the central directory, each entry's data.
function read(buf) {
  const v = new DataView(buf.buffer, buf.byteOffset, buf.byteLength);
  let e = buf.length - 22;
  while (e >= 0 && v.getUint32(e, true) !== 0x06054b50) e--;
  let n = v.getUint16(e + 10, true), cd = v.getUint32(e + 16, true), zip64 = false;
  const u64 = i => v.getUint32(i, true) + v.getUint32(i + 4, true) * 4294967296;
  if (n === 0xFFFF) {
    const loc = e - 20;
    if (v.getUint32(loc, true) !== 0x07064b50) throw new Error('no Zip64 locator');
    const r = u64(loc + 8);
    if (v.getUint32(r, true) !== 0x06064b50) throw new Error('no Zip64 end record');
    n = u64(r + 32); cd = u64(r + 48); zip64 = true;
  }
  const out = [];
  for (let k = 0, i = cd; k < n; k++) {
    if (v.getUint32(i, true) !== 0x02014b50) throw new Error('bad central header');
    const method = v.getUint16(i + 10, true), crc = v.getUint32(i + 16, true);
    let csize = v.getUint32(i + 20, true), size = v.getUint32(i + 24, true);
    const nl = v.getUint16(i + 28, true), xl = v.getUint16(i + 30, true), cl = v.getUint16(i + 32, true);
    let off = v.getUint32(i + 42, true);
    const name = new TextDecoder().decode(buf.subarray(i + 46, i + 46 + nl));
    if (size === 0xFFFFFFFF) {   // (zip.js writes all three in the extra field then)
      const x = i + 46 + nl;
      if (v.getUint16(x, true) !== 1) throw new Error('no Zip64 extra');
      size = u64(x + 4); csize = u64(x + 12); off = u64(x + 20);
    }
    const lnl = v.getUint16(off + 26, true), lxl = v.getUint16(off + 28, true);
    const body = buf.subarray(off + 30 + lnl + lxl, off + 30 + lnl + lxl + csize);
    const data = method === 8 ? new Uint8Array(zlib.inflateRawSync(body)) : body;
    out.push({ name, method, crc, size, data });
    i += 46 + nl + xl + cl;
  }
  return { entries: out, zip64 };
}

const text = 'frame,x [nm]\n' + Array.from({ length: 400 }, (_, i) => `${i},${(i * 17.5).toFixed(3)}`).join('\n');
const bin = new Uint8Array(3000).map((_, i) => (i * 7919) & 255);
const entries = [{ name: 'gt/frames.csv', data: text }, { name: 'movie.tif', data: bin, store: true },
  { name: 'gt/größe µm.txt', data: 'ü' }, { name: 'empty.txt', data: '' }];
const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'isc-zip-'));
for (const z64 of [false, true]) {
  const blob = await Z.zip(entries, { zip64: z64 });
  const buf = new Uint8Array(await blob.arrayBuffer());
  const r = read(buf);
  const want = entries.map(e => typeof e.data === 'string' ? new TextEncoder().encode(e.data) : e.data);
  const same = r.entries.length === entries.length && r.entries.every((x, i) => x.name === entries[i].name &&
    x.size === want[i].length && x.data.length === want[i].length && x.data.every((b, j) => b === want[i][j]) &&
    x.crc === (zlib.crc32 ? zlib.crc32(want[i]) : Z.crc32(want[i])));
  check(same && r.zip64 === z64, `${z64 ? 'Zip64 records' : 'a plain zip'}: every entry back, sizes and CRC-32 right (${buf.length} bytes)`);
  check(r.entries[0].method === 8 && r.entries[1].method === 0, 'the text deflated, the stored entry stored');
  const f = path.join(dir, z64 ? 'z64.zip' : 'plain.zip');
  fs.writeFileSync(f, buf);
}
// Python's zipfile, if there is one: the archives as another reader sees them.
let py = null;
for (const exe of ['python3', 'python']) {
  try { execFileSync(exe, ['-c', 'import zipfile'], { stdio: 'pipe' }); py = exe; break; } catch (e) { /* next */ }
}
if (!py) console.log('skip  Python zipfile (no python)');
else {
  const script = 'import sys, zipfile\nsys.stdout.reconfigure(encoding="utf-8")\n' +
    'for f in sys.argv[1:]:\n' +
    '  z = zipfile.ZipFile(f)\n' +
    '  assert z.testzip() is None, f\n' +
    '  print(f, len(z.namelist()), z.read("gt/frames.csv")[:12].decode(), z.namelist()[2])\n';
  let out = '';
  try { out = execFileSync(py, ['-I', '-c', script, path.join(dir, 'plain.zip'), path.join(dir, 'z64.zip')], { encoding: 'utf8', env: { ...process.env, PYTHONIOENCODING: 'utf-8' } }); } catch (e) { out = String(e.stderr || e); }
  check(/plain\.zip 4 frame,x \[nm\] gt\/größe µm\.txt/.test(out) && /z64\.zip 4 frame,x \[nm\]/.test(out),
    `Python's zipfile reads both (testzip, the names, the bytes): ${out.trim().replace(/\s*\n\s*/g, ' | ').replace(/\S*isc-zip-\S*?[\\/]/g, '')}`);
}
fs.rmSync(dir, { recursive: true, force: true });
console.log(`zip unit: ${fail ? fail + ' FAILED' : 'all ok'}, ${((performance.now() - t0) / 1000).toFixed(2)} s`);
process.exit(fail ? 1 : 0);
