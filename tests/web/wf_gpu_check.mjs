#!/usr/bin/env node
// The viewer's WideField GPU path (web/wf_gpu.js running WidefieldGpu.wgsl on
// WebGPU) against the CPU images of the same scene (isc_wf_cpu_images), in
// headless Chromium (SwiftShader when there is no GPU). Also a movie made from
// the GPU images vs the CPU movie.
//
//   node tests/web/wf_gpu_check.mjs          (Playwright; build + embed the module first)
import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
let chromium;
try { ({ chromium } = await import('playwright')); }
catch { ({ chromium } = await import('/opt/node22/lib/node_modules/playwright/index.mjs')); }

const server = http.createServer((req, res) => {
  const u = decodeURIComponent(req.url.split('?')[0]);
  if (u === '/') { res.setHeader('content-type', 'text/html'); res.end('<!doctype html><html><body></body></html>'); return; }
  const f = path.join(ROOT, u);
  if (!f.startsWith(ROOT) || !fs.existsSync(f)) { res.statusCode = 404; res.end(); return; }
  res.setHeader('content-type', f.endsWith('.js') ? 'text/javascript' : 'application/octet-stream');
  res.end(fs.readFileSync(f));
}).listen(0);
const port = server.address().port;

const browser = await chromium.launch({ args: ['--enable-unsafe-webgpu', '--enable-features=Vulkan', '--use-vulkan=swiftshader',
  '--use-webgpu-adapter=swiftshader', '--disable-vulkan-surface'] });
const page = await browser.newPage();
page.on('console', m => console.log('[page]', m.text()));
await page.goto(`http://localhost:${port}/`);
await page.addScriptTag({ url: '/web/insiliscope_module.js' });
await page.addScriptTag({ url: '/web/wf_gpu.js' });
const r = await page.evaluate(async () => {
  (0, eval)(self.ISC_MODULE_SRC);
  const M = await createInsiliscope();
  const gpu = await IscWfGpu.create(self.ISC_WF_WGSL, true);
  if (!gpu) return { skip: 'no WebGPU adapter' };
  const out = { adapter: gpu.adapterInfo, cases: [] };
  const run = async (spec) => {
    const err = M._malloc(512), s = M._malloc(spec.length + 1);
    M.stringToUTF8(spec, s, spec.length + 1);
    const h = M._isc_wf_begin(s, err, 512);
    if (h < 0) return { spec, error: M.UTF8ToString(err) };
    let t = performance.now();
    const job = IscWfGpu.jobFromModule(M, h);
    const tJob = performance.now() - t;
    t = performance.now();
    const gi = await gpu.images(job);
    const tGpu = performance.now() - t;
    t = performance.now();
    const gi2 = await gpu.images(job); // all planes resident now
    const tGpu2 = performance.now() - t;
    const cs = job.cw * job.ch, n = job.channels.length;
    const cp = M._malloc(n * cs * 4);
    t = performance.now();
    M._isc_wf_cpu_images(h, cp);
    const tCpu = performance.now() - t;
    const cpu = M.HEAPF32.slice(cp / 4, cp / 4 + n * cs);
    let e2 = 0, s2 = 0, emax = 0, peak = 0, same2 = true;
    for (let c = 0; c < n; c++)
      for (let i = 0; i < cs; i++) {
        const a = gi[c][i], b = cpu[c * cs + i];
        e2 += (a - b) * (a - b); s2 += b * b; emax = Math.max(emax, Math.abs(a - b)); peak = Math.max(peak, Math.abs(b));
        same2 = same2 && gi2[c][i] === a;
      }
    // Movie from the GPU images vs the CPU movie.
    const all = new Float32Array(n * cs);
    gi.forEach((g, c) => all.set(g, c * cs));
    const ip = M._malloc(all.byteLength);
    M.HEAPF32.set(all, ip / 4);
    M._isc_wf_set_images(h, ip);
    const info = M._malloc(24);
    const need = M._isc_wf_movie(h, 0, 0, info, err, 512);
    const fp = M._malloc(need * 2);
    M._isc_wf_movie(h, fp, need, info, err, 512);
    const gm = M.HEAPU16.slice(fp / 2, fp / 2 + need);
    const sp = M._malloc(spec.length + 1);
    M.stringToUTF8(spec, sp, spec.length + 1);
    const cm0 = M._malloc(need * 2);
    M._isc_scope_movie(sp, cm0, need, info, err, 512);
    const cm = M.HEAPU16.slice(cm0 / 2, cm0 / 2 + need);
    let diff = 0, maxd = 0, sg = 0, sc = 0, off = 100 * need;
    for (let i = 0; i < need; i++) { const d = Math.abs(gm[i] - cm[i]); if (d) diff++; maxd = Math.max(maxd, d); sg += gm[i]; sc += cm[i]; }
    const meanRel = Math.abs(sg - sc) / Math.max(1, sc - off);
    M._isc_wf_end(h);
    for (const p of [err, s, cp, ip, info, fp, sp, cm0]) M._free(p);
    return { spec, NX: job.NX, NY: job.NY, channels: n, planes: job.planes.length, kernels: job.kernels.length,
      rms: Math.sqrt(e2 / s2), max: emax / peak, resident: same2, tJob, tGpu, tGpu2, tCpu,
      movieDiffPx: diff / need, movieMaxAdu: maxd, meanRel };
  };
  out.cases.push(await run('size=64 frames=4 modality=1 x=-3 y=4 z=0.5 labeling-pct-bleaching=10 labeling-pct-nonbleaching=40'));
  out.cases.push(await run('size=64 frames=4 modality=1 x=7.37 y=-2.11 z=1.2 wf-upscale=2 labeling-pct-bleaching=20 labeling-pct-nonbleaching=50 start-sec=12'));
  return out;
});
await browser.close();
server.close();
if (r.skip) { console.log('SKIP', r.skip); process.exit(0); }
console.log('adapter:', r.adapter);
let fail = 0;
for (const c of r.cases) {
  if (c.error) { console.log('FAIL', c.spec, c.error); fail++; continue; }
  // fp16 plane spectra: <= 1e-3 rms of the image. In a movie a pixel whose
  // Poisson draw sits at a count boundary may land one electron apart, and
  // the counter stream's following read-noise draw then differs too: a few
  // % of pixels, the frames' signal the same.
  const ok = c.rms < 1e-3 && c.max < 5e-3 && c.resident && c.movieDiffPx < 0.05 && c.meanRel < 1e-3;
  if (!ok) fail++;
  console.log(`${ok ? 'ok  ' : 'FAIL'}  ${c.spec}\n      ${c.NX}x${c.NY} FFT, ${c.channels} channels, ${c.planes} planes, ${c.kernels} kernels: ` +
    `GPU vs CPU images rms ${c.rms.toExponential(1)}, max ${c.max.toExponential(1)} of peak; resident re-run identical ${c.resident}; ` +
    `job ${c.tJob.toFixed(0)} ms, GPU ${c.tGpu.toFixed(0)} ms (resident ${c.tGpu2.toFixed(0)} ms), CPU ${c.tCpu.toFixed(0)} ms; ` +
    `movie from GPU images: ${(100 * c.movieDiffPx).toFixed(2)}% of pixels differ (max ${c.movieMaxAdu} ADU), signal ${c.meanRel.toExponential(1)} apart`);
}
console.log(fail ? `${fail} case(s) FAILED` : 'all WideField GPU checks passed');
process.exit(fail ? 1 : 0);
