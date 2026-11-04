// SPDX-License-Identifier: BSD-3-Clause
// A zip writer for the viewer's Save (the movie, its settings file and its ground truth in one file;
// docs/ground-truth.md). Page-free (also under Node: self.iscZip): entries deflated with the browser's
// CompressionStream('deflate-raw') (stored when that is not smaller, or with store: true, or without CompressionStream),
// CRC-32, UTF-8 names, Zip64 records when a size or offset passes 4 GB or there are more than 65535 entries (zip64:
// true forces them, for the tests).
//   await iscZip.zip([{ name: 'a.txt', data: 'text' | Uint8Array, store?: bool }], { zip64?: bool }) -> Blob
'use strict';
(function (self) {
  const CRC = (() => {
    const t = new Uint32Array(256);
    for (let n = 0; n < 256; n++) {
      let c = n;
      for (let k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1;
      t[n] = c >>> 0;
    }
    return t;
  })();
  function crc32(b) {
    let c = 0xFFFFFFFF;
    for (let i = 0; i < b.length; i++) c = CRC[(c ^ b[i]) & 255] ^ (c >>> 8);
    return (c ^ 0xFFFFFFFF) >>> 0;
  }
  async function deflate(b) {
    if (typeof CompressionStream !== 'function') return null;
    const cs = new CompressionStream('deflate-raw');
    const out = new Response(new Blob([b]).stream().pipeThrough(cs)).arrayBuffer();
    return new Uint8Array(await out);
  }
  // DOS date and time of now.
  function dosTime(d) {
    return { time: (d.getHours() << 11) | (d.getMinutes() << 5) | (d.getSeconds() >> 1),
      date: (Math.max(0, d.getFullYear() - 1980) << 9) | ((d.getMonth() + 1) << 5) | d.getDate() };
  }
  // A little-endian record writer.
  function rec(n) {
    const b = new Uint8Array(n), v = new DataView(b.buffer);
    let i = 0;
    const w = {
      u16(x) { v.setUint16(i, x, true); i += 2; return w; },
      u32(x) { v.setUint32(i, x >>> 0, true); i += 4; return w; },
      u64(x) { v.setUint32(i, x % 4294967296, true); v.setUint32(i + 4, Math.floor(x / 4294967296), true); i += 8; return w; },
      bytes(x) { b.set(x, i); i += x.length; return w; },
      out: b,
    };
    return w;
  }
  const BIG = 0xFFFFFFFF;

  async function zip(entries, opt = {}) {
    const enc = new TextEncoder(), when = dosTime(new Date());
    const parts = [], central = [];
    let offset = 0;
    let force64 = !!opt.zip64 || entries.length > 65535;
    for (const e of entries) {
      const data = typeof e.data === 'string' ? enc.encode(e.data) : e.data instanceof Uint8Array ? e.data : new Uint8Array(e.data);
      const name = enc.encode(e.name);
      const crc = crc32(data);
      let body = data, method = 0;
      if (!e.store && data.length > 64) {
        const z = await deflate(data);
        if (z && z.length < data.length) { body = z; method = 8; }
      }
      const z64 = force64 || data.length >= BIG || body.length >= BIG || offset >= BIG;
      // local header (its Zip64 extra: both sizes)
      const ext = z64 ? rec(20).u16(1).u16(16).u64(data.length).u64(body.length).out : new Uint8Array(0);
      const local = rec(30 + name.length + ext.length)
        .u32(0x04034b50).u16(z64 ? 45 : 20).u16(0x0800).u16(method).u16(when.time).u16(when.date).u32(crc)
        .u32(z64 ? BIG : body.length).u32(z64 ? BIG : data.length).u16(name.length).u16(ext.length).bytes(name).bytes(ext).out;
      parts.push(local, body);
      central.push({ name, crc, method, size: data.length, csize: body.length, offset, z64 });
      offset += local.length + body.length;
    }
    const cdStart = offset;
    for (const c of central) {
      const ext = c.z64 ? rec(28).u16(1).u16(24).u64(c.size).u64(c.csize).u64(c.offset).out : new Uint8Array(0);
      const h = rec(46 + c.name.length + ext.length)
        .u32(0x02014b50).u16(c.z64 ? 45 : 20).u16(c.z64 ? 45 : 20).u16(0x0800).u16(c.method).u16(when.time).u16(when.date)
        .u32(c.crc).u32(c.z64 ? BIG : c.csize).u32(c.z64 ? BIG : c.size).u16(c.name.length).u16(ext.length).u16(0)
        .u16(0).u16(0).u32(0).u32(c.z64 ? BIG : c.offset).bytes(c.name).bytes(ext).out;
      parts.push(h);
      offset += h.length;
    }
    const cdSize = offset - cdStart, n = central.length;
    const end64 = force64 || central.some(c => c.z64) || cdStart >= BIG || cdSize >= BIG;
    if (end64) {
      parts.push(rec(56).u32(0x06064b50).u64(44).u16(45).u16(45).u32(0).u32(0).u64(n).u64(n).u64(cdSize).u64(cdStart).out);
      parts.push(rec(20).u32(0x07064b50).u32(0).u64(offset).u32(1).out);
    }
    parts.push(rec(22).u32(0x06054b50).u16(0).u16(0).u16(end64 ? 0xFFFF : n).u16(end64 ? 0xFFFF : n)
      .u32(end64 ? BIG : cdSize).u32(end64 ? BIG : cdStart).u16(0).out);
    return new Blob(parts, { type: 'application/zip' });
  }
  self.iscZip = { zip, crc32 };
})(typeof self !== 'undefined' ? self : globalThis);
