// Runs the parity cases through the JS prototype (the reference
// implementation) and prints them in parity_main.cpp's line format.
//   node js_reference.mjs <prototype index.html> <cases.txt> <out.txt>
//
// Loads the DOM-free generator half of index.html plus microtubules.js via
// load_prototype.mjs. Default params come from the page's own <input>
// defaults, normalised like its params().
import fs from 'fs';
import path from 'path';
import { loadPrototype } from './load_prototype.mjs';
import { OrigamiField, shapeOutline, moleculeSites } from '../../web/prototype/scope/origami.js';
import { npcDyesInBlock, laminDyesInBlock, normLinkage, LEGACY_LINKAGE } from '../../web/prototype/scope/dyes.js';
import { FiducialField } from '../../web/prototype/scope/fiducials.js';

const [protoPath, casesPath, outPath] = process.argv.slice(2);
// V8 < 13 computes Math.pow with its own fdlibm port; 13+ calls std::pow
// (--use-std-math-pow), which is what core's jsm::pow is. The cytoplasm mesh
// uses pow, so an older Node gives a reference core cannot match exactly.
if (+process.versions.v8.split('.')[0] < 13)
  console.warn(`warning: V8 ${process.versions.v8} uses fdlibm Math.pow; use Node >= 24 for the reference`);
const html = fs.readFileSync(protoPath, 'utf8');
// microtubules.js is a sibling of index.html.
const mtSrc = fs.readFileSync(path.join(path.dirname(protoPath), 'microtubules.js'), 'utf8');
const proto = loadPrototype(html, mtSrc);
const g = proto.gen;

// ---- defaults from the page's inputs ----
const PARAM_KEYS = ['chunkSize', 'jitter', 'density', 'cellDiamMin', 'cellDiamMax', 'cellElongMin', 'cellElongMax',
  'cellBlob', 'cellRough', 'cellFractalDim', 'nucLongMin', 'nucLongMax', 'nucRatioMin', 'nucRatioMax',
  'nucHeightMin', 'nucHeightMax', 'nucOffsetFrac', 'nucMargin', 'nucBaseMin', 'nucBaseMax', 'nucIrregMin', 'nucIrregMax',
  'nucBendMin', 'nucBendMax', 'nucSmooth', 'nucThickIrreg', 'nucAsym', 'nucWidestMin', 'nucWidestMax',
  'cytoRimHeightMin', 'cytoRimHeightMax',
  'cytoEdgeRiseMin', 'cytoEdgeRiseMax', 'cytoMidHeightMin', 'cytoMidHeightMax', 'cytoMidDistanceMin',
  'cytoMidDistanceMax', 'cytoMaxSlope', 'cytoDomeSlope', 'cytoRelaxUm', 'cytoRings', 'cytoTheta',
  'enablePacking', 'allowPackRotation', 'packFrac', 'relaxIters', 'relaxDamping',
  'mtDensity', 'mtStartDecayPct', 'mtEndDecayPct', 'mtDirKappa', 'mtWobbleTurn', 'mtWobbleFactor', 'mtStepLen', 'mtSmoothLen', 'mtMinTurnRadius',
  'mtMinSeparation', 'mtMaxZSlope', 'npcDensity', 'npcMinDistNm', 'npcRadiusNm', 'npcRingSepNm', 'npcCornerSpreadNm',
  'laminDensity', 'laminNpcExclNm'];
const defaults = {};
for (const k of PARAM_KEYS) if (k in proto.defaults) defaults[k] = proto.defaults[k];
const missing = PARAM_KEYS.filter(k => !(k in defaults));
if (missing.length) throw new Error('defaults not found in page: ' + missing.join(','));

function normalise(p) {
  for (const [lo, hi] of [['cellDiamMin', 'cellDiamMax'], ['cellElongMin', 'cellElongMax'],
    ['nucLongMin', 'nucLongMax'], ['nucRatioMin', 'nucRatioMax'], ['nucBaseMin', 'nucBaseMax'],
    ['nucIrregMin', 'nucIrregMax'], ['nucBendMin', 'nucBendMax'], ['nucWidestMin', 'nucWidestMax'],
    ['nucHeightMin', 'nucHeightMax'], ['cytoRimHeightMin', 'cytoRimHeightMax'], ['cytoEdgeRiseMin', 'cytoEdgeRiseMax'],
    ['cytoMidHeightMin', 'cytoMidHeightMax'], ['cytoMidDistanceMin', 'cytoMidDistanceMax']])
    p[hi] = Math.max(p[lo], p[hi]);
  // floors from params()
  p.mtWobbleFactor = Math.max(1, p.mtWobbleFactor);
  p.mtStepLen = Math.max(0.02, p.mtStepLen);
  p.mtSmoothLen = Math.max(0, p.mtSmoothLen);
  p.mtMinTurnRadius = Math.max(0, p.mtMinTurnRadius);
  p.mtMaxZSlope = Math.max(1, p.mtMaxZSlope);
  return p;
}

// ---- run ----
const f64 = new Float64Array(1), u64 = new BigUint64Array(f64.buffer);
const hex = x => { f64[0] = x; return u64[0].toString(16).padStart(16, '0'); };
const BOOL_KEYS = new Set(['enablePacking', 'allowPackRotation']);

const out = [];
const paramSets = {};
for (const line of fs.readFileSync(casesPath, 'utf8').split('\n')) {
  const t = line.trim().split(/\s+/);
  const kind = t[0];
  if (kind === 'params') {
    const p = { ...defaults };
    for (const kv of t.slice(2)) {
      const [k, v] = kv.split('=');
      p[k] = BOOL_KEYS.has(k) ? +v !== 0 : +v;
    }
    paramSets[t[1]] = normalise(p);
  } else if (kind === 'rng') {
    const [s, cx, cy, k] = t.slice(1).map(Number);
    const r = g.pcg4d(s | 0, cx | 0, cy | 0, k | 0);
    out.push(`rng ${t[1]} ${t[2]} ${t[3]} ${t[4]} ${r.join(' ')} ${g.hashUnit(s, cx, cy, k)}`);
  } else if (kind === 'stream') {
    const [s, cx, cy, base, count] = t.slice(1).map(Number);
    const next = g.hashStream(s, cx, cy, base);
    const vals = [];
    for (let i = 0; i < count; i++) vals.push(next());
    out.push(`stream ${t[1]} ${t[2]} ${t[3]} ${t[4]} ${vals.join(' ')}`);
  } else if (kind === 'math') {
    const n = +t[1];
    const SCALE = [8 * Math.PI, 200, 1e5, 1e9];
    for (let i = 0; i < n; i++) {
      const u1 = g.hashUnit(7, i, 0, 0), u2 = g.hashUnit(7, i, 1, 0), u3 = g.hashUnit(7, i, 2, 0);
      const s = SCALE[i % 4];
      const x = (u1 - 0.5) * 2 * s;
      const ay = (u2 - 0.5) * s, ax = (u3 - 0.5) * s;
      out.push(`m ${i} ${hex(Math.sin(x))} ${hex(Math.cos(x))} ${hex(Math.atan2(ay, ax))} ${hex(Math.hypot(ay, ax))} ` +
        `${hex(Math.atan(x))} ${hex(Math.exp((u1 - 0.5) * 1500))} ${hex(Math.log(u2 * s))} ` +
        `${hex(Math.asin(u1 * 2 - 1))} ${hex(Math.cbrt(x))} ${hex(Math.hypot(ay, ax, x))}`);
    }
  } else if (kind === 'cells') {
    const [id, seedS, pname, ...rest] = t.slice(1);
    const [cx0, cy0, cx1, cy1, maxCells] = rest.map(Number);
    const p = paramSets[pname], seed = +seedS;
    const f = x => String(x);
    let done = 0;
    for (let cx = cx0; cx <= cx1 && done < maxCells; cx++) {
      for (let cy = cy0; cy <= cy1 && done < maxCells; cy++) {
        const c = g.rawCandidate(seed, cx, cy, p);
        if (!c.present) continue;
        done++;
        g.cytoCache.clear();   // keyed by chunk only: stale across seeds/params
        const t0 = performance.now();
        const mts = g.buildMicrotubulesForCell(seed, cx, cy, c, p);
        const ms = performance.now() - t0;
        out.push(`cell ${id} ${cx} ${cy} nmt ${mts.length} ms ${ms.toFixed(3)}`);
        const { mesh } = g.getCytoGeometry(c, p);
        const hs = [];
        for (const row of mesh.grid) for (const v of row) hs.push(v.h);
        const line = [`mesh ${id} ${cx} ${cy} ${mesh.rings} ${mesh.n}`, g.getMtCellGeometry(c).areaUm2];
        for (let v = 0; v < hs.length; v += 97) line.push(hs[v]);
        for (let k = 0; k < 32; k++) {
          const r = g.hashUnit(99, cx, cy, 2 * k) * c.rOuter * 1.1;
          const a = g.hashUnit(99, cx, cy, 2 * k + 1) * Math.PI * 2;
          line.push(g.sampleCytoMeshHeight(c, p, r * Math.cos(a), r * Math.sin(a)));
        }
        out.push(line.map(f).join(' '));
        mts.forEach((pts, i) => {
          let sx = 0, sy = 0, sz = 0;
          for (const q of pts) { sx += q.x; sy += q.y; sz += q.z; }
          const l = [`mtp ${id} ${cx} ${cy} ${i} ${pts.length}`, sx, sy, sz];
          for (let j = 0; j < pts.length; j += 37) l.push(pts[j].x, pts[j].y, pts[j].z);
          if (pts.length) { const q = pts[pts.length - 1]; l.push(q.x, q.y, q.z); }
          out.push(l.map(f).join(' '));
          // Frames + lattice geometry via the label builder: a constant rng
          // (0.5) makes every site labelled and fixes the linker draws.
          if (i % 16 === 0 && pts.length >= 2) {
            const L = g.buildMicrotubuleLabelPoints(pts, { startUm: 0.5, lenUm: 0.1, phase: 0.3 }, () => 0.5);
            const m = [`mtl ${id} ${cx} ${cy} ${i} ${L.length}`];
            for (const q of L.slice(0, 13)) m.push(q.ax, q.ay, q.az, q.bx, q.by, q.bz, q.x, q.y, q.z);
            out.push(m.map(f).join(' '));
          }
        });
      }
    }
  } else if (kind === 'case') {
    const [id, seedS, pname, ...win] = t.slice(1);
    const [cx0, cy0, cx1, cy1] = win.map(Number);
    const p = paramSets[pname];
    const t0 = performance.now();
    const map = g.buildCandidateMap(+seedS, cx0, cy0, cx1, cy1, p);
    const raw = [];
    for (const c of map.values())
      raw.push(`raw ${c.cx} ${c.cy} ${c.x} ${c.y} ${c.semiMajor} ${c.semiMinor} ${c.rot} ${c.height} ${c.modFloor} ${c.nucZ} ${c.rOuter} ${c.priority}`);
    const ncand = map.size;
    const removed = g.packMap(map, p);
    const ms = performance.now() - t0;
    out.push(`case ${id} ncand ${ncand} removed ${removed} ms ${ms.toFixed(3)}`);
    out.push(...raw);
    for (const c of map.values()) out.push(`pk ${c.cx} ${c.cy} ${c.x} ${c.y} ${c.packRot}`);
  } else if (kind === 'npcs') {
    // The attachment points; per present cell the pore count, the surface area and caps, sums over every pore and
    // every 23rd pore; the dyes of the first 40 pores (density 0.7, fluorescent 0.9) under the pores' built-in
    // linkage and a flexible one, with every 7th dye's linkage parts.
    const [id, seedS, pname, ...rest] = t.slice(1);
    const [cx0, cy0, cx1, cy1, maxCells] = rest.map(Number);
    const p = paramSets[pname], seed = +seedS;
    const f = x => String(x);
    const sites = g.npcAttachmentPoints(p);
    out.push([`npa ${id} 0 0 0 ${sites.length}`, ...sites].map(f).join(' '));
    const LINKS = [LEGACY_LINKAGE.npcs, { binderNm: 10, binderFlexNm: 4, linkerMinNm: 1, linkerMaxNm: 6 }];
    let done = 0;
    for (let cx = cx0; cx <= cx1 && done < maxCells; cx++) {
      for (let cy = cy0; cy <= cy1 && done < maxCells; cy++) {
        const c = g.rawCandidate(seed, cx, cy, p);
        if (!c.present) continue;
        done++;
        const M = g.nucSurfaceMesh(c);
        const pores = g.buildNpcsForCell(seed, cx, cy, c, p);
        const s = new Array(9).fill(0);
        for (const q of pores) {
          s[0] += q.x; s[1] += q.y; s[2] += q.z;
          s[3] += q.n[0]; s[4] += q.n[1]; s[5] += q.n[2];
          s[6] += q.e1[0]; s[7] += q.e1[1]; s[8] += q.e1[2];
        }
        const l = [`npc ${id} ${cx} ${cy} 0 ${pores.length}`, M.area, M.fBot, M.fTop, ...s];
        for (let i = 0; i < pores.length; i += 23) {
          const q = pores[i];
          l.push(q.cand, q.piece, q.x, q.y, q.z, ...q.n, ...q.e1, ...q.e2);
        }
        out.push(l.map(f).join(' '));
        const ids = pores.slice(0, 40).map((_, i) => i);
        LINKS.forEach((lk, v) => {
          const dyes = [];
          npcDyesInBlock(seed >>> 0, cx, cy, pores, ids, sites, 0.7, 0.9, dyes, normLinkage(lk), true);
          let sx = 0, sy = 0, sz = 0;
          for (const e of dyes) { sx += e.pos.x; sy += e.pos.y; sz += e.pos.z; }
          const d = [`npd ${id} ${cx} ${cy} ${v} ${dyes.length}`, sx, sy, sz];
          for (let i = 0; i < dyes.length; i += 7) {
            const e = dyes[i], P = e.parts;
            d.push(e.pore, e.k, e.n, e.id, e.pos.x, e.pos.y, e.pos.z, P.site.x, P.site.y, P.site.z,
              P.rigid.x, P.rigid.y, P.rigid.z, P.tip.x, P.tip.y, P.tip.z);
          }
          out.push(d.map(f).join(' '));
        });
      }
    }
  } else if (kind === 'lamin') {
    // Per present cell the point count and the area, sums over every point and every 23rd point; the dyes of the first
    // 200 points (density 0.7, fluorescent 0.9) under the lamina's built-in linkage and a flexible one, with every 7th
    // dye's linkage parts.
    const [id, seedS, pname, ...rest] = t.slice(1);
    const [cx0, cy0, cx1, cy1, maxCells] = rest.map(Number);
    const p = paramSets[pname], seed = +seedS;
    const f = x => String(x);
    const LINKS = [LEGACY_LINKAGE.lamin, { binderNm: 19.3, binderFlexNm: 15.3, linkerMinNm: 0.5, linkerMaxNm: 2 }];
    let done = 0;
    for (let cx = cx0; cx <= cx1 && done < maxCells; cx++) {
      for (let cy = cy0; cy <= cy1 && done < maxCells; cy++) {
        const c = g.rawCandidate(seed, cx, cy, p);
        if (!c.present) continue;
        done++;
        const M = g.nucSurfaceMesh(c);
        const pts = g.buildLaminForCell(seed, cx, cy, c, p, g.buildNpcsForCell(seed, cx, cy, c, p));
        const s = new Array(6).fill(0);
        for (const q of pts) { s[0] += q.x; s[1] += q.y; s[2] += q.z; s[3] += q.n[0]; s[4] += q.n[1]; s[5] += q.n[2]; }
        const l = [`lam ${id} ${cx} ${cy} 0 ${pts.length}`, M.area, ...s];
        for (let i = 0; i < pts.length; i += 23) { const q = pts[i]; l.push(q.cand, q.piece, q.x, q.y, q.z, ...q.n); }
        out.push(l.map(f).join(' '));
        const ids = pts.slice(0, 200).map((_, i) => i);
        LINKS.forEach((lk, v) => {
          const dyes = [];
          laminDyesInBlock(seed >>> 0, cx, cy, pts, ids, 0.7, 0.9, dyes, normLinkage(lk), true);
          let sx = 0, sy = 0, sz = 0;
          for (const e of dyes) { sx += e.pos.x; sy += e.pos.y; sz += e.pos.z; }
          const d = [`lmd ${id} ${cx} ${cy} ${v} ${dyes.length}`, sx, sy, sz];
          for (let i = 0; i < dyes.length; i += 7) {
            const e = dyes[i], P = e.parts;
            d.push(e.point, e.k, e.n, e.id, e.pos.x, e.pos.y, e.pos.z, P.site.x, P.site.y, P.site.z,
              P.rigid.x, P.rigid.y, P.rigid.z, P.tip.x, P.tip.y, P.tip.z);
          }
          out.push(d.map(f).join(' '));
        });
      }
    }
  } else if (kind === 'origami') {
    // The molecules of the window, their outline (sum and every 8th point) and every designed strand.
    const [id, seedS, ...rest] = t.slice(1);
    const [x0, y0, x1, y1] = rest.slice(0, 4).map(Number);
    const kv = Object.fromEntries(rest.slice(4).map(e => { const [k, v] = e.split('='); return [k, +v]; }));
    const F = new OrigamiField(+seedS >>> 0, { specimen: 1, ...kv });
    const f = x => String(x);
    for (const m of F.moleculesInRect(x0, y0, x1, y1)) {
      const I = F.info(m);
      out.push([`ori ${id} ${m.tx} ${m.ty} ${m.i} ${I.fold}`, I.x, I.y, I.yaw, I.roll, I.endAngle].map(f).join(' '));
      const o = shapeOutline(F.shape(m), 16);
      const l = [`oro ${id} ${m.tx} ${m.ty} ${m.i} ${o.length / 2}`, o.reduce((a, v) => a + v, 0)];
      for (let j = 0; j < o.length; j += 16) l.push(o[j], o[j + 1]);
      out.push(l.map(f).join(' '));
      const st = moleculeSites(F.d, F.P, m, F.shape(m), 1, 1, [], true);
      const q = [`ors ${id} ${m.tx} ${m.ty} ${m.i} ${st.length}`];
      for (const x of st) q.push(x.k, x.n, x.id, x.x, x.y, x.z, ...x.rest, ...x.bent);
      out.push(q.map(f).join(' '));
    }
  } else if (kind === 'fiducials') {
    // The beads of the window (count, then each bead's address, centre and diameter) on the step surface.
    const [id, seedS, ...rest] = t.slice(1);
    const [x0, y0, x1, y1] = rest.slice(0, 4).map(Number);
    const kv = Object.fromEntries(rest.slice(4).map(e => { const [k, v] = e.split('='); return [k, +v]; }));
    const F = new FiducialField(+seedS >>> 0, kv, (x, y) => (x + y > 0 ? 1.5 : 0));
    const f = x => String(x), beads = F.inRect(x0, y0, x1, y1);
    out.push(`fic ${id} 0 0 0 ${beads.length}`);
    for (const b of beads) out.push([`fid ${id} ${b.tx} ${b.ty} ${b.i}`, b.x, b.y, b.z, b.d].map(f).join(' '));
  }
}
fs.writeFileSync(outPath, out.join('\n') + '\n');
console.log(`JS reference: ${out.length} lines -> ${outPath} (node ${process.version}, V8 ${process.versions.v8})`);
