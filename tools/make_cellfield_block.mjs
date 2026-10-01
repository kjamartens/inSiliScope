#!/usr/bin/env node
// Builds the webSMLM block (M4): dist/cellfield_block.js, a drop-in replacement for webSMLM's
// `const CellField=(function(){...})();` IIFE, generated from the insiliscope_block target (the core's
// C ABI, WASM inlined, instantiated synchronously) plus the thin JS wrapper below. Same entry point
// and return shape as the IIFE it replaces: CellField.buildWindow(w, h, opts).
//
//   node tools/make_cellfield_block.mjs                    # (re)write dist/cellfield_block.js
//   node tools/make_cellfield_block.mjs --check <file>     # exit 1 unless <file>'s module is this build's
//   node tools/make_cellfield_block.mjs --out <file>
//
// Build first: source ~/emsdk/emsdk_env.sh && cmake --preset wasm && cmake --build --preset wasm
// The header records the source commit (GITHUB_SHA in CI, else git HEAD, "+dirty" if the tree has
// changes), the Emscripten version, the ABI and sha256 of the module text; --check compares only
// the module, so a block built from the same sources at another commit still passes.
import crypto from 'node:crypto';
import { execSync } from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const SRC = path.join(ROOT, 'build/wasm/core/insiliscope_block.js');
const args = process.argv.slice(2);
const argVal = k => { const i = args.indexOf(k); return i >= 0 ? args[i + 1] : null; };
const OUT = path.resolve(argVal('--out') || path.join(ROOT, 'dist/cellfield_block.js'));
const BEGIN = '// ==== BEGIN insiliscope CellField block ====';
const END = '// ==== END insiliscope CellField block ====';

const mod = fs.readFileSync(SRC, 'utf8');
if (/[^\x09\x0a\x0d\x20-\x7e]/.test(mod)) throw new Error('module is not plain ASCII (SINGLE_FILE_BINARY_ENCODE=0?)');
const sha = crypto.createHash('sha256').update(mod).digest('hex');

const checkFile = argVal('--check');
if (checkFile) {
  const text = fs.readFileSync(checkFile, 'utf8');
  const got = (/^\/\/ sha256\(module\): ([0-9a-f]{64})$/m.exec(text) || [])[1];
  const lit = (/^\)\((".*")\);$/m.exec(text) || [])[1];
  const actual = lit ? crypto.createHash('sha256').update(JSON.parse(lit)).digest('hex') : null;
  if (got !== actual) { console.error(`${checkFile}: header checksum does not match its module`); process.exit(1); }
  if (actual !== sha) { console.error(`${checkFile}: module ${String(actual).slice(0, 12)} != this build ${sha.slice(0, 12)}`); process.exit(1); }
  console.log(`${checkFile} matches this build (sha256 ${sha.slice(0, 12)})`);
  process.exit(0);
}

const sh = c => { try { return execSync(c, { cwd: ROOT, stdio: ['ignore', 'pipe', 'ignore'] }).toString().trim(); } catch { return ''; } };
const repo = process.env.GITHUB_REPOSITORY ? `${process.env.GITHUB_SERVER_URL || 'https://github.com'}/${process.env.GITHUB_REPOSITORY}`
                                           : 'https://github.com/kjamartens/insiliscope';
const commit = process.env.GITHUB_SHA || ((sh('git rev-parse HEAD') || 'unknown') +
  (sh('git status --porcelain -- core cli tools CMakeLists.txt CMakePresets.json') ? '+dirty' : ''));
const abi = /#define ISC_ABI_VERSION (\d+)/.exec(fs.readFileSync(path.join(ROOT, 'core/include/insiliscope/insiliscope.h'), 'utf8'))[1];
const cache = fs.readFileSync(path.join(ROOT, 'build/wasm/CMakeCache.txt'), 'utf8');
const tc = (/CMAKE_TOOLCHAIN_FILE:\w+=(.+)/.exec(cache) || [])[1];
const verFile = tc && path.resolve(path.dirname(tc.trim()), '../../../emscripten-version.txt');
const emver = verFile && fs.existsSync(verFile) ? fs.readFileSync(verFile, 'utf8').trim().replace(/"/g, '') : 'unknown';

// The wrapper. Kept ES2017 and self-contained: CellField.workerSource() re-evaluates this very
// function (with the module text) inside a Web Worker.
function cellFieldFactory(SRC) {
  'use strict';
  const ABI = __ABI__;
  // webSMLM's CF_PARAMS (cell_field_sim defaults as tuned there, build 2026-09-24f), passed
  // explicitly so a change of the core's own defaults cannot move webSMLM's field. Every lattice
  // site carries a dye (labelEfficiency 1), as in the JS it replaces: webSMLM applies its own
  // labelling efficiency to the returned sites.
  const DEFAULTS = {
    chunkSize: 26, jitter: 0.8, density: 0.33,
    cellDiamMin: 25, cellDiamMax: 35, cellElongMin: 0.5, cellElongMax: 1, cellBlob: 1.75, cellRough: 0.15, cellFractalDim: 1.35,
    cellHeightMin: 3, cellHeightMax: 6,
    nucLongMin: 8, nucLongMax: 12, nucRatioMin: 0.6, nucRatioMax: 1, nucHeightMin: 0.3, nucHeightMax: 0.5,
    nucOffsetFrac: 0.1, nucMargin: 0.6,
    cytoRimHeightMin: 0.1, cytoRimHeightMax: 0.3, cytoEdgeRiseMin: 0.1, cytoEdgeRiseMax: 0.5,
    cytoMidHeightMin: 1, cytoMidHeightMax: 2, cytoMidDistanceMin: 0.1, cytoMidDistanceMax: 0.3,
    cytoMaxSlope: 1, cytoDomeSlope: 3, cytoRelaxUm: 1, cytoRings: 60, cytoTheta: 256,
    enablePacking: 1, allowPackRotation: 1, packFrac: 1.0, relaxIters: 80, relaxDamping: 0.55,
    mtDensity: 0.9, mtStartFracMin: 0, mtStartFracMax: 0.3, mtStartOffsetXY: 0,
    mtEndFracMin: 0.01, mtEndFracMax: 0.4, mtEndJitterDeg: 145, mtWobbleTurn: 0.8, mtWobbleFactor: 1.05,
    mtStepLen: 0.05, mtSmoothLen: 1.5, mtMinTurnRadius: 0.15, mtMinSeparation: 0.05, mtMaxZSlope: 5,
    labelEfficiency: 1, labelNonBleaching: 0,
  };
  const PAD_UM = (12.5 + 12 + 5) / 1000;   // MT radius + binder + max linker: dye reach from the centreline
  let M = null, world = 0, worldKey = '';

  function mod() {
    if (M) return M;
    M = new Function('var Module={};\n' + SRC + '\n;return Module;')();
    if (!M._isc_abi_version || M._isc_abi_version() !== ABI) throw new Error('CellField: WASM module ABI mismatch');
    return M;
  }
  function cstr(s) {
    const n = M.lengthBytesUTF8(s) + 1, p = M._malloc(n);
    M.stringToUTF8(s, p, n);
    return p;
  }
  function getWorld(seed, p) {
    const key = seed + JSON.stringify(p);
    if (world && key === worldKey) return world;
    if (world) M._isc_world_free(world);
    const hp = M._isc_params_new();
    for (const k of Object.keys(p)) {
      const s = cstr(k);
      const r = M._isc_params_set(hp, s, +p[k]);
      M._free(s);
      if (r !== 0) { M._isc_params_free(hp); world = 0; throw new Error('CellField: unknown parameter ' + k); }
    }
    world = M._isc_world_new(seed >>> 0, hp);
    M._isc_params_free(hp);
    worldKey = key;
    return world;
  }
  // Calls fn(ptr, cap) with a double buffer, growing it until the result fits; returns [n, copy].
  function query(stride, fn) {
    let cap = 4096;
    for (;;) {
      const ptr = M._malloc(cap * stride * 8);
      const n = fn(ptr, cap);
      if (n <= cap) {
        const out = n > 0 ? M.HEAPF64.slice(ptr / 8, ptr / 8 + n * stride) : new Float64Array(0);
        M._free(ptr);
        if (n < 0) throw new Error('CellField: bad window');
        return [n, out];
      }
      M._free(ptr);
      cap = n;
    }
  }
  function countMts(w, cells, win) {
    let nMt = 0, capPts = 1 << 16, capMts = 1024;
    for (let c = 0; c < cells.length; c += 14) {
      const cx = cells[c], cy = cells[c + 1], x = cells[c + 2], y = cells[c + 3], rot = cells[c + 4];
      let nm, tot, xyz, lens;
      for (;;) {
        const pXyz = M._malloc(capPts * 24), pLens = M._malloc(capMts * 4), pTot = M._malloc(4);
        nm = M._isc_cell_microtubules(w, cx, cy, pXyz, capPts, pLens, capMts, pTot);
        tot = M.HEAP32[pTot >> 2];
        if (nm >= 0 && nm <= capMts && tot <= capPts) {
          xyz = M.HEAPF64.slice(pXyz / 8, pXyz / 8 + tot * 3);
          lens = M.HEAP32.slice(pLens >> 2, (pLens >> 2) + nm);
        }
        M._free(pXyz); M._free(pLens); M._free(pTot);
        if (nm < 0) break;
        if (xyz) break;
        capPts = Math.max(capPts, tot); capMts = Math.max(capMts, nm);
      }
      if (!xyz) continue;
      const cr = Math.cos(rot), sr = Math.sin(rot);
      let k = 0;
      for (let i = 0; i < nm; i++) {
        let hit = false, px = 0, py = 0, pz = 0;
        for (let j = 0; j < lens[i]; j++, k++) {
          const lx = xyz[3 * k], ly = xyz[3 * k + 1], z = xyz[3 * k + 2];
          const X = x + lx * cr - ly * sr, Y = y + lx * sr + ly * cr;
          if (j > 0 && !hit)
            hit = Math.max(px, X) + PAD_UM >= win.x0 && Math.min(px, X) - PAD_UM <= win.x1 &&
                  Math.max(py, Y) + PAD_UM >= win.y0 && Math.min(py, Y) - PAD_UM <= win.y1 &&
                  Math.max(pz, z) + PAD_UM >= win.zLo && Math.min(pz, z) - PAD_UM <= win.zHi;
          px = X; py = Y; pz = z;
        }
        if (hit) nMt++;
      }
    }
    return nMt;
  }

  // Same contract as the JS IIFE it replaces. w,h = structure canvas (px); the window is w x h px
  // (pxnm each) centred on world (xUm, yUm) um. Returns { sites: [[x px, y px, z nm], ...], nCells,
  // nMt, removed, packed }: z measured from focusUm above the coverslip, sites within +-slabNm of it.
  // nCells = cells whose footprint reaches within 0.5 um of the window; nMt = microtubules whose
  // centreline comes within dye reach of the window (slab included); removed = null (cells are
  // packed on fixed 8x8-chunk blocks, independent of the window, so a pan never re-packs; the
  // per-window prune count no longer exists); packed = packing on.
  // Optional opts.params: overrides of DEFAULTS (core parameter names).
  function buildWindow(w, h, o) {
    mod();
    const seed = o.seed | 0, pxUm = o.pxnm / 1000;
    const p = Object.assign({}, DEFAULTS, o.params || {}, { mtDensity: o.mtDensity, density: o.cellDensity });
    const wd = getWorld(seed, p);
    const halfW = w * pxUm / 2, halfH = h * pxUm / 2, slabUm = o.slabNm / 1000;
    const win = { x0: o.xUm - halfW, x1: o.xUm + halfW, y0: o.yUm - halfH, y1: o.yUm + halfH,
                  zLo: o.focusUm - slabUm, zHi: o.focusUm + slabUm };
    const [n, buf] = query(4, (ptr, cap) =>
      M._isc_sites_in_window(wd, win.x0, win.y0, win.x1, win.y1, win.zLo, win.zHi, ptr, cap));
    const sites = new Array(n);
    for (let i = 0; i < n; i++) {
      const X = buf[4 * i], Y = buf[4 * i + 1], Z = buf[4 * i + 2];
      sites[i] = [w / 2 + (X - o.xUm) / pxUm, h / 2 + (Y - o.yUm) / pxUm, (Z - o.focusUm) * 1000];
    }
    const [nCells, cells] = query(14, (ptr, cap) =>
      M._isc_cells_in_window(wd, win.x0 - 0.5, win.y0 - 0.5, win.x1 + 0.5, win.y1 + 0.5, ptr, cap));
    return { sites, nCells, nMt: countMts(wd, cells, win), removed: null, packed: !!p.enablePacking };
  }
  // Frees the cached world (it is rebuilt on the next call).
  function dispose() { if (world) M._isc_world_free(world); world = 0; worldKey = ''; }
  // JS source text that evaluates to an independent CellField (for a Web Worker).
  function workerSource() { return '(' + cellFieldFactory.toString() + ')(' + JSON.stringify(SRC) + ')'; }

  return { buildWindow, dispose, workerSource, defaults: Object.assign({}, DEFAULTS), abiVersion: ABI };
}

const factory = cellFieldFactory.toString().replace('__ABI__', abi);
const text = `${BEGIN}
// GENERATED by tools/make_cellfield_block.mjs in ${repo} -- do not edit; regenerate instead.
// source: ${repo}/tree/${commit}
// commit: ${commit}
// emscripten: ${emver}; core C ABI ${abi}
// sha256(module): ${sha}
// licence: BSD-3-Clause, ${repo}/blob/${commit}/LICENSE
// rebuild: cmake --preset wasm && cmake --build --preset wasm && node tools/make_cellfield_block.mjs
// Replaces webSMLM's CellField IIFE: the insiliscope core (C++ -> WASM, the module text in the
// last line, WASM inlined) + a thin wrapper. CellField.buildWindow(w, h, opts) as before
// (see the comment on buildWindow); the module is instantiated synchronously on first use.
// CellField.workerSource() gives source text that evaluates to a CellField inside a Web Worker.
// Needs a browser main thread or worker (in Node, fake one: globalThis.WorkerGlobalScope,
// self, location). The sha256 above covers the JSON-decoded module string.
const CellField=(${factory}
)(${JSON.stringify(mod)});
${END}
`;
fs.mkdirSync(path.dirname(OUT), { recursive: true });
fs.writeFileSync(OUT, text);
console.log(`wrote ${path.relative(ROOT, OUT)}: ${(text.length / 1024).toFixed(1)} KB, ABI ${abi}, emscripten ${emver}, commit ${commit.slice(0, 12)}, sha256 ${sha.slice(0, 12)}`);
