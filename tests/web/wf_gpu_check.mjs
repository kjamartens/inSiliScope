#!/usr/bin/env node
// The viewer's WideField GPU path (web/wf_gpu.js running WidefieldGpu.wgsl on
// WebGPU) against the CPU images of the same scene (isc_wf_cpu_images), in
// headless Chromium (SwiftShader when there is no GPU; ISC_CHROME=1: the installed Chrome). Also a movie made from
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

// ISC_CHROME=1: the installed Chrome on the real GPU (Windows: headless Chromium has no WebGPU adapter there).
const browser = await chromium.launch(process.env.ISC_CHROME ? { channel: 'chrome', args: ['--enable-unsafe-webgpu'] } :
  { args: ['--enable-unsafe-webgpu', '--enable-features=Vulkan', '--use-vulkan=swiftshader',
    '--use-webgpu-adapter=swiftshader', '--disable-vulkan-surface'] });
const dog = setTimeout(() => { console.log('FAIL  wf_gpu_check: no end after 120 s'); process.exit(2); }, 120000);
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
  let stage = '';
  const run = async (spec) => {
    stage = 'isc_wf_begin';
    const err = M._malloc(512), s = M._malloc(spec.length + 1);
    M.stringToUTF8(spec, s, spec.length + 1);
    const h = M._isc_wf_begin(s, err, 512);
    if (h < 0) return { spec, error: M.UTF8ToString(err) };
    // The movie's mean-field scenes (issue 16: one per continuous population that starts mean-field).
    const scenes = M._isc_wf_scenes(h);
    if (scenes < 1) { M._isc_wf_end(h); return { spec, error: 'no mean-field scene' }; }
    M._isc_wf_select(h, 0);
    let t = performance.now();
    stage = 'jobFromModule';
    const job = IscWfGpu.jobFromModule(M, h);
    const tJob = performance.now() - t;
    t = performance.now();
    stage = 'the GPU images';
    const gi = await gpu.images(job);
    const tGpu = performance.now() - t;
    t = performance.now();
    const gi2 = await gpu.images(job); // all planes resident now
    const tGpu2 = performance.now() - t;
    const cs = job.cw * job.ch, n = job.channels.length;
    const cp = M._malloc(n * cs * 4);
    t = performance.now();
    stage = 'isc_wf_cpu_images';
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
    stage = 'isc_wf_set_images + isc_wf_movie';
    M._isc_wf_set_images(h, ip);
    const info = M._malloc(24);
    const need = M._isc_wf_movie(h, 0, 0, info, err, 512);
    const fp = M._malloc(need * 2);
    M._isc_wf_movie(h, fp, need, info, err, 512);
    const gm = M.HEAPU16.slice(fp / 2, fp / 2 + need);
    const sp = M._malloc(spec.length + 1);
    M.stringToUTF8(spec, sp, spec.length + 1);
    const cm0 = M._malloc(need * 2);
    stage = 'isc_scope_movie';
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
  // A WideField label; and a PALM label whose green pre state is mean-field next to its blinks (two kernel groups).
  try { out.cases.push(await run('size=32 frames=2 psf-kernel-half-width-nm=3000 x=-3 y=4 z=0.5 mt-dye=mEGFP light-preset=auto')); }
  catch (e) { out.cases.push({ spec: 'size=32 frames=2 psf-kernel-half-width-nm=3000 x=-3 y=4 z=0.5 mt-dye=mEGFP light-preset=auto', error: `${e.message} in ${stage}` }); return out; }   // (an abort ends the module)
  try { out.cases.push(await run('size=32 frames=2 psf-kernel-half-width-nm=3000 x=7.37 y=-2.11 z=1.2 wf-upscale=2 mt-dye=mEos3.2 light-preset=auto laser-488=0.05 mt-label-pct=3 start-sec=12')); }
  catch (e) { out.cases.push({ spec: 'size=32 frames=2 psf-kernel-half-width-nm=3000 x=7.37 y=-2.11 z=1.2 wf-upscale=2 mt-dye=mEos3.2 light-preset=auto laser-488=0.05 mt-label-pct=3 start-sec=12', error: `${e.message} in ${stage}` }); return out; }   // (an abort ends the module)
  // Two scenes in turn (one per labelled target, 2026-10-29): A, B, A again keeps A's spectra (resident, identical).
  const two = 'size=32 frames=2 psf-kernel-half-width-nm=3000 x=10 y=-8 z=1.5 mt-dye=mEGFP npc-dye=mEGFP npc-mode=WideField light-preset=auto mean-field-density-per-um2=0 mean-field-max-emitters=0';
  try {
    stage = 'isc_wf_begin (two scenes)';
    const err = M._malloc(512), s = M._malloc(two.length + 1);
    M.stringToUTF8(two, s, two.length + 1);
    const h = M._isc_wf_begin(s, err, 512);
    if (h < 0) out.turns = { spec: two, error: M.UTF8ToString(err) };
    else {
      const jobs = [];
      for (let k = 0; k < Math.min(2, M._isc_wf_scenes(h)); k++) { M._isc_wf_select(h, k); jobs.push(IscWfGpu.jobFromModule(M, h)); }
      if (jobs.length < 2) out.turns = { spec: two, error: `${jobs.length} mean-field scene(s), not 2` };
      else {
        stage = 'the GPU images (two scenes)';
        const a = await gpu.images(jobs[0]);
        await gpu.images(jobs[1]);
        const t = performance.now();
        const a2 = await gpu.images(jobs[0]);
        const tA2 = performance.now() - t;
        let same = true;
        a.forEach((g, c) => { for (let i = 0; i < g.length; i++) same = same && a2[c][i] === g[i]; });
        out.turns = { spec: two, same, tA2, resident: gpu.resident.size, shape: jobs.map(j => `${j.NX}x${j.NY}`).join(' / '),
          distinct: jobs[0].geometry !== jobs[1].geometry, planes: jobs.map(j => j.planes.length).join(' / ') };
      }
      M._isc_wf_end(h);
    }
    M._free(err); M._free(s);
  } catch (e) { out.turns = { spec: two, error: `${e.message} in ${stage}` }; }
  return out;
});
clearTimeout(dog);
await browser.close();
server.close();
// (a visible skip: 77 = ctest's SKIP_RETURN_CODE, not a pass)
if (r.skip) { console.log(`SKIP  ${r.skip}: nothing checked (exit 77)`); process.exit(77); }
console.log('adapter:', r.adapter);
let fail = 0;
for (const c of r.cases) {
  if (c.error) { console.log('FAIL', c.spec, c.error); fail++; continue; }
  // fp16 plane spectra: <= 1e-3 rms of the image. In a movie a pixel whose
  // Poisson draw sits at a count boundary may land one electron apart, and
  // the counter stream's following read-noise draw then differs too: the
  // share grows with the photons per pixel (8.9 % for the dense PALM case on
  // an Intel Iris Xe, 2026-10-05), the frames' signal the same.
  const ok = c.rms < 1e-3 && c.max < 5e-3 && c.resident && c.movieDiffPx < 0.15 && c.meanRel < 1e-3;
  if (!ok) fail++;
  console.log(`${ok ? 'ok  ' : 'FAIL'}  ${c.spec}\n      ${c.NX}x${c.NY} FFT, ${c.channels} channels, ${c.planes} planes, ${c.kernels} kernels: ` +
    `GPU vs CPU images rms ${c.rms.toExponential(1)}, max ${c.max.toExponential(1)} of peak; resident re-run identical ${c.resident}; ` +
    `job ${c.tJob.toFixed(0)} ms, GPU ${c.tGpu.toFixed(0)} ms (resident ${c.tGpu2.toFixed(0)} ms), CPU ${c.tCpu.toFixed(0)} ms; ` +
    `movie from GPU images: ${(100 * c.movieDiffPx).toFixed(2)}% of pixels differ (max ${c.movieMaxAdu} ADU), signal ${c.meanRel.toExponential(1)} apart`);
}
{
  const t = r.turns;
  if (!t || t.error) { console.log('FAIL', t ? t.spec : 'two scenes', t ? t.error : 'not run'); fail++; }
  else {
    const ok = t.same && t.distinct && t.resident >= 2;
    if (!ok) fail++;
    console.log(`${ok ? 'ok  ' : 'FAIL'}  ${t.spec}\n      two scenes in turn (${t.shape} FFT, ${t.planes} planes): A after B identical ${t.same}, ` +
      `${t.tA2.toFixed(0)} ms; ${t.resident} geometries resident`);
  }
}
console.log(fail ? `${fail} case(s) FAILED` : 'all WideField GPU checks passed');
process.exit(fail ? 1 : 0);
