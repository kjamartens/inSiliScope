// SPDX-License-Identifier: BSD-3-Clause
// Video export sinks for the viewer (globalThis.IscEncode): MP4 (H.264) and WebM (VP9 / VP8) through the browser's
// WebCodecs VideoEncoder and the muxers in mp4.js / webm.js, and GIF through gif.js. Frames are handed in one by one
// (the caller renders frame i at t = i / fps), so the output never depends on how fast the frames were made.
//   const caps = await IscEncode.probe(width, height, fps);       // {mp4: {ok, codec, why}, webm: {...}, gif: {ok}}
//   const sink = await IscEncode.createSink({ format, width, height, fps, quality, gif: {width, fps, dither, keys} });
//   if (sink.wantsPalette) sink.palette(sampleCanvases);
//   for (...) await sink.addFrame(canvas);                          // a canvas of width x height
//   const blob = await sink.finish();                               // sink.ext, sink.mime
'use strict';
(function () {
const even = v => Math.max(2, v - (v & 1));
// H.264 levels: [level byte, max frame macroblocks, max macroblocks per second]
const AVC_LEVELS = [[0x1F, 3600, 108000], [0x20, 5120, 216000], [0x28, 8192, 245760], [0x2A, 8704, 522240], [0x33, 36864, 983040], [0x34, 36864, 2073600]];
function avcLevel(w, h, fps) {
  const mbs = Math.ceil(w / 16) * Math.ceil(h / 16);
  const L = AVC_LEVELS.find(([, fs, mbps]) => mbs <= fs && mbs * fps <= mbps);
  return L ? L[0] : 0x34;
}
const hex2 = v => v.toString(16).toUpperCase().padStart(2, '0');
const BPP = { low: 0.05, medium: 0.1, high: 0.18 };
function videoConfigs(format, w, h, fps, quality) {
  const bitrate = Math.round((BPP[quality] || BPP.high) * w * h * fps);
  const base = { width: w, height: h, framerate: fps, bitrate, bitrateMode: 'variable', latencyMode: 'quality' };
  if (format === 'mp4') {
    const lv = hex2(avcLevel(w, h, fps));
    // High, Main, Constrained Baseline (Chrome's software encoder does Baseline only)
    return ['6400', '4D40', '42E0'].map(p => Object.assign({ codec: 'avc1.' + p + lv, avc: { format: 'avc' } }, base));
  }
  const vp9lv = w * h <= 1280 * 720 ? '31' : w * h <= 2048 * 1088 ? '40' : '51';
  return [Object.assign({ codec: 'vp09.00.' + vp9lv + '.08' }, base), Object.assign({ codec: 'vp8' }, base)];
}
async function pickConfig(format, w, h, fps, quality) {
  if (typeof VideoEncoder === 'undefined') return { ok: false, why: 'this browser has no WebCodecs video encoder' };
  for (const hw of ['prefer-hardware', 'prefer-software']) for (const c of videoConfigs(format, w, h, fps, quality)) {
    const cfg = Object.assign({ hardwareAcceleration: hw }, c);
    try { const r = await VideoEncoder.isConfigSupported(cfg); if (r && r.supported) return { ok: true, config: r.config || cfg, codec: cfg.codec }; }
    catch (e) { /* try the next */ }
  }
  return { ok: false, why: format === 'mp4' ? 'this browser cannot encode H.264' : 'this browser cannot encode VP9 or VP8' };
}
async function probe(w, h, fps) {
  w = even(w); h = even(h);
  const [mp4, webm] = await Promise.all([pickConfig('mp4', w, h, fps, 'high'), pickConfig('webm', w, h, fps, 'high')]);
  return { mp4: { ok: mp4.ok, codec: mp4.codec, why: mp4.why }, webm: { ok: webm.ok, codec: webm.codec, why: webm.why }, gif: { ok: true } };
}

function videoSink(format, o, picked) {
  const W = even(o.width), H = even(o.height), fps = o.fps, samples = [];
  let desc = null, error = null, n = 0;
  const enc = new VideoEncoder({
    output(chunk, meta) {
      const data = new Uint8Array(chunk.byteLength);
      chunk.copyTo(data);
      if (meta && meta.decoderConfig && meta.decoderConfig.description && !desc) desc = new Uint8Array(meta.decoderConfig.description.buffer || meta.decoderConfig.description).slice();
      samples.push({ data, key: chunk.type === 'key', pts: chunk.timestamp });
    },
    error(e) { error = e; },
  });
  enc.configure(picked.config);
  const keyEvery = Math.max(1, Math.round(2 * fps));
  return {
    ext: format === 'mp4' ? 'mp4' : 'webm', mime: format === 'mp4' ? 'video/mp4' : 'video/webm', codec: picked.codec, wantsPalette: false,
    async addFrame(canvas) {
      if (error) throw error;
      const ts = Math.round(n * 1e6 / fps), f = new VideoFrame(canvas, { timestamp: ts, duration: Math.round(1e6 / fps) });
      enc.encode(f, { keyFrame: n % keyEvery === 0 });
      f.close(); n++;
      while (enc.encodeQueueSize > 2) await new Promise(r => setTimeout(r, 0));
    },
    async finish() {
      await enc.flush();
      enc.close();
      if (error) throw error;
      let bytes;
      if (format === 'mp4') {
        let avcC = desc;
        if (!avcC) {   // an Annex B stream: build the record from the first keyframe, convert the samples
          avcC = self.IscMp4.avcCFromAnnexB(samples[0].data);
          for (const s of samples) s.data = self.IscMp4.annexBToAvcc(s.data);
        }
        bytes = self.IscMp4.mux({ width: W, height: H, fps, avcC, samples });
      } else bytes = self.IscWebm.mux({ width: W, height: H, fps, codec: /^vp8/.test(picked.codec) ? 'V_VP8' : 'V_VP9', samples });
      return new Blob([bytes], { type: this.mime });
    },
    abort() { try { enc.close(); } catch (e) { /* closed */ } },
  };
}
function gifSink(o) {
  const g = o.gif || {}, W = g.width || o.width, H = g.height || o.height;
  const cnv = typeof OffscreenCanvas !== 'undefined' ? new OffscreenCanvas(W, H) : Object.assign(document.createElement('canvas'), { width: W, height: H });
  const c2 = cnv.getContext('2d', { willReadFrequently: true });
  c2.imageSmoothingQuality = 'high';
  const enc = self.IscGif.encoder({ width: W, height: H, fps: g.fps || o.fps, dither: g.dither || 'bayer4' });
  const grab = canvas => { c2.clearRect(0, 0, W, H); c2.drawImage(canvas, 0, 0, W, H); return c2.getImageData(0, 0, W, H).data; };
  return {
    ext: 'gif', mime: 'image/gif', codec: 'gif', wantsPalette: true, width: W, height: H,
    palette(canvases) { enc.setPalette(self.IscGif.buildPalette(canvases.map(grab), g.keys || [])); },
    async addFrame(canvas) { enc.addFrame(grab(canvas)); },
    async finish() { return new Blob([enc.finish()], { type: 'image/gif' }); },
    abort() {},
  };
}
// format 'mp4' | 'webm' | 'gif'; falls back mp4 -> webm -> gif when the browser cannot encode it (sink.fellBack says why).
async function createSink(o) {
  const order = o.format === 'gif' ? ['gif'] : o.format === 'webm' ? ['webm', 'gif'] : ['mp4', 'webm', 'gif'];
  const why = [];
  for (const f of order) {
    if (f === 'gif') { const s = gifSink(o); s.fellBack = why.join('; '); return s; }
    const p = await pickConfig(f, even(o.width), even(o.height), o.fps, o.quality);
    if (p.ok) { const s = videoSink(f, o, p); s.fellBack = why.join('; '); return s; }
    why.push(p.why);
  }
}
function download(blob, name) {
  const a = document.createElement('a');
  a.href = URL.createObjectURL(blob); a.download = name;
  document.body.append(a); a.click(); a.remove();
  setTimeout(() => URL.revokeObjectURL(a.href), 20000);
}

globalThis.IscEncode = { probe, createSink, download, avcLevel, videoConfigs, even };
})();
