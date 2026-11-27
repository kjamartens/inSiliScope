#!/usr/bin/env node
// SPDX-License-Identifier: BSD-3-Clause
// Not a test: the viewer's ● Record timed with the blink splat + camera noise on WebGPU and on the CPU (?gpu=0's
// switch), DNA-PAINT and dSTORM at a few sizes, each after a warm-up Record of the same spec (the sample, PSF and GPU
// pipelines made). Prints the engine's ms (the movie job) and the page's wall ms per Record.
//   node tests/web/bench_gpu_splat.mjs [--cases=256x40,512x40]  (~1-2 min)
import { runViewer } from './browser.mjs';

const arg = process.argv.find(a => a.startsWith('--cases='));
const CASES = (arg ? arg.slice(8) : '256x40,512x40').split(',').map(s => s.split('x').map(Number));

await runViewer('bench GPU splat', {
  watchdogSec: 900, viewport: { width: 1200, height: 800 },
  args: ['--enable-unsafe-webgpu', '--disable-dawn-features=use_dxc', ...(process.platform === 'linux' ? ['--enable-features=Vulkan'] : [])],
}, async ({ page }) => {
  await page.waitForFunction(() => window.liveState && genWorkers.ok, null, { timeout: 120000 });
  const env = await page.evaluate(() => ({ coi: self.crossOriginIsolated, cores: navigator.hardwareConcurrency }));
  console.log(`page: cross-origin isolated ${env.coi} (the threaded module), ${env.cores} logical cores`);
  await page.evaluate(() => {
    const disk = document.getElementById('lv_recDisk'); if (disk) { disk.checked = false; disk.dispatchEvent(new Event('change')); }
    window.__flags = {};
    window.gpuFlags = () => window.__flags;
  });
  const set = (id, v) => page.evaluate(([id, v]) => {
    const e = document.getElementById(id);
    if (e) { e.value = v; e.dispatchEvent(new Event(e.tagName === 'SELECT' ? 'change' : 'input', { bubbles: true })); }
  }, [id, String(v)]);
  const record = async flags => {
    const t0 = Date.now();
    await page.evaluate(f => { window.__flags = f; liveState.rec.info = null; liveState.rec.engineMs = -1; }, flags);
    await page.evaluate(() => { document.getElementById('lv_rec').click(); document.getElementById('lv_recGo').click(); });
    await page.waitForFunction(() => !liveState.rec.on && liveState.rec.engineMs > 0, null, { timeout: 600000, polling: 100 });
    const r = await page.evaluate(() => ({ ms: liveState.rec.engineMs, got: liveState.rec.got, gpu: liveState.rec.gpu,
      back: (liveState.rec.info && liveState.rec.info.backends || []).join('; ') }));
    return { ...r, wall: Date.now() - t0 };
  };
  for (const mode of ['DNA-PAINT', 'dSTORM']) {
    if (mode === 'dSTORM') { await set('mv_mt-mode', 0); await set('mv_npc-dye', 'None'); }
    for (const [size, frames] of CASES) {
      await set('mv_size', size);
      await page.evaluate(n => { const e = document.getElementById('lv_recN'); e.value = n; e.dispatchEvent(new Event('change')); }, frames);
      await record({});   // (warm: the sample, its PSF, the GPU's kernels and self-check)
      await record({ noGpu: true });
      const g = await record({});
      const c = await record({ noGpu: true });
      console.log(`${mode.padEnd(9)} ${size} px x ${frames} frames: GPU ${g.ms.toFixed(0)} ms (wall ${g.wall}), ` +
        `CPU ${c.ms.toFixed(0)} ms (wall ${c.wall}): x${(c.ms / g.ms).toFixed(2)}  [${g.gpu || 'no GPU'}; ${g.back}]`);
    }
  }
});
