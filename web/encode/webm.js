// SPDX-License-Identifier: BSD-3-Clause
// A minimal WebM muxer for one VP9/VP8 video track (no dependencies; globalThis.IscWebm). The whole file is built at
// the end, so every size is known: EBML header, Segment { Info, Tracks, Cues, Clusters }; a new cluster starts at
// every keyframe, the cues (before the clusters, fixed-width positions) point at them, so players seek at once.
//   const bytes = IscWebm.mux({ width, height, fps, codec: 'V_VP9', samples: [{ data, key }] });
'use strict';
(function () {
// element ids as bytes
const ID = {
  EBML: [0x1A, 0x45, 0xDF, 0xA3], EBMLVersion: [0x42, 0x86], EBMLReadVersion: [0x42, 0xF7], EBMLMaxIDLength: [0x42, 0xF2],
  EBMLMaxSizeLength: [0x42, 0xF3], DocType: [0x42, 0x82], DocTypeVersion: [0x42, 0x87], DocTypeReadVersion: [0x42, 0x85],
  Segment: [0x18, 0x53, 0x80, 0x67], Info: [0x15, 0x49, 0xA9, 0x66], TimecodeScale: [0x2A, 0xD7, 0xB1], Duration: [0x44, 0x89],
  MuxingApp: [0x4D, 0x80], WritingApp: [0x57, 0x41], Tracks: [0x16, 0x54, 0xAE, 0x6B], TrackEntry: [0xAE], TrackNumber: [0xD7],
  TrackUID: [0x73, 0xC5], TrackType: [0x83], FlagLacing: [0x9C], CodecID: [0x86], DefaultDuration: [0x23, 0xE3, 0x83],
  Video: [0xE0], PixelWidth: [0xB0], PixelHeight: [0xBA], Cues: [0x1C, 0x53, 0xBB, 0x6B], CuePoint: [0xBB], CueTime: [0xB3],
  CueTrackPositions: [0xB7], CueTrack: [0xF7], CueClusterPosition: [0xF1], Cluster: [0x1F, 0x43, 0xB6, 0x75], Timecode: [0xE7],
  SimpleBlock: [0xA3],
};
// size as an EBML variable-length integer (the shortest form, or `width` bytes)
function vsize(n, width) {
  let w = width || 1;
  while (!width && w < 8 && n >= Math.pow(2, 7 * w) - 1) w++;
  const out = new Array(w);
  let v = n;
  for (let i = w - 1; i >= 0; i--) { out[i] = v % 256; v = Math.floor(v / 256); }
  out[0] |= 0x80 >> (w - 1);
  return out;
}
const uint = (v, w) => { const out = []; if (w) { for (let i = w - 1; i >= 0; i--) out.push(Math.floor(v / Math.pow(256, i)) % 256); return out; }
  do { out.unshift(v % 256); v = Math.floor(v / 256); } while (v > 0); return out; };
const f64 = v => { const b = new Uint8Array(8); new DataView(b.buffer).setFloat64(0, v); return [...b]; };
const text = s => [...s].map(c => c.charCodeAt(0));
function concat(parts) {
  let n = 0;
  for (const p of parts) n += p.length;
  const out = new Uint8Array(n);
  let o = 0;
  for (const p of parts) { out.set(p, o); o += p.length; }
  return out;
}
const el = (id, ...payload) => { const body = concat(payload.map(p => (p instanceof Uint8Array ? p : Uint8Array.from(p)))); return concat([Uint8Array.from(id), Uint8Array.from(vsize(body.length)), body]); };

function mux(o) {
  const N = o.samples.length, msPer = 1000 / o.fps;
  const header = el(ID.EBML, el(ID.EBMLVersion, [1]), el(ID.EBMLReadVersion, [1]), el(ID.EBMLMaxIDLength, [4]), el(ID.EBMLMaxSizeLength, [8]),
    el(ID.DocType, text('webm')), el(ID.DocTypeVersion, [4]), el(ID.DocTypeReadVersion, [2]));
  const info = el(ID.Info, el(ID.TimecodeScale, uint(1000000)), el(ID.Duration, f64(N * msPer)),
    el(ID.MuxingApp, text('insiliscope viewer')), el(ID.WritingApp, text('insiliscope viewer')));
  const tracks = el(ID.Tracks, el(ID.TrackEntry, el(ID.TrackNumber, [1]), el(ID.TrackUID, [1]), el(ID.TrackType, [1]), el(ID.FlagLacing, [0]),
    el(ID.CodecID, text(o.codec || 'V_VP9')), el(ID.DefaultDuration, uint(Math.round(1e9 / o.fps))),
    el(ID.Video, el(ID.PixelWidth, uint(o.width)), el(ID.PixelHeight, uint(o.height)))));
  // clusters: one per keyframe run (a frame time fits int16 ms within a cluster at a keyframe every few seconds;
  // longer runs are split)
  const clusters = [];
  let cur = null;
  o.samples.forEach((s, i) => {
    const t = Math.round(i * msPer);
    if (!cur || s.key || t - cur.t0 > 30000) { cur = { t0: t, blocks: [] }; clusters.push(cur); }
    const rel = t - cur.t0;
    cur.blocks.push(el(ID.SimpleBlock, [0x81, (rel >> 8) & 255, rel & 255, s.key ? 0x80 : 0], s.data));
  });
  const clusterBytes = clusters.map(c => el(ID.Cluster, el(ID.Timecode, uint(c.t0)), ...c.blocks));
  // cues: fixed 8-byte positions, so their size does not depend on the positions
  const cuesFor = pos => el(ID.Cues, ...clusters.map((c, i) => el(ID.CuePoint, el(ID.CueTime, uint(c.t0)),
    el(ID.CueTrackPositions, el(ID.CueTrack, [1]), el(ID.CueClusterPosition, uint(pos[i], 8))))));
  const cuesLen = cuesFor(clusters.map(() => 0)).length;
  const pos = [];
  let p = info.length + tracks.length + cuesLen;
  for (const cb of clusterBytes) { pos.push(p); p += cb.length; }
  const body = concat([info, tracks, cuesFor(pos), ...clusterBytes]);
  return concat([header, Uint8Array.from(ID.Segment), Uint8Array.from(vsize(body.length, 8)), body]);
}

globalThis.IscWebm = { mux, vsize, ID };
})();
