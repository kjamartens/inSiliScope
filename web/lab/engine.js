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
import { World, PACK_BLOCK_CHUNKS, WORLD_VERSION } from '../prototype/scope/world.js';
import { renderScopeMovie, parseSpec, scopeDims } from '../prototype/scope/scope_movie.js';

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
  const row = c => [c.cx, c.cy, c.x, c.y, c.packRot, c.rOuter, c.height, c.nucOffX, c.nucOffY, c.nucRot, c.nucLong,
    c.nucShort, c.nucHeight, c.nucZ];
  // The packing blocks a rect's cell query touches (World::CellsInRect), with their cells as rows.
  function blocksOf(w, [x0, y0, x1, y1]) {
    const S = w.p.chunkSize, reach = w.cellReachUm(), B = PACK_BLOCK_CHUNKS, out = [];
    const bx0 = Math.floor(Math.floor((x0 - reach) / S) / B), bx1 = Math.floor(Math.floor((x1 + reach) / S) / B);
    const by0 = Math.floor(Math.floor((y0 - reach) / S) / B), by1 = Math.floor(Math.floor((y1 + reach) / S) / B);
    for (let bx = bx0; bx <= bx1; bx++) for (let by = by0; by <= by1; by++) {
      const cells = w.packedBlock(bx, by), rows = new Float64Array(cells.length * 14);
      cells.forEach((c, i) => rows.set(row(c), i * 14));
      out.push({ bx, by, rows });
    }
    return out;
  }
  // The pack worker's blocks a cell/sites job carries (iscEngine's inject).
  function inject(w, blocks) {
    if (blocks) for (const b of blocks) w.setPackedBlock(b.bx, b.by, b.rows);
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
      if (d.prepare) res.prepared = true;
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
      if (d.type === 'block') {
        inject(w, d.blocks);
        const cells = w.packedBlock(d.bx, d.by), rows = new Float64Array(cells.length * 14);
        cells.forEach((c, i) => rows.set(row(c), i * 14));
        return [{ type: 'block', id: d.id, key: d.key, bx: d.bx, by: d.by, rows, version: WORLD_VERSION }, [rows.buffer]];
      }
      if (d.type === 'pack') {
        inject(w, d.blocks);   // blocks the page remembered (its local storage), if any
        const [x0, y0, x1, y1] = d.rect;
        const list = w.cellsInRect(x0, y0, x1, y1), cells = new Float64Array(list.length * 14);
        list.forEach((c, i) => cells.set(row(c), i * 14));
        const blocks = blocksOf(w, d.rect);
        return [{ type: 'pack', id: d.id, key: d.key, win: d.win, cells, blocks, version: WORLD_VERSION },
                [cells.buffer, ...blocks.map(b => b.rows.buffer)]];
      }
      if (d.type === 'cell') {
        inject(w, d.blocks);
        const c = findCell(w, d.cx, d.cy);
        if (!c) return [{ type: 'cell', key: d.key, sig: d.sig, seed: d.seed, missing: true }, []];
        g.ensureCytoCacheFresh(w.seed, w.p);
        // The mesh interleaved as isc_cell_mesh writes it (x, y, h per vertex k*n + i); its outer ring is
        // the footprint outline (cellOutlineLocal at cytoTheta), so no outline field.
        const mesh = g.getCytoGeometry(c, w.p).mesh, nv = (mesh.rings + 1) * mesh.n;
        const flat = new Float64Array(nv * 3);
        for (let k = 0; k <= mesh.rings; k++) for (let i = 0; i < mesh.n; i++) {
          const v = 3 * (k * mesh.n + i), q = mesh.grid[k][i];
          flat[v] = q.x; flat[v + 1] = q.y; flat[v + 2] = q.h;
        }
        const out = { type: 'cell', key: d.key, sig: d.sig, seed: d.seed, rings: mesh.rings, n: mesh.n, mesh: flat, mts: null };
        const transfer = [flat.buffer];
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
        inject(w, d.blocks);
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
    if (e.data && e.data.type === 'init') return;   // the viewer's shared-WASM handshake: nothing to do here
    ready.then(eng => eng.handle(e.data)).then(([out, transfer]) => postMessage(out, transfer))
      .catch(err => setTimeout(() => { throw err; }));
  };
}
