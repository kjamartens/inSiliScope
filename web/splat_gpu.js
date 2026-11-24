// SPDX-License-Identifier: BSD-3-Clause
// The blink splat + camera noise of the viewer's movies and Records on WebGPU (2026-11-24): the backend the module's
// SplatGpuHost calls (Module.iscGpu, cli/gpu_js_backend.cpp), running the kernel splat_frames of
// adapter/inSiliScope/Simulation/SplatGpu.wgsl (embedded as self.ISC_SPLAT_WGSL by tools/embed_web_module.mjs) -- the
// WGSL twin of the adapter's Direct3D 11 splat. The engine's movie stays synchronous: every call here returns at once, and the
// frames come back through an OffscreenCanvas (read16), whose getImageData waits for the GPU (TensorFlow.js's readSync
// for its WebGPU backend): each 32-bit word of the output goes into one rgba8unorm texel, an opaque canvas keeps its
// red and green bytes exactly, and those are the ADU.
//
// Self-contained (stringified into the viewer's workers): defines
// self.IscSplatGpu = { create(device, wgsl, name) -> Promise of the object for Module.iscGpu, or of null }.
function iscSplatGpuDefine() {
  // (the WGSL file's "//// kernel <name>" sections, each with the "//// common" one, as web/wf_gpu.js splits them)
  function kernelSources(text) {
    const parts = text.split(/^\/\/\/\/ /m);
    let common = '';
    const out = {};
    for (const p of parts.slice(1)) {
      const nl = p.indexOf('\n');
      const head = p.slice(0, nl).trim(), body = p.slice(nl + 1);
      if (head === 'common') common = body;
      else if (head.startsWith('kernel ')) out[head.slice(7).trim()] = body;
    }
    for (const k of Object.keys(out)) out[k] = common + '\n' + out[k];
    return out;
  }
  const ROW = 1024;            // texels per canvas row (bytesPerRow 4096: a multiple of 256)
  const STRIP = ROW * 4096;    // words per read (16 MB)

  // The pipelines compiled asynchronously here (~2.5 s for splat_frames, D3D12 FXC or DXC alike: paid by the first
  // movie otherwise), then the backend.
  async function create(device, wgsl, name) {
    if (!device || !wgsl || typeof OffscreenCanvas === 'undefined') return null;
    const pipes = {};
    for (const [k, code] of Object.entries(kernelSources(wgsl)))
      pipes[k] = await device.createComputePipelineAsync({ layout: 'auto', compute: { module: device.createShaderModule({ code }), entryPoint: k } });
    const bufs = new Map();
    let next = 1;
    const B = {
      name: name || 'WebGPU', err: '', lost: '',
      maxBinding: Math.min(device.limits.maxStorageBufferBindingSize, device.limits.maxBufferSize),
      fail(e) { B.err = String(e && e.message || e); return 0; },
      make(bytes, data) {
        try {
          const size = Math.max(16, Math.ceil(bytes / 4) * 4);
          const b = device.createBuffer({ size, usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_SRC | GPUBufferUsage.COPY_DST });
          if (data) B.put(b, 0, data);
          bufs.set(next, b);
          return next++;
        } catch (e) { return B.fail(e); }
      },
      // (writeBuffer takes whole words: a tail of 1-3 bytes padded with zeros)
      put(b, offset, data) {
        const whole = data.length & ~3;
        if (whole) device.queue.writeBuffer(b, offset, data, 0, whole);
        if (whole < data.length) {
          const t = new Uint8Array(4);
          t.set(data.subarray(whole));
          device.queue.writeBuffer(b, offset + whole, t);
        }
      },
      write(id, offset, data) {
        try { B.put(bufs.get(id), offset, data); return 1; } catch (e) { return B.fail(e); }
      },
      copy(dst, src, bytes) {
        try {
          const enc = device.createCommandEncoder();
          enc.copyBufferToBuffer(bufs.get(src), 0, bufs.get(dst), 0, Math.ceil(bytes / 4) * 4);
          device.queue.submit([enc.finish()]);
          return 1;
        } catch (e) { return B.fail(e); }
      },
      // The parameters (16 words) at binding 0, the buffers at 1.. in order; submitted at once (each dispatch its own
      // uniform block).
      dispatch(kernel, params, ids, x, y, z) {
        try {
          const pipe = pipes[kernel];
          if (!pipe) return B.fail('no kernel ' + kernel);
          if (B.lost) return B.fail(B.lost);
          const ub = device.createBuffer({ size: 64, usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST });
          device.queue.writeBuffer(ub, 0, params);
          const entries = [{ binding: 0, resource: { buffer: ub } }];
          ids.forEach((id, i) => entries.push({ binding: i + 1, resource: { buffer: bufs.get(id) } }));
          const group = device.createBindGroup({ layout: pipe.getBindGroupLayout(0), entries });
          const enc = device.createCommandEncoder();
          const pass = enc.beginComputePass();
          pass.setPipeline(pipe);
          pass.setBindGroup(0, group);
          pass.dispatchWorkgroups(x, y, z);
          pass.end();
          device.queue.submit([enc.finish()]);
          ub.destroy();   // (after its submit: freed when the work is done)
          return 1;
        } catch (e) { return B.fail(e); }
      },
      // The low 16 bits of the buffer's first `words` words into dst (a Uint16Array view of the heap), synchronously.
      read16(id, words, dst) {
        try {
          if (B.lost) return B.fail(B.lost);
          const src = bufs.get(id);
          const c2 = B.c2 || (B.c2 = new OffscreenCanvas(ROW, 1));
          const x2 = B.x2 || (B.x2 = c2.getContext('2d', { willReadFrequently: true }));
          for (let w0 = 0; w0 < words; w0 += STRIP) {
            const n = Math.min(STRIP, words - w0), rows = Math.ceil(n / ROW), full = Math.floor(n / ROW);
            // (a canvas per read: a fresh texture, never an image of the last one)
            const cv = new OffscreenCanvas(ROW, rows), ctx = cv.getContext('webgpu');
            ctx.configure({ device, format: 'rgba8unorm', usage: GPUTextureUsage.COPY_DST, alphaMode: 'opaque' });
            const tex = ctx.getCurrentTexture(), enc = device.createCommandEncoder();
            if (full) enc.copyBufferToTexture({ buffer: src, offset: w0 * 4, bytesPerRow: ROW * 4 }, { texture: tex }, [ROW, full]);
            if (n > full * ROW)
              enc.copyBufferToTexture({ buffer: src, offset: (w0 + full * ROW) * 4, bytesPerRow: ROW * 4 },
                { texture: tex, origin: [0, full] }, [n - full * ROW, 1]);
            device.queue.submit([enc.finish()]);
            if (c2.width !== ROW || c2.height !== rows) { c2.width = ROW; c2.height = rows; }
            x2.clearRect(0, 0, ROW, rows);
            x2.drawImage(cv, 0, 0);
            const px = x2.getImageData(0, 0, ROW, rows).data;
            for (let i = 0; i < n; i++) dst[w0 + i] = px[4 * i] | (px[4 * i + 1] << 8);
            ctx.unconfigure();
          }
          return 1;
        } catch (e) { return B.fail(e); }
      },
      free(id) { const b = bufs.get(id); if (b) { b.destroy(); bufs.delete(id); } },
    };
    device.lost.then(i => { B.lost = i.message || 'the WebGPU device was lost'; });
    return B;
  }
  self.IscSplatGpu = { create };
}
self.iscSplatGpuDefine = iscSplatGpuDefine;
iscSplatGpuDefine();
