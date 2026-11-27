// The viewer module's movie in strips (2026-11-20, docs/dev/LARGE_RECORDS.md part B): isc_movie_strips n hands
// every frame's rows strip by strip to Module.onMovieRows; put together they are the frames the whole movie streams
// (Module.onMovieFrame), bit for bit for blinks (the C++ side: ctest strips). isc_movie_plan says how a movie renders.
//   node tests/web/strips_unit.mjs        exit 0 = pass
import { readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { loadWasmScope } from '../parity/wasm_scope.mjs';

const t0 = performance.now();
const root = join(dirname(fileURLToPath(import.meta.url)), '..', '..');
const C = await loadWasmScope(readFileSync(join(root, 'web', 'insiliscope_module.js'), 'utf8'));
const M = C.M;
let failures = 0;
const check = (ok, what) => { console.log(`${ok ? 'ok  ' : 'FAIL'}  ${what}`); if (!ok) failures++; };
const str = s => { const n = M.lengthBytesUTF8(s) + 1, p = M._malloc(n); M.stringToUTF8(s, p, n); return p; };

function streamed(spec, stripRows) {
  const s = str(spec), info = M._malloc(24), err = M._malloc(512);
  const frames = [];
  let order = true, next = { f: 0, y: 0 }, W = 0, H = 0, N = 0;
  M.onMovieFrame = (f, px) => { frames[f] = px.slice(); return true; };
  M.onMovieRows = (f, y0, rows, px) => {
    W = M.HEAP32[info / 4]; H = M.HEAP32[info / 4 + 1]; N = M.HEAP32[info / 4 + 2];
    if (f !== next.f || y0 !== next.y) order = false;
    next = f + 1 === N ? { f: 0, y: y0 + rows } : { f: f + 1, y: y0 };
    if (!frames[f]) frames[f] = new Uint16Array(W * H);
    frames[f].set(px, y0 * W);
    return true;
  };
  M._isc_movie_stream(1);
  M._isc_movie_strips(stripRows);
  const rc = M._isc_scope_movie(s, 0, 0, info, err, 512);
  M._isc_movie_stream(0);
  M._isc_movie_strips(0);
  M.onMovieFrame = M.onMovieRows = null;
  const out = { rc, err: rc < 0 ? M.UTF8ToString(err) : '', frames, order: order && (!stripRows || next.y === H),
    w: M.HEAP32[info / 4], h: M.HEAP32[info / 4 + 1] };
  M._free(s); M._free(info); M._free(err);
  return out;
}
const same = (a, b) => a.length === b.length && a.every((f, i) => f && b[i] && f.length === b[i].length && f.every((v, k) => v === b[i][k]));

const spec = 'size=128 height=96 frames=3 psf-kernel-half-width-nm=2000 wf-kernel-um=2 fid-density=0.3';
const whole = streamed(spec, 0), strips = streamed(spec, 40);
check(whole.rc === 0 && strips.rc === 0, `streamed whole and in strips of 40 rows ${whole.err}${strips.err}`);
check(strips.order, 'every frame\'s rows once, strip by strip');
check(strips.w === 128 && strips.h === 96, `the sensor's size in info (${strips.w} x ${strips.h})`);
check(same(whole.frames, strips.frames), 'the strips put together = the whole movie, bit for bit');

function plan(spec, rows) {
  const s = str(spec), out = M._malloc(16);
  const rc = M._isc_movie_plan(s, rows, out);
  const r = Array.from(M.HEAP32.subarray(out / 4, out / 4 + 4));
  M._free(s); M._free(out);
  return rc === 0 ? r : null;
}
const p0 = plan('size=256 frames=10', -1), p1 = plan('size=6000 height=300 frames=2', -1), p2 = plan('size=256', 64);
check(p0 && p0[0] === 256 && p0[1] === 256 && p0[2] === 256, `256 px: no strips (${p0})`);
check(p1 && p1[0] === 6000 && p1[1] === 300 && p1[3] > 0, `6000 x 300 px: in strips, the sensor's size (${p1})`);
check(p2 && p2[2] === 64, `strips asked for: 64 rows (${p2})`);

console.log(`${failures ? 'FAILED' : 'PASS'} (${((performance.now() - t0) / 1000).toFixed(1)} s)`);
process.exit(failures ? 1 : 0);
