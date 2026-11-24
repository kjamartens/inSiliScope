// SPDX-License-Identifier: BSD-3-Clause
// An engine worker's link to the remote server (docs/remote.md): its define() runs in the workers' Blob script
// (iscRemoteProxyDefine, beside iscRemoteWireDefine), and genWorkerMain hands it the jobs it takes. The server's replies
// are the messages the worker's own engine would post (cli/remote_server.cpp), so the page cannot tell them apart; what
// the server cannot do -- write the browser's disk storage (OPFS) -- happens here: a Record's frames and a ground
// truth's files arrive as they are made and are written at their place, as the local engine writes them.
// Before the server answers, jobs wait; if it cannot be reached (or it is another version) they run here, on the
// worker's own engine, and so does everything after the connection is lost. The page hears of each through the
// BroadcastChannel 'isc-remote' ({state: connected | failed | lost, worker, info | reason}).
(function () {
  function define() {
    // The jobs the server takes: everything heavy, or with scope 'records' only movies and ground truth (live stays
    // here). 'options' stays here (the controls the page has are this module's).
    const ALL = ['live', 'live-replay', 'movie', 'setup', 'panel', 'gt', 'gt-run', 'gt-files'];
    const RECORDS = ['movie', 'gt', 'gt-run', 'gt-files'];
    const STREAMS = new Set(['movie', 'gt', 'gt-run', 'live']);

    async function diskOpen(path) {
      let dir = await navigator.storage.getDirectory();
      const parts = path.split('/');
      for (const p of parts.slice(0, -1)) dir = await dir.getDirectoryHandle(p, { create: true });
      const fh = await dir.getFileHandle(parts[parts.length - 1], { create: true });
      return fh.createSyncAccessHandle();
    }
    function diskWrite(h, view, at) {
      const k = h.write(view, { at });
      if (k !== view.byteLength) throw new Error(`${k} of ${view.byteLength} bytes written`);
    }
    const refused = e => (e && e.name === 'QuotaExceededError' ? 'full' : (e && e.message) || String(e));

    // cfg: {url, scope, name, expect: {protocol, abi, world, version}}; post(msg, transfer): to the page; local(d): the
    // job on this worker's own engine.
    function create(cfg, post, local) {
      const W = self.IscWire;
      const types = new Set(cfg.scope === 'records' ? RECORDS : ALL);
      const bc = typeof BroadcastChannel !== 'undefined' ? new BroadcastChannel('isc-remote') : null;
      const say = (state, extra) => { if (bc) bc.postMessage({ state, worker: cfg.name || '', ...(extra || {}) }); };
      let state = 'connecting', ws = null, chain = Promise.resolve();
      const waiting = [], files = new Map(), inflight = new Map();
      const keyOf = (type, id) => type + ':' + id;

      try {
        ws = new WebSocket(cfg.url);
        ws.binaryType = 'arraybuffer';
      } catch (e) { setTimeout(() => fail('the address is not valid: ' + (e.message || e)), 0); }
      const timer = setTimeout(() => { if (state === 'connecting') fail('the server did not answer'); }, 15000);
      if (ws) {
        ws.onopen = () => ws.send(W.encode({ type: 'hello', id: 0, protocol: cfg.expect.protocol, version: cfg.expect.version }));
        ws.onmessage = e => {
          let d;
          try { d = W.decode(e.data); } catch (err) { console.warn('remote: a message it cannot read', err); return; }
          if (state === 'connecting' && d.type === 'hello') { onHello(d); return; }
          onReply(d);
        };
        ws.onerror = () => { if (state === 'connecting') fail('cannot reach the server'); };
        ws.onclose = () => { if (state === 'connecting') fail('cannot reach the server'); else if (state === 'open') lost(); };
      }

      function onHello(d) {
        clearTimeout(timer);
        const x = cfg.expect || {};
        const why = d.protocol !== x.protocol ? `it speaks protocol ${d.protocol}, this page ${x.protocol}`
          : x.abi && d.abi !== x.abi ? `its core is ABI ${d.abi}, this page's ${x.abi}`
          : x.world && d.world !== x.world ? `its world version is ${d.world}, this page's ${x.world}` : '';
        if (why) { fail('another version: ' + why + ' (it serves ' + d.version + ')'); return; }
        state = 'open';
        say('connected', { info: d });
        for (const [name, bytes] of files) ws.send(W.encode({ type: 'bfp-file', name, bytes }));
        for (const d of waiting.splice(0)) send(d);
      }
      function fail(reason) {
        if (state !== 'connecting') return;
        state = 'failed';
        clearTimeout(timer);
        try { if (ws) ws.close(); } catch (e) { /* closed */ }
        say('failed', { reason });
        for (const d of waiting.splice(0)) local(d);
      }
      function lost() {
        state = 'lost';
        say('lost', { reason: 'the connection to the server closed' });
        for (const c of inflight.values()) {
          close(c);
          post({ type: c.d.type, id: c.d.id, error: 'the remote server disconnected (the next job runs in this browser)' });
        }
        inflight.clear();
      }
      function close(c) {
        if (!c.h) return;
        try { c.h.flush(); c.h.close(); } catch (e) { /* closed */ }
        c.h = null;
      }

      // A job to the server (in order: a job that opens its disk file first waits for it, and so do the ones after).
      function send(d) {
        chain = chain.then(async () => {
          if (state !== 'open') { local(d); return; }
          const c = { d, h: null, diskError: d.diskError || '', px: 0 };
          if (d.disk && !d.diskOpen && !d.diskError) {
            try { c.h = await diskOpen(d.disk.path); } catch (e) { c.diskError = String((e && e.message) || e || 'no disk storage'); }
          }
          const job = { ...d };
          delete job.disk; delete job.diskOpen; delete job.diskError;
          // a Record written to the disk storage: its strips' rows when it needs strips (a side past 4096 px)
          if (c.h && d.type === 'movie' && d.stream) job.rowsOut = true;
          if (!d.id && d.id !== 0) { ws.send(W.encode(job)); return; }
          inflight.set(keyOf(d.type, d.id), c);
          ws.send(W.encode(job));
        });
      }

      function streamCtx(id) {
        for (const c of inflight.values()) if (c.d.id === id && STREAMS.has(c.d.type)) return c;
        return null;
      }
      function onReply(d) {
        if (d.type === 'movie-part' || d.type === 'movie-rows') {
          const c = streamCtx(d.id);
          if (c && c.h) {
            try {
              if (d.type === 'movie-part') {
                c.px = d.frames.length / Math.max(1, d.n);
                diskWrite(c.h, d.frames, d.first * c.px * 2);
              } else {
                c.px = d.w * d.h;
                const k = d.rows * d.w;
                for (let i = 0; i < d.n; i++) diskWrite(c.h, d.frames.subarray(i * k, (i + 1) * k), ((d.first + i) * d.h + d.y0) * d.w * 2);
              }
            } catch (e) {
              c.diskError = `the disk storage refused frame ${d.first + 1} (${refused(e)})`;
              close(c);   // (the rest of the movie is lost: the page shows what it has, as when the local engine stops)
            }
            return;
          }
          post(d, W.transfers(d));
          return;
        }
        if (d.type === 'movie-progress') { post(d); return; }
        const k = keyOf(d.type, d.id), c = inflight.get(k);
        inflight.delete(k);
        if (c) finish(c, d);
        post(d, W.transfers(d));
      }
      // The disk storage's part of a reply, as the local engine makes it.
      function finish(c, d) {
        if (c.diskError && !d.diskError) d.diskError = c.diskError;
        if (!c.h || d.error) { close(c); return; }
        try {
          if (d.type === 'movie' && d.streamed) {
            d.disk = true;
            if (d.got && c.px && c.px <= 16777216) { d.last = new Uint16Array(c.px); c.h.read(d.last, { at: (d.got - 1) * c.px * 2 }); }
          } else if (d.type === 'live' && d.frames) {
            const fpx = d.w * d.h;
            try {
              diskWrite(c.h, d.frames, (c.d.disk.first || 0) * fpx * 2);
              d.frames = d.frames.slice(d.frames.length - fpx);
              d.disk = true;
            } catch (e) { d.diskError = `the disk storage refused the frames (${refused(e)})`; }
          } else if (d.files) {
            let at = 0;
            d.files = d.files.map(([name, bytes]) => {
              if (!(bytes instanceof Uint8Array) || d.diskError) return [name, bytes];
              try { diskWrite(c.h, bytes, at); const r = [name, { at, size: bytes.length }]; at += bytes.length; return r; }
              catch (e) { d.diskError = `the disk storage refused ${name} (${refused(e)})`; return [name, bytes]; }
            });
            d.spool = c.d.disk.path;
          }
        } finally { close(c); }
      }

      return {
        state: () => state,
        takes: d => types.has(d.type) && (state === 'connecting' || state === 'open'),
        run: d => { if (state === 'connecting') waiting.push(d); else send(d); },
        bfpFile: d => {
          files.set(d.name, d.bytes);
          if (state === 'open') ws.send(W.encode({ type: 'bfp-file', name: d.name, bytes: d.bytes }));
        },
      };
    }
    self.IscRemoteProxy = { create };
  }
  self.iscRemoteProxyDefine = define;
  define();
})();
