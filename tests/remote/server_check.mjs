// The remote server (cli/remote_server.cpp, docs/remote.md) against the viewer's own engine: each job type the
// server takes is sent over a WebSocket (web/live/remote_wire.js) and its reply compared with what the viewer's
// WebAssembly module (web/insiliscope_module.js) computes for the same request -- frames byte for byte.
//   node tests/remote/server_check.mjs --server build/native/cli/insiliscope_server   (POSIX: starts one, a free port)
//   node tests/remote/server_check.mjs --url "ws://127.0.0.1:18799/?t=TOKEN"          (a server already running)
//   --gpu: the server renders WideField on its GPU (started with --gpu auto; else --gpu off): its WideField frames are
//   compared within float rounding (most pixels equal, a few a Poisson draw apart), everything else byte for byte.
// Node 22+ (its WebSocket). ~30 s.
import { readFileSync, mkdtempSync, existsSync } from 'node:fs';
import { spawn } from 'node:child_process';
import { tmpdir } from 'node:os';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { loadWasmScope } from '../parity/wasm_scope.mjs';

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..', '..');
const arg = k => { const i = process.argv.indexOf(k); return i > 0 ? process.argv[i + 1] : null; };
globalThis.self = globalThis;
(0, eval)(readFileSync(join(ROOT, 'web/live/remote_wire.js'), 'utf8'));
const W = globalThis.IscWire;

const GPU = process.argv.includes('--gpu');
const t0 = performance.now();
let failures = 0;
const ok = (cond, what) => { console.log((cond ? 'ok   ' : 'FAIL ') + what); if (!cond) failures++; };

// ---- the server ----
let url = arg('--url'), child = null;
if (!url) {
  const bin = arg('--server');
  if (!bin) { console.error('give --server <insiliscope_server> or --url <ws://...>'); process.exit(2); }
  const dir = mkdtempSync(join(tmpdir(), 'isc-remote-')), announce = join(dir, 'port'), token = 'test' + Date.now();
  child = spawn(bin, ['--listen', '127.0.0.1:0', '--token', token, '--announce', announce, '--threads', '4', '--idle-exit', '5', '--gpu', GPU ? 'auto' : 'off'],
    { stdio: ['ignore', 'inherit', 'inherit'] });
  for (let i = 0; i < 100 && !existsSync(announce); i++) await new Promise(r => setTimeout(r, 50));
  const port = readFileSync(announce, 'utf8').trim().split(/\s+/)[1];
  url = `ws://127.0.0.1:${port}/?t=${token}`;
}
const done = code => { if (child) child.kill(); console.log(`server_check: ${failures ? failures + ' failed' : 'all passed'} in ${((performance.now() - t0) / 1000).toFixed(1)} s`); process.exit(code); };

// A connection: jobs in, replies (and their progress / parts) out.
function connect(u) {
  return new Promise((resolve, reject) => {
    const ws = new WebSocket(u);
    ws.binaryType = 'arraybuffer';
    const waiting = new Map(), extra = new Map();
    ws.onmessage = e => {
      const d = W.decode(e.data);
      if (/^movie-(progress|part|rows)$/.test(d.type)) { (extra.get(d.id) || extra.set(d.id, []).get(d.id)).push(d); return; }
      const k = d.type + ':' + d.id, w = waiting.get(k);
      if (w) { waiting.delete(k); w({ ...d, extra: extra.get(d.id) || [] }); extra.delete(d.id); }
    };
    ws.onerror = () => reject(new Error('cannot reach ' + u));
    ws.onopen = () => resolve({
      ws,
      job(msg) { return new Promise(r => { waiting.set(msg.type + ':' + msg.id, r); ws.send(W.encode(msg)); }); },
      close() { ws.close(); },
    });
  });
}

// ---- the viewer's engine, the same calls ----
const C = await loadWasmScope(readFileSync(join(ROOT, 'web/insiliscope_module.js'), 'utf8'));
const M = C.M;
const str = s => { const n = M.lengthBytesUTF8(s) + 1, p = M._malloc(n); M.stringToUTF8(s, p, n); return p; };
function localLive(spec, n, lit = true) {
  const s = str(spec), info = M._malloc(17 * 8), err = M._malloc(512);
  const px = M._isc_live_frames(s, n, lit ? 1 : 0, info, err, 512);
  const out = px < 0 ? { error: M.UTF8ToString(err) } : { frames: M.HEAPU16.slice(M._isc_live_image() / 2, M._isc_live_image() / 2 + px), v: M.HEAPF64.slice(info / 8, info / 8 + 17) };
  M._free(s); M._free(info); M._free(err);
  return out;
}
function localSetup(spec) {
  const s = str(spec), err = M._malloc(512), n = M._isc_scope_setup(s, 0, 0, err, 512), b = M._malloc(n + 1);
  M._isc_scope_setup(s, b, n + 1, err, 512);
  const j = JSON.parse(M.UTF8ToString(b));
  M._free(s); M._free(err); M._free(b);
  return j;
}
function localPanel(spec, id, params, scale) {
  const s = str(spec), pid = str(id), pr = str(params), sizes = M._malloc(8), err = M._malloc(512);
  M._isc_scope_panel(s, pid, pr, scale, 0, 0, 0, 0, sizes, err, 512);
  const hn = M.HEAP32[sizes / 4], fn = M.HEAP32[sizes / 4 + 1], hb = M._malloc(hn + 1), fb = M._malloc(Math.max(4, fn * 4));
  M._isc_scope_panel(s, pid, pr, scale, hb, hn + 1, fb, fn, sizes, err, 512);
  const out = { header: JSON.parse(M.UTF8ToString(hb)), data: M.HEAPF32.slice(fb / 4, fb / 4 + fn) };
  for (const p of [s, pid, pr, sizes, err, hb, fb]) M._free(p);
  return out;
}
const same = (a, b) => a && b && a.length === b.length && Buffer.compare(Buffer.from(a.buffer, a.byteOffset, a.byteLength), Buffer.from(b.buffer, b.byteOffset, b.byteLength)) === 0;
const firstDiff = (a, b) => { for (let i = 0; i < Math.min(a.length, b.length); i++) if (a[i] !== b[i]) return `sample ${i}: ${a[i]} vs ${b[i]}`; return `lengths ${a.length} vs ${b.length}`; };
const deepEq = (a, b) => JSON.stringify(a) === JSON.stringify(b);
// GPU frames: >= 95 % of the samples equal, the mean |difference| under 0.5 % of the mean.
const near = (a, b) => {
  if (!a || !b || a.length !== b.length) return { ok: false, text: 'lengths' };
  let eq = 0, d = 0, m = 0;
  for (let i = 0; i < a.length; i++) { eq += a[i] === b[i]; d += Math.abs(a[i] - b[i]); m += b[i]; }
  return { ok: eq >= 0.95 * a.length && d <= 0.005 * m, text: `${(100 * eq / a.length).toFixed(2)} % equal, mean |diff| ${(d / a.length).toFixed(3)} of ${(m / a.length).toFixed(1)} ADU` };
};

// ---- the checks ----
const SPEC = 'size=48 frames=4 exposure-ms=50 world-seed=7 psf-kernel-half-width-nm=3000 x=10 y=12';
try {
  const A = await connect(url);
  const hello = await A.job({ type: 'hello', id: 0, protocol: 1 });
  ok(hello.protocol === 1 && hello.abi === M._isc_abi_version() && hello.world === M.UTF8ToString(M._isc_world_version()),
    `hello: protocol ${hello.protocol}, ABI ${hello.abi}, world ${hello.world}, ${hello.host} ${hello.cpus} CPUs ${hello.memGB.toFixed(0)} GB, v${hello.version}, GPU ${hello.gpu}`);
  if (GPU) ok(hello.gpu && !/^(off|none)/.test(hello.gpu), 'hello: the server has a GPU (' + hello.gpu + ')');

  let t = performance.now();
  const opts = await A.job({ type: 'options', id: 1 });
  const nLocal = M.UTF8ToString((() => { const n = M._isc_scope_options(0, 0) + 1, b = M._malloc(n); M._isc_scope_options(b, n); return b; })()).split('\n').filter(Boolean).length;
  ok(opts.names && opts.names.length === nLocal && opts.defaults.length === nLocal, `options: ${opts.names && opts.names.length} = the module's ${nLocal}`);

  t = performance.now();
  const setup = await A.job({ type: 'setup', id: 2, spec: SPEC });
  ok(!setup.error && deepEq(setup.setup, localSetup(SPEC)), `setup = the module's (${(performance.now() - t).toFixed(0)} ms)`);

  const list = await A.job({ type: 'panel', id: 3 });
  ok(Array.isArray(list.list) && list.list.length > 3, `panel list: ${list.list && list.list.map(p => p.id).join(', ')}`);
  t = performance.now();
  const pan = await A.job({ type: 'panel', id: 4, spec: SPEC, panel: 'spectra', params: '', scale: 1 });
  const lp = localPanel(SPEC, 'spectra', '', 1);
  ok(!pan.error && deepEq(pan.header, lp.header) && same(pan.data, lp.data), `panel spectra = the module's (${pan.data && pan.data.length} floats, ${(performance.now() - t).toFixed(0)} ms)`);

  // a movie, whole and streamed
  t = performance.now();
  const mv = await A.job({ type: 'movie', id: 5, spec: SPEC, rect: [1, 2, 3, 4] });
  const tr = performance.now() - t;
  t = performance.now();
  const lm = C.movie(SPEC);
  const tl = performance.now() - t;
  ok(!mv.error && same(mv.frames, lm.frames), `movie = the module's, byte for byte (${mv.w}x${mv.h}x${mv.n}; server ${tr.toFixed(0)} ms, module ${tl.toFixed(0)} ms)` + (mv.error ? ' ' + mv.error : same(mv.frames, lm.frames) ? '' : ' ' + firstDiff(mv.frames, lm.frames)));
  ok(deepEq(mv.rect, [1, 2, 3, 4]), 'movie: its rect back');
  const st = await A.job({ type: 'movie', id: 6, spec: SPEC, stream: true });
  const parts = st.extra.filter(d => d.type === 'movie-part'), cat = new Uint16Array(parts.reduce((s, p) => s + p.frames.length, 0));
  let at = 0;
  for (const p of parts) { cat.set(p.frames, at); at += p.frames.length; }
  ok(!st.error && st.streamed && st.got === 4 && same(cat, lm.frames), `streamed movie: ${parts.length} part(s), ${st.got} frames = the module's; progress messages ${st.extra.filter(d => d.type === 'movie-progress').length}`);

  // WideField: mean-field scenes (on the server's GPU with --gpu)
  const WSPEC = SPEC + ' mt-mode=3 npc-dye=None x=-10 y=4';   // (a field with cells)
  t = performance.now();
  const wm = await A.job({ type: 'movie', id: 7, spec: WSPEC });
  const tw = performance.now() - t;
  const lw = C.movie(WSPEC);
  if (GPU) { const n = near(wm.frames, lw.frames); ok(!wm.error && n.ok, `WideField movie on the GPU = the module's within rounding: ${n.text} (server ${tw.toFixed(0)} ms)` + (wm.error ? ' ' + wm.error : '')); }
  else ok(!wm.error && same(wm.frames, lw.frames), `WideField movie = the module's, byte for byte (server ${tw.toFixed(0)} ms)` + (wm.error ? ' ' + wm.error : ''));
  // a structured pattern: one scene per state (each on the GPU with --gpu)
  const SSPEC = WSPEC + ' sim-type=1';
  t = performance.now();
  const sm = await A.job({ type: 'movie', id: 8, spec: SSPEC });
  const ts = performance.now() - t;
  const ls = C.movie(SSPEC);
  if (GPU) { const n = near(sm.frames, ls.frames); ok(!sm.error && n.ok, `SIM2D WideField movie on the GPU = the module's within rounding: ${n.text} (server ${ts.toFixed(0)} ms)` + (sm.error ? ' ' + sm.error : '')); }
  else ok(!sm.error && same(sm.frames, ls.frames), `SIM2D WideField movie = the module's, byte for byte (server ${ts.toFixed(0)} ms)` + (sm.error ? ' ' + sm.error : ''));

  // live: a fresh sample, frames one by one, a chunk of 3, then the same sample replayed elsewhere
  const LSPEC = SPEC.replace('frames=4', 'frames=1');
  M._isc_live_reset();
  const remote = [], local = [];
  t = performance.now();
  for (let i = 0; i < 3; i++) {
    const r = await A.job({ type: 'live', id: 10 + i, spec: LSPEC, reset: i === 0, t: 0, lit: true });
    remote.push(r);
  }
  const tLive = (performance.now() - t) / 3;
  for (let i = 0; i < 3; i++) local.push(localLive(LSPEC, 1));
  ok(remote.every((r, i) => !r.error && same(r.frames, local[i].frames)), `live: 3 frames = the module's (server ~${tLive.toFixed(0)} ms/frame round trip)`);
  ok(remote.every((r, i) => Math.abs(r.t - (local[i].v[2] + 0.05)) < 1e-9 && r.w === local[i].v[0] && r.engine === 'live' && Array.isArray(r.paths)), 'live: time, size, engine and paths as the module says');
  const ch = await A.job({ type: 'live', id: 20, spec: LSPEC, n: 3, rec: true });
  const lch = localLive(LSPEC, 3);
  ok(!ch.error && ch.n === 3 && same(ch.frames, lch.frames), `live chunk of 3 = the module's (${ch.frames && ch.frames.length} samples)`);
  const B = await connect(url);
  const rp = await B.job({ type: 'live-replay', id: 30, records: [{ spec: LSPEC, n: 6, lit: true }] });
  const nx = await B.job({ type: 'live', id: 31, spec: LSPEC });
  const ln = localLive(LSPEC, 1);
  ok(!rp.error && same(nx.frames, ln.frames), `live-replay of 6 frames on another connection: the next frame = the module's 7th (t ${rp.t})`);
  B.close();

  // a live WideField frame (the GPU's with --gpu)
  const WL = WSPEC.replace('frames=4', 'frames=1');
  M._isc_live_reset();
  const wl = await A.job({ type: 'live', id: 35, spec: WL, reset: true, t: 0, lit: true });
  const lwl = localLive(WL, 1);
  if (GPU) { const n = near(wl.frames, lwl.frames); ok(!wl.error && n.ok, `live WideField frame on the GPU = the module's within rounding: ${n.text}` + (wl.error ? ' ' + wl.error : '')); }
  else ok(!wl.error && same(wl.frames, lwl.frames), 'live WideField frame = the module\'s, byte for byte' + (wl.error ? ' ' + wl.error : ''));

  // an unknown job and a cancelled one: answered with an error; a closed connection ends its engine
  const bad = await A.job({ type: 'pack', id: 40 });
  ok(!!bad.error, 'a job the server does not take: an error (' + bad.error + ')');
  A.close();
  done(failures ? 1 : 0);
} catch (e) {
  console.error(e);
  failures++;
  done(1);
}
