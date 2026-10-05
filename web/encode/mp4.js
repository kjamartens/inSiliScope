// SPDX-License-Identifier: BSD-3-Clause
// A minimal MP4 muxer for one H.264 video track (no dependencies; globalThis.IscMp4): ftyp, moov (before mdat, so a
// player can start at once), mdat. Samples come from WebCodecs' VideoEncoder (avc format: length-prefixed NAL units;
// avcC = the decoder config's description), in decode order.
//   const bytes = IscMp4.mux({ width, height, fps, avcC, samples: [{ data, key, pts }] });   // pts in us, optional
'use strict';
(function () {
const TS = 90000;   // track timescale: integer frame durations at every common fps

const str4 = s => [s.charCodeAt(0), s.charCodeAt(1), s.charCodeAt(2), s.charCodeAt(3)];
const u32 = v => [(v >>> 24) & 255, (v >>> 16) & 255, (v >>> 8) & 255, v & 255];
const u16 = v => [(v >>> 8) & 255, v & 255];
// box(type, ...parts): parts are byte arrays or Uint8Arrays
function box(type, ...parts) {
  let n = 8;
  for (const p of parts) n += p.length;
  const out = new Uint8Array(n);
  out.set(u32(n), 0); out.set(str4(type), 4);
  let o = 8;
  for (const p of parts) { out.set(p, o); o += p.length; }
  return out;
}
const full = (type, version, flags, ...parts) => box(type, [version, (flags >> 16) & 255, (flags >> 8) & 255, flags & 255], ...parts);
const MATRIX = [0x00010000, 0, 0, 0, 0x00010000, 0, 0, 0, 0x40000000].flatMap(u32);

function mux(o) {
  const N = o.samples.length, delta = Math.round(TS / o.fps), durTrack = N * delta, durMovie = Math.round(N * 1000 / o.fps);
  // composition offsets when the encoder reordered frames (B-frames): pts - dts, shifted to be >= 0
  let ctts = null, shift = 0;
  if (o.samples.some(s => s.pts !== undefined)) {
    const off = o.samples.map((s, i) => Math.round((s.pts || 0) * TS / 1e6) - i * delta);
    if (off.some(v => v !== 0)) { shift = Math.max(0, -Math.min(...off)); ctts = off.map(v => v + shift); }
  }
  const avc1 = box('avc1', [0, 0, 0, 0, 0, 0], u16(1), new Array(16).fill(0), u16(o.width), u16(o.height),
    u32(0x00480000), u32(0x00480000), u32(0), u16(1), new Array(32).fill(0), u16(0x0018), u16(0xFFFF),
    box('avcC', o.avcC));
  const stbl = (offset) => box('stbl',
    full('stsd', 0, 0, u32(1), avc1),
    full('stts', 0, 0, u32(1), u32(N), u32(delta)),
    ...(ctts ? [full('ctts', 0, 0, u32(N), ctts.flatMap(v => [...u32(1), ...u32(v)]))] : []),
    full('stss', 0, 0, u32(o.samples.filter(s => s.key).length), o.samples.flatMap((s, i) => (s.key ? u32(i + 1) : []))),
    full('stsc', 0, 0, u32(1), u32(1), u32(N), u32(1)),
    full('stsz', 0, 0, u32(0), u32(N), o.samples.flatMap(s => u32(s.data.length))),
    full('stco', 0, 0, u32(1), u32(offset)));
  const moov = offset => box('moov',
    full('mvhd', 0, 0, u32(0), u32(0), u32(1000), u32(durMovie), u32(0x00010000), u16(0x0100), new Array(10).fill(0), MATRIX,
      new Array(24).fill(0), u32(2)),
    box('trak',
      full('tkhd', 0, 3, u32(0), u32(0), u32(1), u32(0), u32(durMovie), new Array(8).fill(0), u16(0), u16(0), u16(0), u16(0),
        MATRIX, u32(o.width << 16), u32(o.height << 16)),
      ...(shift ? [box('edts', full('elst', 0, 0, u32(1), u32(durMovie), u32(shift), u16(1), u16(0)))] : []),
      box('mdia',
        full('mdhd', 0, 0, u32(0), u32(0), u32(TS), u32(durTrack), u16(0x55C4), u16(0)),
        full('hdlr', 0, 0, u32(0), str4('vide'), new Array(12).fill(0), [...'VideoHandler'].map(c => c.charCodeAt(0)), [0]),
        box('minf',
          full('vmhd', 0, 1, u16(0), u16(0), u16(0), u16(0)),
          box('dinf', full('dref', 0, 0, u32(1), full('url ', 0, 1))),
          stbl(offset)))));
  const ftyp = box('ftyp', str4('isom'), u32(0x200), str4('isom'), str4('iso2'), str4('avc1'), str4('mp41'));
  const size = o.samples.reduce((s, x) => s + x.data.length, 0);
  const moovLen = moov(0).length;   // its length does not depend on the offset
  const first = ftyp.length + moovLen + 8;
  const out = new Uint8Array(first + size);
  out.set(ftyp, 0); out.set(moov(first), ftyp.length);
  out.set(u32(8 + size), ftyp.length + moovLen); out.set(str4('mdat'), ftyp.length + moovLen + 4);
  let p = first;
  for (const s of o.samples) { out.set(s.data, p); p += s.data.length; }
  return out;
}

// Annex B (start codes) -> length-prefixed NAL units, and an avcC record from its SPS/PPS (if an encoder gives
// Annex B without a description).
function splitAnnexB(b) {
  const nals = [];
  let i = 0, start = -1;
  while (i + 3 <= b.length) {
    const sc = b[i] === 0 && b[i + 1] === 0 && (b[i + 2] === 1 || (b[i + 2] === 0 && b[i + 3] === 1));
    if (sc) { if (start >= 0) nals.push(b.subarray(start, i)); i += b[i + 2] === 1 ? 3 : 4; start = i; } else i++;
  }
  if (start >= 0) nals.push(b.subarray(start));
  return nals;
}
function annexBToAvcc(b) {
  const nals = splitAnnexB(b).filter(n => n.length && (n[0] & 31) !== 7 && (n[0] & 31) !== 8);
  const out = new Uint8Array(nals.reduce((s, n) => s + 4 + n.length, 0));
  let o = 0;
  for (const n of nals) { out.set(u32(n.length), o); out.set(n, o + 4); o += 4 + n.length; }
  return out;
}
function avcCFromAnnexB(b) {
  const nals = splitAnnexB(b), sps = nals.find(n => (n[0] & 31) === 7), pps = nals.find(n => (n[0] & 31) === 8);
  if (!sps || !pps) return null;
  return Uint8Array.from([1, sps[1], sps[2], sps[3], 0xFF, 0xE1, ...u16(sps.length), ...sps, 1, ...u16(pps.length), ...pps]);
}

globalThis.IscMp4 = { mux, box, annexBToAvcc, avcCFromAnnexB, TS };
})();
