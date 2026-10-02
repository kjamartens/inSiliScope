// The C++ side of the imaging parity checks: web/insiliscope_module.js (the viewer's WASM: core C ABI +
// the scope movie) evaluated under Node or in a worker, with small wrappers. No build needed.
//   const C = await loadWasmScope(moduleText);
//   C.events(seed, params, kinetics, rect, zMin, zMax, t0, t1) -> Float64Array (stride 7)
//   C.opticalVolume(seed, params, rect, zMin, zMax, nx, ny, nz, sub) -> Float32Array (3 channels)
//   C.movie('k=v ...') -> { frames: Uint16Array, info }
export async function loadWasmScope(moduleText) {
  const g = globalThis;
  if (!g.self) g.self = g;
  const prev = g.ISC_MODULE_SRC;
  (0, eval)(moduleText);
  (0, eval)(g.ISC_MODULE_SRC);
  if (prev !== undefined) g.ISC_MODULE_SRC = prev;
  const M = await g.createInsiliscope();
  const str = s => { const n = M.lengthBytesUTF8(s) + 1, p = M._malloc(n); M.stringToUTF8(s, p, n); return p; };

  function world(seed, params) {
    const pp = M._isc_params_new();
    for (const [k, v] of Object.entries(params)) {
      const s = str(k);
      const r = M._isc_params_set(pp, s, +v);
      M._free(s);
      if (r !== 0) throw new Error('unknown core param ' + k);
    }
    const w = M._isc_world_new(seed >>> 0, pp);
    M._isc_params_free(pp);
    return w;
  }
  // cap/total convention: ask, grow, ask again.
  function query(fn, stride) {
    let cap = 1 << 16, buf = M._malloc(cap * stride * 8);
    for (;;) {
      const n = fn(buf, cap);
      if (n < 0) { M._free(buf); throw new Error('query failed'); }
      if (n <= cap) { const out = M.HEAPF64.slice(buf / 8, buf / 8 + n * stride); M._free(buf); return out; }
      M._free(buf); cap = n + (n >> 2); buf = M._malloc(cap * stride * 8);
    }
  }
  return {
    M,
    events(seed, params, kin, [x0, y0, x1, y1], zMin, zMax, t0, t1) {
      const w = world(seed, params);
      if (M._isc_world_set_kinetics(w, kin.activationRatePerSec, kin.onSec, kin.offSec, kin.bleachProb, kin.photonCV) !== 0)
        throw new Error('bad kinetics');
      const r = query((b, cap) => M._isc_events_in_window(w, x0, y0, x1, y1, zMin, zMax, t0, t1, b, cap), 7);
      M._isc_world_free(w);
      return r;
    },
    sites(seed, params, [x0, y0, x1, y1], zMin, zMax) {
      const w = world(seed, params);
      const r = query((b, cap) => M._isc_sites_in_window(w, x0, y0, x1, y1, zMin, zMax, b, cap), 4);
      M._isc_world_free(w);
      return r;
    },
    // Optical volume (ABI 6): Float32Array 3 * nx * ny * nz, channel-major; .cells = the return value.
    opticalVolume(seed, params, [x0, y0, x1, y1], zMin, zMax, nx, ny, nz, sub) {
      const w = world(seed, params), n = 3 * nx * ny * nz, buf = M._malloc(n * 4);
      const cells = M._isc_optical_volume_in_window(w, x0, y0, x1, y1, zMin, zMax, nx, ny, nz, sub, buf);
      const out = M.HEAPF32.slice(buf / 4, buf / 4 + n);
      M._free(buf); M._isc_world_free(w);
      if (cells < 0) throw new Error('optical volume query failed');
      out.cells = cells;
      return out;
    },
    // WideField scene images (CPU) of a spec: {info: [NX, NY, nx, ny, fovX0, fovY0, cw, ch, kernels, planes,
    // channels, hasPersistent, dyes, geometry], frac, images: Float32Array channels * cw * ch}.
    wfImages(spec) {
      const s = str(spec), err = M._malloc(512);
      try {
        const h = M._isc_wf_begin(s, err, 512);
        if (h < 0) throw new Error(M.UTF8ToString(err));
        const info = M._malloc(14 * 4), frac = M._malloc(16);
        M._isc_wf_job(h, info, frac);
        const iv = Array.from(M.HEAP32.slice(info / 4, info / 4 + 14)), fr = Array.from(M.HEAPF64.slice(frac / 8, frac / 8 + 2));
        const n = iv[6] * iv[7] * Math.max(1, iv[10]), out = M._malloc(n * 4);
        M._isc_wf_cpu_images(h, out);
        const images = M.HEAPF32.slice(out / 4, out / 4 + n);
        M._free(info); M._free(frac); M._free(out); M._isc_wf_end(h);
        return { info: iv, frac: fr, images };
      } finally { M._free(s); M._free(err); }
    },
    movie(spec) {
      const s = str(spec), info = M._malloc(24), err = M._malloc(512);
      try {
        const need = M._isc_scope_movie(s, 0, 0, info, err, 512);
        if (need < 0) throw new Error(M.UTF8ToString(err));
        const out = M._malloc(need * 2);
        const r = M._isc_scope_movie(s, out, need, info, err, 512);
        if (r < 0) { M._free(out); throw new Error(M.UTF8ToString(err)); }
        const frames = M.HEAPU16.slice(out / 2, out / 2 + need);
        const iv = M.HEAP32.slice(info / 4, info / 4 + 6);
        M._free(out);
        return { frames, info: { width: iv[0], height: iv[1], frames: iv[2], blinks: iv[3], dyes: iv[4], halfTimeMs: iv[5] } };
      } finally { M._free(s); M._free(info); M._free(err); }
    },
  };
}
