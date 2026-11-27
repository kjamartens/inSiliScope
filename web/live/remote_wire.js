// SPDX-License-Identifier: BSD-3-Clause
// The remote server's messages (docs/remote.md; the other end is cli/remote_server.cpp): u32 LE n, n bytes of JSON, then
// binary blobs, each starting on a multiple of 8 bytes from the message's start. The JSON's "$blobs" lists the blobs'
// sizes; a typed array (or an ArrayBuffer) in the message is {"$b": index, "$t": its type}. Page-free: the page, the
// engine workers (its define() runs in their Blob script, iscRemoteWireDefine) and Node (tests/remote/) load it.
(function () {
  function define() {
    const TYPES = { Int8Array, Uint8Array, Uint8ClampedArray, Int16Array, Uint16Array, Int32Array, Uint32Array,
      Float32Array, Float64Array };
    const align8 = n => (n + 7) & ~7;
    // msg (a plain object: numbers, strings, arrays, objects, typed arrays) -> ArrayBuffer. undefined and functions are
    // left out, a non-finite number is null (as JSON.stringify does).
    function encode(msg) {
      const blobs = [];
      const conv = v => {
        if (v instanceof ArrayBuffer) { blobs.push(new Uint8Array(v)); return { $b: blobs.length - 1, $t: 'ArrayBuffer' }; }
        if (ArrayBuffer.isView(v)) {
          blobs.push(new Uint8Array(v.buffer, v.byteOffset, v.byteLength));
          return { $b: blobs.length - 1, $t: v.constructor.name };
        }
        if (Array.isArray(v)) return v.map(x => (x === undefined || typeof x === 'function' ? null : conv(x)));
        if (v && typeof v === 'object') {
          const o = {};
          for (const k of Object.keys(v)) {
            const x = v[k];
            if (x !== undefined && typeof x !== 'function') o[k] = conv(x);
          }
          return o;
        }
        return typeof v === 'number' && !isFinite(v) ? null : v;
      };
      const body = { $blobs: null, ...conv(msg) };
      body.$blobs = blobs.map(b => b.byteLength);
      const json = new TextEncoder().encode(JSON.stringify(body));
      let size = align8(4 + json.length);
      for (const b of blobs) size = align8(size + b.byteLength);
      const out = new Uint8Array(size);
      new DataView(out.buffer).setUint32(0, json.length, true);
      out.set(json, 4);
      let at = align8(4 + json.length);
      for (const b of blobs) { out.set(b, at); at = align8(at + b.byteLength); }
      return out.buffer;
    }
    // ArrayBuffer -> the message, each typed array a copy of its own (so a page can transfer one without the others).
    function decode(buf) {
      const jl = new DataView(buf).getUint32(0, true);
      const body = JSON.parse(new TextDecoder().decode(new Uint8Array(buf, 4, jl)));
      const offs = [];
      let at = align8(4 + jl);
      for (const n of body.$blobs || []) { offs.push([at, n]); at = align8(at + n); }
      delete body.$blobs;
      const conv = v => {
        if (Array.isArray(v)) return v.map(conv);
        if (v && typeof v === 'object') {
          if (typeof v.$b === 'number' && offs[v.$b]) {
            const [o, n] = offs[v.$b], copy = buf.slice(o, o + n);
            return v.$t === 'ArrayBuffer' ? copy : new (TYPES[v.$t] || Uint8Array)(copy);
          }
          for (const k of Object.keys(v)) v[k] = conv(v[k]);
        }
        return v;
      };
      return conv(body);
    }
    // The buffers of a decoded message's typed arrays (a postMessage transfer list).
    function transfers(msg) {
      const out = new Set();
      const walk = v => {
        if (v instanceof ArrayBuffer) out.add(v);
        else if (ArrayBuffer.isView(v)) out.add(v.buffer);
        else if (Array.isArray(v)) v.forEach(walk);
        else if (v && typeof v === 'object') for (const k of Object.keys(v)) walk(v[k]);
      };
      walk(msg);
      return [...out];
    }
    self.IscWire = { encode, decode, transfers };
  }
  self.iscRemoteWireDefine = define;
  define();
})();
