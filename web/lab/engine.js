// The viewer's engine (web/index.html iscEngine) on the JS reference instead of the WASM core: the same
// handle(d) -> [message, transferList] for the jobs 'pack', 'cell', 'sites', 'movie', the same reply fields and
// buffer types, so web/lab.html (= web/index.html served with self.ISC_ENGINE_URL, see serve.mjs) runs unchanged on
// it. Geometry from the prototype (web/prototype/index.html + microtubules.js, via load_prototype.mjs), the world
// and imaging from web/prototype/scope (World, renderScopeMovie). Edit those, reload, see it; port at merge.
//
//   const eng = await createEngine();                 // browser: fetches the prototype next to this file
//   const eng = await createEngine({ html, mt });     // Node: prototype texts given
//   const [msg, transfer] = eng.handle(job);
//
// Loaded as a module worker it answers postMessage(job) like the viewer's Blob workers (genWorkerMain).
import { loadPrototype } from '../../tests/parity/load_prototype.mjs';
import { World, PACK_BLOCK_CHUNKS } from '../prototype/scope/world.js';
import { renderScopeMovie, parseSpec, scopeDims } from '../prototype/scope/scope_movie.js';

const NUC_SLICES = 17, NUC_PTS = 48; // nucleus rings sent with each cell (both poles included)

async function text(url) {
  const r = await fetch(url, { cache: 'no-store' });
  if (!r.ok) throw new Error(`${url}: HTTP ${r.status}`);
  return r.text();
}

export async function createEngine(src = {}) {
  const base = new URL('../prototype/', import.meta.url);
  const [html, mt] = src.html ? [src.html, src.mt]
    : await Promise.all([text(new URL('index.html', base)), text(new URL('microtubules.js', base))]);
  const P = loadPrototype(html, mt);
  const g = P.gen;
  let world = null, worldKey = '';

  // One World per (seed, params), like iscEngine's useWorld. The viewer sends its params() output; the
  // prototype's params() normalises the keys it knows, and every other key (labelEfficiency,
  // labelNonBleaching, a slider only the viewer has so far) reaches the generator as sent.
  function useWorld(seed, p) {
    const key = (seed >>> 0) + '|' + Object.keys(p).sort().map(k => k + '=' + p[k]).join('|');
    if (key === worldKey) return world;
    const vals = { ...P.defaults };
    for (const k of Object.keys(p)) if (k in P.defaults) vals[k] = typeof P.defaults[k] === 'boolean' ? !!p[k] : p[k];
    const norm = P.paramsFrom(vals), params = { ...norm };
    for (const k of Object.keys(p)) if (!(k in norm)) params[k] = p[k];
    world = new World(P, seed >>> 0, params);
    worldKey = key;
    return world;
  }

  // World::FindCell: the cell of chunk (cx, cy) in its packing block, or null.
  function findCell(w, cx, cy) {
    const B = PACK_BLOCK_CHUNKS;
    for (const c of w.packedBlock(Math.floor(cx / B), Math.floor(cy / B))) if (c.cx === cx && c.cy === cy) return c;
    return null;
  }

  function movie(d) {
    const t0 = performance.now();
    try {
      const { width: W, height: H, frames: N } = scopeDims(parseSpec(d.spec));
      const frames = new Uint16Array(W * H * N);
      const info = renderScopeMovie(P, d.spec, (f, adu) => { frames.set(adu, f * W * H); });
      const half = info.halfTimeSec ?? 0;
      const res = { type: 'movie', id: d.id, spec: d.spec, rect: d.rect, frames, w: info.width, h: info.height,
        n: info.frames, blinks: info.blinks ?? 0, dyes: info.dyes ?? 0,
        halfMs: Number.isFinite(half) ? Math.trunc(Math.min(2e9, half * 1000)) : -1, ms: performance.now() - t0 };
      if (info.dyes != null) res.gpu = 'CPU'; // WideField: the JS reference has no WebGPU path
      return [res, [frames.buffer]];
    } catch (e) {
      return [{ type: 'movie', id: d.id, error: String(e && e.message || e) }, []];
    }
  }

  return {
    P,
    handle(d) {
      if (d.type === 'movie') return movie(d);
      const w = useWorld(d.seed, d.p);
      if (d.type === 'pack') {
        const [x0, y0, x1, y1] = d.rect;
        const list = w.cellsInRect(x0, y0, x1, y1), cells = new Float64Array(list.length * 14);
        list.forEach((c, i) => cells.set([c.cx, c.cy, c.x, c.y, c.packRot, c.rOuter, c.height, c.nucOffX, c.nucOffY,
          c.nucRot, c.nucLong, c.nucShort, c.nucHeight, c.nucZ], i * 14));
        return [{ type: 'pack', id: d.id, key: d.key, win: d.win, cells }, [cells.buffer]];
      }
      if (d.type === 'cell') {
        const c = findCell(w, d.cx, d.cy);
        if (!c) return [{ type: 'cell', key: d.key, sig: d.sig, seed: d.seed, missing: true }, []];
        const o = g.cellOutlineLocal(c, Math.max(8, Math.round(w.p.cytoTheta)));
        const outline = new Float64Array(o.length * 2);
        o.forEach((q, i) => { outline[2 * i] = q[0]; outline[2 * i + 1] = q[1]; });
        g.ensureCytoCacheFresh(w.seed, w.p);
        const mesh = g.getCytoGeometry(c, w.p).mesh, nv = (mesh.rings + 1) * mesh.n;
        const mx = new Float64Array(nv), my = new Float64Array(nv), mh = new Float64Array(nv);
        for (let k = 0; k <= mesh.rings; k++) for (let i = 0; i < mesh.n; i++) {
          const v = k * mesh.n + i, q = mesh.grid[k][i];
          mx[v] = q.x; my[v] = q.y; mh[v] = q.h;
        }
        // Nucleus surface rings (cell-local x, y, z; the viewer draws these when present, else its ellipsoid).
        const rings = g.nucleusRingsLocal(c, NUC_SLICES, NUC_PTS), nuc = new Float64Array(rings.length * NUC_PTS * 3);
        rings.forEach((r, k) => r.forEach((q, i) => nuc.set([q.x, q.y, q.z], (k * NUC_PTS + i) * 3)));
        const out = { type: 'cell', key: d.key, sig: d.sig, seed: d.seed, rings: mesh.rings, n: mesh.n, outline, mx, my, mh, mts: null,
          nuc, nucPts: NUC_PTS };
        const transfer = [outline.buffer, mx.buffer, my.buffer, mh.buffer, nuc.buffer];
        if (d.mt) {
          const paths = w.cellAssets(c).mts;
          let total = 0;
          for (const pts of paths) total += pts.length;
          const xyz = new Float64Array(total * 3), lens = new Uint32Array(paths.length);
          let k = 0;
          paths.forEach((pts, i) => { lens[i] = pts.length; for (const q of pts) { xyz[k++] = q.x; xyz[k++] = q.y; xyz[k++] = q.z; } });
          out.mts = { xyz, lens };
          transfer.push(xyz.buffer, lens.buffer);
        }
        return [out, transfer];
      }
      if (d.type === 'sites') {
        const [x0, y0, x1, y1] = d.rect;
        const list = w.sitesInWindow(x0, y0, x1, y1, -Infinity, Infinity), sites = new Float64Array(list.length * 4);
        list.forEach((s, i) => { sites[4 * i] = s.x; sites[4 * i + 1] = s.y; sites[4 * i + 2] = s.z; sites[4 * i + 3] = s.id; });
        return [{ type: 'sites', id: d.id, key: d.key, rect: d.rect, sites }, [sites.buffer]];
      }
      throw new Error('unknown job ' + d.type);
    },
  };
}

// As a module worker: the viewer's message loop (genWorkerMain), errors surface as the worker's onerror.
if (typeof WorkerGlobalScope !== 'undefined' && self instanceof WorkerGlobalScope) {
  const ready = createEngine();
  self.onmessage = e => {
    ready.then(eng => eng.handle(e.data)).then(([out, transfer]) => postMessage(out, transfer))
      .catch(err => setTimeout(() => { throw err; }));
  };
}
