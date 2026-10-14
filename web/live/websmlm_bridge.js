// Copyright (c) 2026, inSiliScope contributors. BSD-3-Clause (see LICENSE).
//
// Runs inside the viewer site's copy of webSMLM (web/websmlm/webSMLM.html, tools/fetch_websmlm.mjs adds the
// <script> tag): takes the live microscope's frames from the viewer in any other tab of the same site, over a
// BroadcastChannel, and feeds them to webSMLM's own live-stream API (window.webSMLM.liveStream: a TIFF chunk in
// #liveStreamChunkInput, pushChunk(); the first chunk arms a session, end() ends it). Tabs on another site (webSMLM's
// own) cannot be reached this way: a browser has no channel between unrelated tabs of different origins.
//
// Messages (structured clones): viewer -> 'hello' | {type:'chunk', to, id, tiff, n, camera} | {type:'stop', to};
// this tab -> {type:'here', from, version, active} | {type:'ack', from, id, ok, error, totalFrames, totalLocs}.
(function () {
  'use strict';
  if (typeof BroadcastChannel === 'undefined') return;
  const ch = new BroadcastChannel('insiliscope-websmlm');
  const me = Math.random().toString(36).slice(2, 10);
  const version = () => { const m = (document.querySelector('.pill') || {}).textContent || ''; return (m.match(/v[\d.]+/) || [''])[0]; };
  const api = () => window.webSMLM && window.webSMLM.liveStream;
  let queue = Promise.resolve();

  // The viewer's camera, set when a session starts (webSMLM reads its sidebar once per session).
  function configure(cam) {
    if (!cam) return;
    const set = (id, v) => {
      const el = document.getElementById(id);
      if (!el || !(v > 0 || v === 0) || !isFinite(v)) return;
      el.value = String(v);
      el.dispatchEvent(new Event('input', { bubbles: true }));
      el.dispatchEvent(new Event('change', { bubbles: true }));
    };
    set('pxnm', cam.pixelNm);
    set('frametime', cam.frameSec);
    set('gain', cam.electronsPerAdu);
    set('camoffset', cam.offsetAdu);
    set('cameraExcessNoise', cam.emccd ? 2 : 1);
  }

  async function push(m) {
    const a = api();
    if (!a) throw new Error('webSMLM has not finished loading');
    if (!a.isActive()) configure(m.camera);
    const dt = new DataTransfer();
    dt.items.add(new File([m.tiff], 'insiliscope_live.tif', { type: 'image/tiff' }));
    document.getElementById('liveStreamChunkInput').files = dt.files;
    return a.pushChunk();
  }

  ch.onmessage = ev => {
    const m = ev.data || {};
    if (m === 'hello' || m.type === 'hello') {
      const a = api();
      ch.postMessage({ type: 'here', from: me, version: version(), active: !!(a && a.isActive()), visible: !document.hidden });
      return;
    }
    if (m.to !== me) return;
    // In order, one at a time: a stop must not end the session while a chunk is still being localized.
    queue = queue.then(async () => {
      if (m.type === 'stop') { const a = api(); if (a && a.isActive()) a.end(); return; }
      if (m.type !== 'chunk') return;
      try {
        const r = await push(m);
        ch.postMessage({ type: 'ack', from: me, id: m.id, ok: true, totalFrames: r && r.totalFrames, totalLocs: r && r.totalLocs });
      } catch (e) {
        ch.postMessage({ type: 'ack', from: me, id: m.id, ok: false, error: String(e && e.message || e) });
      }
    });
  };
  window.addEventListener('pagehide', () => ch.postMessage({ type: 'gone', from: me }));
  // Tell a viewer that is already streaming and waiting for a tab.
  ch.postMessage({ type: 'here', from: me, version: version(), active: false, visible: !document.hidden });
})();
