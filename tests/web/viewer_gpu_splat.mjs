#!/usr/bin/env node
// SPDX-License-Identifier: BSD-3-Clause
// The blink splat + camera noise of the viewer's Records on WebGPU (web/splat_gpu.js running SplatGpu.wgsl through the
// module's SplatGpuHost, cli/gpu_js_backend.cpp; the read-back through an OffscreenCanvas): a DNA-PAINT and a WideField
// ● Record, each once on the GPU and once on the CPU (?gpu=0's switch, noGpu), the same fresh sample. On the GPU the
// Record names its adapter, the DNA-PAINT one splats its blinks there ("splat, GPU"), and the frames equal the CPU's
// within float rounding: >= 95 % of the samples equal, the mean |difference| under 0.5 % of the mean (the remote
// server's check, tests/remote/server_check.mjs `near`).
//   node tests/web/viewer_gpu_splat.mjs [--channel=chrome]   (exit 77: no hardware WebGPU adapter, nothing checked)
// Headless Chromium on Windows has no DXC (dxil.dll): Dawn's FXC compiler instead (--disable-dawn-features=use_dxc).
import { runViewer } from './browser.mjs';

const N = 8, SIZE = 128;
const near = (a, b) => {
  if (!a || !b || a.length !== b.length || !a.length) return { ok: false, text: `lengths ${a && a.length} vs ${b && b.length}` };
  let eq = 0, d = 0, m = 0, sat = 0, lo = 65535, hi = 0;
  for (let i = 0; i < a.length; i++) {
    eq += a[i] === b[i]; d += Math.abs(a[i] - b[i]); m += b[i]; sat += b[i] === 65535;
    lo = Math.min(lo, b[i]); hi = Math.max(hi, b[i]);
  }
  return { ok: eq >= 0.95 * a.length && d <= 0.005 * m, eq: eq / a.length, sat: sat / a.length,
    text: `${(100 * eq / a.length).toFixed(2)} % equal, mean |diff| ${(d / a.length).toFixed(3)} of ${(m / a.length).toFixed(1)} ADU ` +
      `(${lo}..${hi}, ${(100 * sat / a.length).toFixed(1)} % saturated)` };
};

await runViewer('viewer GPU splat', {
  watchdogSec: 270, viewport: { width: 1200, height: 800 },
  args: ['--enable-unsafe-webgpu', '--disable-dawn-features=use_dxc', ...(process.platform === 'linux' ? ['--enable-features=Vulkan'] : [])],
}, async ({ page, check }) => {
  const adapter = await page.evaluate(async () => {
    const a = navigator.gpu && await navigator.gpu.requestAdapter({ powerPreference: 'high-performance' }).catch(() => null);
    if (!a) return null;
    const fb = a.isFallbackAdapter || (a.info && a.info.isFallbackAdapter);
    const d = fb ? null : await a.requestDevice().catch(() => null);
    if (d) d.destroy();
    return d ? `${a.info.vendor} ${a.info.architecture} ${a.info.description}`.trim() : null;
  });
  if (!adapter) { console.log('SKIP  no hardware WebGPU adapter in this browser: nothing checked'); return 'skip'; }
  await page.waitForFunction(() => window.liveState && genWorkers.ok, null, { timeout: 120000 });
  await page.evaluate(size => {
    const s = document.getElementById('mv_size'); s.value = size; s.dispatchEvent(new Event('input', { bubbles: true }));
    const disk = document.getElementById('lv_recDisk'); if (disk) { disk.checked = false; disk.dispatchEvent(new Event('change')); }   // (frames in memory)
    const n = document.getElementById('lv_recN'); n.value = 8; n.dispatchEvent(new Event('change'));
    // the GPU switch of the Record's job (gpuFlags)
    window.__flags = {};
    window.gpuFlags = () => window.__flags;
  }, SIZE);
  // flags: the Record job's GPU switches (gpuFlags: noGpu = ?gpu=0, noWfGpu = ?wfgpu=0)
  const record = async flags => {
    const t0 = Date.now();
    await page.evaluate(f => { window.__flags = f; liveState.rec.info = null; liveState.rec.engineMs = -1; }, flags);
    await page.evaluate(() => { document.getElementById('lv_rec').click(); document.getElementById('lv_recGo').click(); });
    await page.waitForFunction(() => !liveState.rec.on && liveState.rec.engineMs > 0, null, { timeout: 180000 });
    return page.evaluate(async ({ t0 }) => {
      const R = liveState.rec, st = liveState.play.store, all = new Uint16Array(R.got * R.w * R.h);
      for (let k = 0; k < R.got; k++) all.set(await st.frame(k), k * R.w * R.h);
      return { frames: Array.from(all), got: R.got, w: R.w, h: R.h, gpu: R.gpu, spec: R.spec, ms: R.engineMs, msg: R.msg,
        info: R.info, wall: Date.now() - t0 };
    }, { t0 });
  };
  // A GPU Record (gpu: its switches), the same again (warm: its speed, the same frames), and a CPU one.
  const runCase = async (kind, gpu) => {
    const g = await record(gpu), g2 = await record(gpu), c = await record({ noGpu: true });
    const backends = (g.info && g.info.backends || []).join('; ');
    const same = g.frames.length === g2.frames.length && g.frames.every((v, i) => v === g2.frames[i]);
    check(g.got === N && c.got === N && g.w === SIZE && g.spec === c.spec,
      `${kind}: two Records of ${g.got} / ${c.got} frames of ${g.w}×${g.h}, the same spec`);
    check(!!g.gpu && !c.gpu, `${kind}: the GPU Record names its GPU ("${g.gpu || ''}"), the CPU one none ("${c.gpu || ''}")`);
    const n = near(new Uint16Array(g.frames), new Uint16Array(c.frames));
    check(n.ok && n.sat < 0.01, `${kind}: the GPU frames = the CPU's within rounding, below saturation: ${n.text}`);
    check(same, `${kind}: a second GPU Record = the first, sample for sample (the engine: GPU ${g.ms.toFixed(0)} ms first, ` +
      `${g2.ms.toFixed(0)} ms again; CPU ${c.ms.toFixed(0)} ms; the first Record also builds the sample and its PSF)`);
    return { g, c, backends, n };
  };
  // DNA-PAINT (the default scene: the pores and the microtubules blink)
  const dp = await runCase('DNA-PAINT', {});
  check(/\(splat, GPU\)/.test(dp.backends), `DNA-PAINT: the blinks splatted on the GPU (the last frame: ${dp.backends})`);
  // WideField: every target's label in WideField mode, 1 % of the sites labelled, a 5 ms exposure (below saturation); the mean-field images on the
  // CPU (?wfgpu=0: the WebGPU scenes' fp16 spectra are another path, wf_gpu_check.mjs), the noise on the GPU, no blinks
  await page.evaluate(() => {
    for (const [id, v] of [['mv_mt-mode', '3'], ['mv_npc-mode', '3'], ['mv_mt-label-pct', '1'], ['mv_npc-label-pct', '1'], ['mv_exposure-ms', '5']]) {
      const e = document.getElementById(id);
      if (e) { e.value = v; e.dispatchEvent(new Event(e.tagName === 'SELECT' ? 'change' : 'input', { bubbles: true })); }
    }
  });
  const wf = await runCase('WideField', { noWfGpu: true });
  check(/(^| )mt-mode=3( |$)/.test(wf.g.spec) && /(^| )mt-label-pct=1( |$)/.test(wf.g.spec) && / exposure-ms=5( |$)/.test(wf.g.spec) && !/SMLM: [1-9]/.test(wf.backends),
    `WideField: the labels WideField, 1 %, at 5 ms, no blinks (the last frame: ${wf.backends})`);
  // (for the record: the mean-field images on the GPU as well, against the same CPU Record; not checked)
  const all = await record({});
  console.log(`info  WideField with the mean-field images on the GPU too: ${near(new Uint16Array(all.frames), new Uint16Array(wf.c.frames)).text}`);
  console.log(`adapter: ${adapter}; GPU: ${dp.g.gpu}`);
});
