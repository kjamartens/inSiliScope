// SPDX-License-Identifier: BSD-3-Clause
// A multi-D Record's plan (2026-11-23, Advanced and Expert): nested "for" blocks over XY positions, Z heights and any
// setting, with recordings (N frames at an exposure, plus settings of their own) inside them, run in the order built.
// Page-free (Node loads it too, tests/web/mda_unit.mjs): the plan's values, its blocks in run order, the checks, the
// P x Z x C x T layout of what was recorded, the saved files' pages and the index rows, the OME-XML.
//
// plan = { positions: [{ name, x, y, z }], root: [node, ...] }   (µm; a position's z may be null: the focus at Start)
// node = { kind: 'loop', axis: 'position' | 'z' | 'setting', key (setting), values: '1, 2, 3' | '0:0.5:2' | [..],
//          relative (z: offsets from the position's z), children: [node, ...] }
//      | { kind: 'record', name, frames, exposureMs, settings: { key: value } }
// A setting's name: the Micro-Manager name its help shows (Lasers.PatternPhaseMatrix) or the engine's key; keys below.
'use strict';
(function () {
// Keys a plan may not set: the frame size (one stack), the loops' and the recordings' own, and those that make a new
// sample (live.js worldKey: the continuing sample would start afresh).
const OWN = new Set(['x', 'y', 'z', 'frames', 'exposure-ms']);
const REFUSED = new Map([['size', 'changes the image size (one stack has one size)'], ['height', 'changes the image size (one stack has one size)'],
  ['start-sec', 'the sample continues (its own clock)'], ['fresh-basis', 'set by the Record'], ['frame0', 'set by the Record'],
  ['seed', 'a new sample (the Record continues the live one)'], ['world-seed', 'a new sample (the Record continues the live one)'],
  ['specimen', 'a new sample (the Record continues the live one)']]);
const MAX_VALUES = 10000, MAX_BLOCKS = 100000;
function refusedWhy(k) {
  if (OWN.has(k)) return k === 'exposure-ms' || k === 'frames' ? 'set by the recording' : 'set by a Position or Z loop';
  if (REFUSED.has(k)) return REFUSED.get(k);
  if (/^p\./.test(k)) return 'a new sample (the Record continues the live one)';
  return '';
}
// A value's text: "..." holds anything (';' and ',' too; \" and \\ inside); text -> pieces split at seps outside quotes,
// each { text, sep (the separator before it) }.
function splitTop(text, seps = /[,;]/) {
  const out = [];
  let cur = '', sep = '', q = false;
  for (let i = 0; i < text.length; i++) {
    const c = text[i];
    if (q && c === '\\' && i + 1 < text.length) { cur += c + text[++i]; continue; }
    if (c === '"') q = !q;
    if (!q && seps.test(c)) { out.push({ text: cur, sep }); cur = ''; sep = c; continue; }
    cur += c;
  }
  out.push({ text: cur, sep });
  return out;
}
const quoted = t => /^"[\s\S]*"$/.test(t) && t.length >= 2;
const unquote = t => t.slice(1, -1).replace(/\\([\s\S])/g, '$1');
// a value as text a plan's field takes back: quoted when it holds a separator, a quote or edge spaces
const quote = v => {
  const s = fmt(v);
  return typeof v === 'string' && (/[;,"\n]/.test(s) || s !== s.trim()) ? '"' + s.replace(/[\\"]/g, c => '\\' + c).replace(/\n/g, '; ') + '"' : s;
};
// '1, 2, 3' or '0:0.5:2' (start:step:stop, numbers; both ends in) or a list -> values (numbers where they read as
// numbers, else the text: names); "..." a text as it is. whole: the setting's value is one piece (a matrix, the BFP
// maps: keys.whole): without quotes the whole text is one value; several: each in quotes. Throws on a bad range.
function parseValues(v, whole = false) {
  if (Array.isArray(v)) return v.slice();
  const text = String(v == null ? '' : v);
  if (whole && !/"/.test(text)) return text.trim() ? [text.trim()] : [];
  const out = [];
  for (const piece of splitTop(text)) {
    const t = piece.text.trim();
    if (!t) continue;
    if (quoted(t)) { out.push(unquote(t)); continue; }
    const r = /^(-?[\d.eE+-]+)\s*:\s*(-?[\d.eE+-]+)\s*:\s*(-?[\d.eE+-]+)$/.exec(t);
    if (r) {
      const a = +r[1], s = +r[2], b = +r[3];
      if (![a, s, b].every(Number.isFinite) || s === 0 || (b - a) / s < 0) throw new Error(`the range ${t}: start:step:stop, the step towards stop`);
      const n = Math.floor((b - a) / s + 1e-9) + 1;
      if (n > MAX_VALUES) throw new Error(`the range ${t}: more than ${MAX_VALUES} values`);
      for (let i = 0; i < n; i++) out.push(+(a + i * s).toPrecision(12));
      continue;
    }
    out.push(Number.isFinite(+t) ? +t : t);
  }
  return out;
}
// A recording's settings text, "name=value; name=value" (or one per line) -> { name: value }. A whole setting's value
// (a matrix) may hold ';', ',' and line breaks without quotes: it runs on to the next piece that starts with a known
// name and '=' (isKey). A piece that is no name=value is kept as a name with no value (the checks then say so).
function parseSettings(text, isKey = () => true, whole = () => false) {
  const o = {};
  let cur = null;
  for (const piece of splitTop(String(text || ''), /[,;\n]/)) {
    const m = /^\s*([^=\s"]+)\s*=\s*([\s\S]*)$/.exec(piece.text);
    if (m && (!cur || isKey(m[1]) || !whole(cur))) { cur = m[1]; o[cur] = m[2]; continue; }
    if (cur && whole(cur)) { o[cur] += piece.sep + piece.text; continue; }
    const t = piece.text.trim();
    if (t) { cur = t; o[t] = ''; }
  }
  for (const k of Object.keys(o)) {
    const t = String(o[k]).trim();
    o[k] = quoted(t) ? unquote(t) : !whole(k) && t !== '' && Number.isFinite(+t) ? +t : t;
  }
  return o;
}
const formatSettings = o => Object.entries(o || {}).map(([k, v]) => `${k}=${quote(v)}`).join('; ');
const fmt = v => typeof v === 'number' ? String(+v.toPrecision(12)) : String(v);
// A setting's value for the spec: a number, or a name made the option's number (eng: the engine's option table,
// settings_core fromName); NaN when it is neither.
function specValue(eng, k, v) {
  if (typeof v === 'number') return v;
  const C = self.iscSettingsCore;
  if (eng && C) return C.fromName(eng, k, v);
  return Number.isFinite(+v) ? +v : NaN;
}
// How a plan's setting names become the spec's overrides: keys = { resolve(name, value) -> { key: value, ... } (null:
// no such setting; throws on a bad value), whole(name) }. The viewer's are web/live/mm_cfg.js planKeys: the Micro-Manager
// names the settings' help shows (Lasers.PatternPhaseMatrix, ...) and the engine's keys. These: the engine's keys alone
// (known(k): a key the viewer can set).
function engineKeys(eng, known) {
  return {
    resolve(k, v) {
      if (known && !known(k)) return null;
      const o = eng && eng.get(k);
      if (o && o.text) return { [k]: String(v) };
      const x = specValue(eng, k, v);
      if (!Number.isFinite(x)) throw new Error(`${k} = ${v}?` + (o && o.names ? ` (one of ${o.names.join(', ')})` : ' (a number)'));
      return { [k]: x };
    },
    whole: k => !!(eng && eng.get(k) && eng.get(k).text),
  };
}
// The blocks in run order. base = { x, y, z }: the stage and focus at Start (where no Position loop says); eng: the
// engine's option table (names -> numbers); keys: the names (above). A block: { seq, path, pos (index into
// plan.positions or -1), pName, x, y, z, zKey, zLabel, cKey, cName, rec, frames, exposureMs, ov (the spec's overrides:
// each setting's keys, a later (inner) one over an earlier), sets ([[name, value]]: shown) }.
function expand(plan, base, eng, keys) {
  keys = keys || engineKeys(eng);
  const blocks = [], positions = (plan && plan.positions) || [];
  const byName = new Map(positions.map((p, i) => [p.name, i]));
  const walk = (nodes, ctx, path) => {
    (nodes || []).forEach((node, i) => {
      const at = path ? path + '.' + (i + 1) : String(i + 1);
      if (!node) return;
      if (node.kind === 'record') {
        const pi = ctx.pos, P = pi >= 0 ? positions[pi] : null;
        const px = P ? +P.x : base.x, py = P ? +P.y : base.y;
        const pz = P && P.z !== null && P.z !== undefined && P.z !== '' && Number.isFinite(+P.z) ? +P.z : base.z;
        const z = ctx.z ? (ctx.z.relative ? pz + ctx.z.v : ctx.z.v) : pz;
        const own = Object.entries(node.settings || {});
        const sets = [...ctx.sets, ...own];
        const ov = { x: px, y: py, z: +z.toPrecision(12), 'exposure-ms': +node.exposureMs };
        for (const [k, v] of sets) Object.assign(ov, keys.resolve(k, v) || {});
        const loopSets = ctx.sets.map(([k, v]) => `${k}=${quote(v)}`);
        blocks.push({ seq: blocks.length, path: at, pos: pi, pName: P ? P.name : '', x: px, y: py, z: ov.z,
          zKey: ctx.z ? ctx.z.key : '', zLabel: ctx.z ? (ctx.z.relative ? (ctx.z.v >= 0 ? '+' : '') + fmt(ctx.z.v) : fmt(ctx.z.v)) : '',
          cKey: at + '|' + loopSets.join(' '), cName: [node.name || 'Record ' + at, ...loopSets].join(' · '),
          rec: node.name || 'Record ' + at, frames: Math.max(0, Math.round(+node.frames || 0)), exposureMs: +node.exposureMs, ov, sets });
        if (blocks.length > MAX_BLOCKS) throw new Error(`more than ${MAX_BLOCKS} blocks`);
        return;
      }
      if (node.kind !== 'loop') return;
      for (const v of parseValues(node.values, node.axis === 'setting' && keys.whole(node.key))) {
        if (node.axis === 'position') {
          const pi = typeof v === 'number' && !byName.has(v) ? v - 1 : byName.get(String(v));
          walk(node.children, { ...ctx, pos: pi === undefined ? -2 : pi }, at);
        } else if (node.axis === 'z') {
          walk(node.children, { ...ctx, z: { v: +v, relative: !!node.relative, key: (node.relative ? 'r' : 'a') + fmt(+v) } }, at);
        } else {
          walk(node.children, { ...ctx, sets: [...ctx.sets.filter(([k]) => k !== node.key), [node.key, v]] }, at);
        }
      }
    });
  };
  walk(plan && plan.root, { pos: -1, z: null, sets: [] }, '');
  return blocks;
}
// What is wrong with a plan (empty: nothing). opt = { eng (the option table), keys (the names, above) or known(k) (a key
// the viewer can set), maxFrames, blockCheck(block) -> [message] (a check of the blocks as they will run) }.
function validate(plan, base, opt = {}) {
  const errs = [], eng = opt.eng, positions = (plan && plan.positions) || [];
  const keys = opt.keys || engineKeys(eng, opt.known);
  const names = new Set();
  positions.forEach((p, i) => {
    const what = `Position ${i + 1}`;
    if (!p || !String(p.name || '').trim()) errs.push(`${what}: no name`);
    else if (names.has(p.name)) errs.push(`${what}: the name ${p.name} is used twice`);
    else names.add(p.name);
    if (p && (!Number.isFinite(+p.x) || !Number.isFinite(+p.y) || p.x === '' || p.y === '')) errs.push(`${what}: x and y in µm`);
  });
  let records = 0;
  const key = (k, where) => {
    if (!k) { errs.push(`${where}: no setting chosen`); return false; }
    const why = refusedWhy(k);
    if (why) { errs.push(`${where}: ${k} cannot be set here: ${why}`); return false; }
    return true;
  };
  // false: the setting is unknown (said once)
  const value = (k, v, where) => {
    if (typeof v === 'string' && v.trim() === '') { errs.push(`${where}: ${k} has no value`); return true; }
    let r;
    try { r = keys.resolve(k, v); } catch (e) { errs.push(`${where}: ${e.message}`); return true; }
    if (!r) { errs.push(`${where}: no setting ${k}`); return false; }
    for (const rk of Object.keys(r)) {
      const why = refusedWhy(rk);
      if (why) { errs.push(`${where}: ${k} cannot be set here (${rk}): ${why}`); return false; }
    }
    return true;
  };
  const walk = (nodes, path) => (nodes || []).forEach((node, i) => {
    const at = path ? path + '.' + (i + 1) : String(i + 1);
    if (!node) return;
    if (node.kind === 'record') {
      records++;
      const where = `Block ${at} (record ${node.name || ''})`.replace(' ()', '');
      if (!(Number.isInteger(+node.frames) && +node.frames >= 1)) errs.push(`${where}: frames, a whole number ≥ 1`);
      if (!(+node.exposureMs > 0)) errs.push(`${where}: an exposure in ms > 0`);
      for (const [k, v] of Object.entries(node.settings || {})) if (key(k, where)) value(k, v, where);
      return;
    }
    const where = `Block ${at} (for ${node.axis === 'setting' ? node.key || 'a setting' : node.axis})`;
    let vals = [];
    try { vals = parseValues(node.values, node.axis === 'setting' && !!node.key && keys.whole(node.key)); } catch (e) { errs.push(`${where}: ${e.message}`); }
    if (!vals.length) errs.push(`${where}: no values`);
    if (!(node.children || []).length) errs.push(`${where}: nothing inside`);
    if (node.axis === 'position') {
      for (const v of vals) if (!names.has(String(v)) && !(Number.isInteger(v) && v >= 1 && v <= positions.length)) errs.push(`${where}: no position ${v}`);
    } else if (node.axis === 'z') {
      for (const v of vals) if (typeof v !== 'number') errs.push(`${where}: ${v} is not a height in µm`);
    } else if (node.axis === 'setting') {
      if (key(node.key, where)) for (const v of vals) if (!value(node.key, v, where)) break;
    } else errs.push(`${where}: a loop over what?`);
    walk(node.children, at);
  });
  walk(plan && plan.root, '');
  if (!records) errs.push('No recording: add a Record block');
  if (!errs.length) {
    try {
      const b = expand(plan, base, eng, keys), n = b.reduce((s, x) => s + x.frames, 0);
      if (!b.length) errs.push('The loops run no recording');
      if (opt.maxFrames && n > opt.maxFrames) errs.push(`${n} frames: more than ${opt.maxFrames}`);
      if (opt.blockCheck) for (const x of b) for (const m of opt.blockCheck(x)) if (!errs.includes(m)) errs.push(m);
    } catch (e) { errs.push(e.message || String(e)); }
  }
  return errs;
}
// The layout of what was recorded: the blocks, got = the frames recorded (the first got of the run), place = per
// frame { t } (the sample's time at it, s). P, Z, C: the distinct positions / Z-loop values / channels in order of
// first use; frames[f] = { f, b (block), k (frame in the block), p, z, c, t }; T = the longest cell; cells.get(key) =
// its frames' indices.
function layout(blocks, got, positions) {
  const P = [], Z = [], C = [], pIdx = new Map(), zIdx = new Map(), cIdx = new Map(), cells = new Map(), frames = [];
  let f = 0;
  for (const b of blocks) {
    if (f >= got) break;
    const pk = b.pos >= 0 ? 'p' + b.pos : 'stage';
    if (!pIdx.has(pk)) { pIdx.set(pk, P.length); P.push(b.pos >= 0 ? { ...positions[b.pos] } : { name: 'Stage', x: b.x, y: b.y, z: null }); }
    if (!zIdx.has(b.zKey)) { zIdx.set(b.zKey, Z.length); Z.push({ key: b.zKey, label: b.zLabel || 'focus' }); }
    if (!cIdx.has(b.cKey)) { cIdx.set(b.cKey, C.length); C.push({ key: b.cKey, name: b.cName, exposureMs: b.exposureMs }); }
    const p = pIdx.get(pk), z = zIdx.get(b.zKey), c = cIdx.get(b.cKey), key = p + '|' + z + '|' + c;
    if (!cells.has(key)) cells.set(key, []);
    const cell = cells.get(key);
    for (let k = 0; k < b.frames && f < got; k++, f++) { frames.push({ f, b: b.seq, k, p, z, c, t: cell.length }); cell.push(f); }
  }
  let T = 0;
  for (const v of cells.values()) T = Math.max(T, v.length);
  return { P, Z, C, T, frames, cells, got: f };
}
const safeName = (s, used) => {
  let n = String(s || 'P').replace(/[^A-Za-z0-9._-]+/g, '_').replace(/^_+|_+$/g, '') || 'P', m = n, i = 2;
  while (used.has(m.toLowerCase())) m = `${n}_${i++}`;
  used.add(m.toLowerCase());
  return m;
};
// The saved files' pages: format 'ome' (one file: the frames in run order, then one page per blank plane), 'imagej'
// (a file per position, ImageJ's order: c fastest, then z, then t), 'flat' (one file, run order). A page: { f } (a
// recorded frame) or { pad: true, p, z, c, t }. Each file: { name, p (imagej), pages }.
function files(L, format) {
  const { P, Z, C, T, frames, cells } = L;
  const at = (p, z, c, t) => { const v = cells.get(p + '|' + z + '|' + c); return v && t < v.length ? v[t] : -1; };
  if (format === 'flat') return [{ name: 'movie.tif', pages: frames.map(fr => ({ f: fr.f })) }];
  if (format === 'imagej') {
    const used = new Set(['index']);
    return P.map((pp, p) => {
      const pages = [];
      for (let t = 0; t < T; t++) for (let z = 0; z < Z.length; z++) for (let c = 0; c < C.length; c++) {
        const f = at(p, z, c, t);
        pages.push(f >= 0 ? { f } : { pad: true, p, z, c, t });
      }
      return { name: safeName(pp.name, used) + '.tif', p, pages };
    });
  }
  const pages = frames.map(fr => ({ f: fr.f }));
  for (let p = 0; p < P.length; p++) for (let c = 0; c < C.length; c++) for (let z = 0; z < Z.length; z++)
    for (let t = 0; t < T; t++) if (at(p, z, c, t) < 0) pages.push({ pad: true, p, z, c, t });
  return [{ name: 'movie.ome.tif', pages }];
}
const csvCell = v => { const s = v === null || v === undefined ? '' : String(v); return /[",\n]/.test(s) ? '"' + s.replace(/"/g, '""') + '"' : s; };
const num = v => v === null || v === undefined || v === '' || !Number.isFinite(+v) ? '' : String(+(+v).toPrecision(10));
// index.csv: a row per page of every file. place[f] = { t } (the sample's time at the frame's start, s).
function indexCsv(L, blocks, fileList, place) {
  const head = ['file', 'page', 'position', 'position_name', 'x_um', 'y_um', 'z_um', 'z_index', 'z_label', 'channel', 'channel_name',
    't', 'block', 'block_path', 'frame_in_block', 'acq_frame', 'exposure_ms', 'sample_time_s', 'padding', 'settings'];
  const rows = [head.join(',')];
  for (const file of fileList) file.pages.forEach((pg, i) => {
    let r;
    if (pg.f !== undefined) {
      const fr = L.frames[pg.f], b = blocks[fr.b];
      r = [file.name, i, fr.p, L.P[fr.p].name, num(b.x), num(b.y), num(b.z), fr.z, L.Z[fr.z].label, fr.c, L.C[fr.c].name, fr.t, b.seq,
        b.path, fr.k, fr.f, num(b.exposureMs), place && place[fr.f] ? num(place[fr.f].t) : '', 0,
        formatSettings(Object.fromEntries(b.sets))];
    } else {
      r = [file.name, i, pg.p, L.P[pg.p].name, '', '', '', pg.z, L.Z[pg.z].label, pg.c, L.C[pg.c].name, pg.t, '', '', '', '', '', '', 1, ''];
    }
    rows.push(r.map(csvCell).join(','));
  });
  return rows.join('\n') + '\n';
}
const xml = s => String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;');
// The OME-XML of an 'ome' file (files(L, 'ome')[0]): an Image per position (XYTZC: a block's frames run along T), its
// channels, TiffData per run of pages, a Plane per recorded frame (time, exposure, stage position).
function omeXml(L, blocks, file, o) {
  const { P, Z, C, T } = L;
  const out = ['<?xml version="1.0" encoding="UTF-8"?>',
    '<OME xmlns="http://www.openmicroscopy.org/Schemas/OME/2016-06" xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance" ' +
    'xsi:schemaLocation="http://www.openmicroscopy.org/Schemas/OME/2016-06 http://www.openmicroscopy.org/Schemas/OME/2016-06/ome.xsd" ' +
    `Creator="inSiliScope viewer ${xml(o.version || '')}">`];
  // each page's (p, c, z, t), then runs of consecutive pages along T in one (p, c, z)
  const coord = file.pages.map(pg => pg.f !== undefined ? L.frames[pg.f] : pg);
  for (let p = 0; p < P.length; p++) {
    out.push(`<Image ID="Image:${p}" Name="${xml(P[p].name)}">`);
    if (o.date) out.push(`<AcquisitionDate>${xml(o.date)}</AcquisitionDate>`);
    out.push(`<Pixels ID="Pixels:${p}" DimensionOrder="XYTZC" Type="uint16" SignificantBits="16" Interleaved="false" BigEndian="false" ` +
      `SizeX="${o.w}" SizeY="${o.h}" SizeZ="${Z.length}" SizeC="${C.length}" SizeT="${T}" ` +
      `PhysicalSizeX="${num(o.pxUm)}" PhysicalSizeXUnit="µm" PhysicalSizeY="${num(o.pxUm)}" PhysicalSizeYUnit="µm">`);
    C.forEach((c, i) => out.push(`<Channel ID="Channel:${p}:${i}" Name="${xml(c.name)}" SamplesPerPixel="1"/>`));
    for (let i = 0; i < coord.length;) {
      const a = coord[i];
      if (a.p !== p) { i++; continue; }
      let j = i + 1;
      while (j < coord.length && coord[j].p === p && coord[j].c === a.c && coord[j].z === a.z && coord[j].t === a.t + (j - i)) j++;
      out.push(`<TiffData IFD="${i}" FirstC="${a.c}" FirstZ="${a.z}" FirstT="${a.t}" PlaneCount="${j - i}"/>`);
      i = j;
    }
    for (const fr of L.frames) {
      if (fr.p !== p) continue;
      const b = blocks[fr.b], tt = o.place && o.place[fr.f] ? o.place[fr.f].t - o.t0 : null;
      out.push(`<Plane TheC="${fr.c}" TheZ="${fr.z}" TheT="${fr.t}"` + (tt !== null ? ` DeltaT="${num(tt)}" DeltaTUnit="s"` : '') +
        ` ExposureTime="${num(b.exposureMs)}" ExposureTimeUnit="ms" PositionX="${num(b.x)}" PositionXUnit="µm" ` +
        `PositionY="${num(b.y)}" PositionYUnit="µm" PositionZ="${num(b.z)}" PositionZUnit="µm"/>`);
    }
    out.push('</Pixels>', '</Image>');
  }
  out.push('</OME>');
  return out.join('\n');
}
// An ImageJ hyperstack's description (files(L, 'imagej')): C x Z x T pages, c fastest.
function imagejDesc(L, extra) {
  const n = L.C.length * L.Z.length * L.T;
  return [`ImageJ=1.11a`, `images=${n}`, `channels=${L.C.length}`, `slices=${L.Z.length}`, `frames=${L.T}`, 'hyperstack=true',
    `mode=${L.C.length > 1 ? 'composite' : 'grayscale'}`, 'unit=micron', 'loop=false', ...(extra || [])].join('\n') + '\n';
}
// A file's TIFF data. paged (the OME file: writeTiffPaged): the recorded frames as they are (run order, got of them),
// then one blank frame every blank page points at: { dataBytes, blankAt, pageAt(p) }, bytes. Otherwise (ImageJ, flat:
// writeTiff16, whose pages are back to back) the parts in page order: frameAt(f) a recorded frame's part, blank one
// blank frame's.
function pagedLayout(file, got, px) {
  const blankAt = got * px * 2;
  return { dataBytes: blankAt + px * 2, blankAt, pageAt: p => file.pages[p].f !== undefined ? file.pages[p].f * px * 2 : blankAt };
}
const pageParts = (file, frameAt, blank) => file.pages.map(pg => pg.f !== undefined ? frameAt(pg.f) : blank);
// For the editor: blocks, frames, the first few blocks in words.
function summary(blocks, n = 12) {
  const frames = blocks.reduce((s, b) => s + b.frames, 0);
  const line = b => `${b.seq + 1}. ` + [b.pName || 'stage', `z ${num(b.z)} µm`, b.cName].join(' · ') + ` — ${b.frames} × ${num(b.exposureMs)} ms`;
  return { blocks: blocks.length, frames, lines: blocks.slice(0, n).map(line), more: Math.max(0, blocks.length - n) };
}
// The block a frame index of the run falls in (blocks in order): { b, k }.
function blockAt(blocks, f) {
  let s = 0;
  for (const b of blocks) { if (f < s + b.frames) return { b: b.seq, k: f - s }; s += b.frames; }
  return null;
}
const example = () => ({ positions: [], root: [
  { kind: 'loop', axis: 'z', values: '-0.5:0.5:0.5', relative: true, children: [
    { kind: 'record', name: 'A', frames: 10, exposureMs: 50, settings: {} }] }] });
self.ISC_MDA = { parseValues, parseSettings, formatSettings, quote, engineKeys, expand, validate, layout, files, indexCsv, omeXml, imagejDesc, pagedLayout, pageParts, summary, blockAt, refusedWhy, example, fmt };
})();
