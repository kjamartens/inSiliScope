// SPDX-License-Identifier: BSD-3-Clause
// A Micro-Manager configuration (.cfg) from the viewer's settings: loaded in Micro-Manager, the adapter renders with the
// same engine spec as the viewer (Renderer.WriteScopeSpecTo = movieSpec(), but for the viewer-only options). Page-free
// (Node loads it too: tests/web/mm_cfg_unit.mjs); the table is self.ISC_MM_CFG (web/live/mm_config.js, written by
// tools/gen_mm_configs.py with tools/mm_cfg_map.py from the adapter itself): each engine option's property and how its
// values map, in the order a configuration sets them (a target's mode, its label, combinations, then their members).
// The file: the shipped Expert configuration (devices, roles, groups, pixel sizes) with the viewer's seed and a Startup
// preset that sets every property the viewer has a value for.
'use strict';
(function () {
const BEAM_COLUMNS = ['ux', 'uy', 'spot', 'tiltX', 'tiltY', 'amp', 'phase', 'pol', 'ellip', 'phaseStep', 'nm'];
const num = v => String(+v);   // (JavaScript's shortest form: the table's choice keys are written the same way)

// The BFP maps' text as one line of a Micro-Manager property (= the adapter's BfpOneLine, with the arguments' commas as
// ';': MMCore refuses a comma in a value, and a .cfg line is split at commas): statements joined by '; ', comments
// dropped.
function bfpOneLine(text) {
  let out = '', cur = '', quoted = false, comment = false;
  const flush = () => { const t = cur.replace(/^[ \t\r]+|[ \t\r]+$/g, ''); if (t) out += (out ? '; ' : '') + t; cur = ''; };
  for (const c of String(text)) {
    if (c === '\n') { flush(); comment = quoted = false; continue; }
    if (comment) continue;
    if ((c === ';' || c === ',') && !quoted) { flush(); continue; }
    if (c === '"') quoted = !quoted;
    else if (c === '#' && !quoted) { comment = true; continue; }
    cur += c;
  }
  flush();
  return out;
}

// The viewer's keys of an option (its own name and the names Micro-Manager writes it under: p.chunkSize = chunk-um).
function viewerKeys(cfg, o) {
  return [o, ...Object.keys(cfg.aliases).filter(k => cfg.aliases[k] === o)];
}
const isSpecial = (cfg, k) => k in cfg.special || /^zern\.\d+$/.test(k) || /^beam\.\d+\.[A-Za-z]+$/.test(k) || /^sim-pair-phase\.\d+\.\d+$/.test(k);

// The viewer's options no property sets: neither a row's, a special one, nor viewer-only (tests/web/viewer_mm_cfg.mjs
// fails on any: a new viewer setting needs a Micro-Manager property, spec/MM_DEVICES.md, or a line in
// tools/mm_cfg_map.py VIEWER_ONLY).
function uncovered(keys, cfg) {
  const mapped = new Set();
  for (const r of cfg.rows) for (const o of Object.keys(r.opts)) mapped.add(o);
  return [...new Set(keys)].filter(k => !mapped.has(cfg.aliases[k] || k) && !isSpecial(cfg, k) && !(k in cfg.viewerOnly)).sort();
}

// spec: Map (iscSettingsCore.parseSpec(movieSpec())); optics: {objectiveMag, sensorUm, emissionMag} (the pixel size's
// parts); name: written into the header. -> {text, settings: [[device, property, value]], warnings: [...]}
// engine: Map option -> {def} (self.ISC_ENGINE_OPTIONS: an option Micro-Manager's spec leaves out is at its default).
function write(spec, { cfg = self.ISC_MM_CFG, optics = {}, name = 'inSiliScope', date = '', engine = self.ISC_ENGINE_OPTIONS } = {}) {
  const get = o => { for (const k of viewerKeys(cfg, o)) if (spec.has(k)) return spec.get(k); return undefined; };
  const settings = [], warnings = [];
  // The values Micro-Manager has as the settings go in: a fresh hub's, then what each combination written sets. A number
  // property keeps 4 decimals (MMDevice), so a value it has already (a default, a camera preset's dark current) is not
  // written again when that would round it; another value with more digits is written rounded (noted).
  const has = {};
  if (engine) for (const [k, o] of engine) if (o && Number.isFinite(+o.def)) has[k] = o.def;
  Object.assign(has, cfg.fresh || {});
  const r4 = v => Math.round(v * 1e4) / 1e4, lossy = v => Math.abs(r4(v) - v) > 1e-12 * Math.max(1, Math.abs(v));
  const sameAs = (o, v) => has[o] !== undefined && Math.abs(+has[o] - v) <= 1e-9 * Math.max(1, Math.abs(v));
  const put = (d, p, v) => {
    if (/,/.test(String(v))) { warnings.push(`${d}.${p}: a value with a comma cannot be in a configuration (left out)`); return; }
    settings.push([d, p, String(v)]);
  };
  for (const r of cfg.rows) {
    for (const [o, map] of Object.entries(r.opts)) {
      const v = get(o);
      if (v === undefined || typeof v === 'string') continue;
      if (r.kind === 'num') {
        if (lossy(v) && sameAs(o, v)) break;
        // (a dye field at its dye's value reads back as no edit: its setter takes 4 decimals as the library's)
        if (lossy(v) && !/^([a-z]+-dye|dye\d)\./.test(o)) warnings.push(`${r.dev}.${r.prop}: ${v} kept to 4 decimals (${r4(v)}; Micro-Manager's number properties)`);
        put(r.dev, r.prop, num(v));
        break;
      }
      const c = map[num(v)];
      if (c === undefined && v === -1) break;   // (-1: the option follows another, a camera's type its preset)
      if (c === undefined) { warnings.push(`${r.dev}.${r.prop}: ${o} = ${v} is none of its choices (left out)`); continue; }
      put(r.dev, r.prop, c);
      if (r.sets && r.sets[c]) Object.assign(has, r.sets[c]);
      break;
    }
  }
  // the pixel size: the camera's relay (EmissionPath) and the objective's magnification (a camera preset sets the
  // sensor pixel; a custom camera's comes from the optics)
  if (optics.emissionMag > 0) put('EmissionPath', 'EmissionMagnification', num(optics.emissionMag));
  if (!spec.has('magnification') && optics.objectiveMag > 0) put('Objective', 'Magnification', num(optics.objectiveMag));
  const custom = cfg.rows.find(r => r.prop === 'CameraPreset');
  if (custom && custom.opts['camera-preset'] && custom.opts['camera-preset'][num(get('camera-preset'))] === 'Custom' && optics.sensorUm > 0)
    put('Camera', 'SensorPixelUm', num(optics.sensorUm));
  // the objective's aberrations: the preset, then the typed modes over it (all 28, space-separated)
  const zp = get('psf-zernike-preset');
  if (zp !== undefined && cfg.zernike[zp] !== undefined) put('Objective', 'ZernikePreset', cfg.zernike[zp]);
  if ([...spec.keys()].some(k => /^zern\.\d+$/.test(k)))
    put('Objective', 'ZernikeCoefficients', Array.from({ length: 28 }, (_, j) => num(spec.get('zern.' + j) || 0)).join(' '));
  // the beam matrix: rows by ';', columns by spaces
  const beams = [];
  for (const [k, v] of spec) { const m = /^beam\.(\d+)\.([A-Za-z]+)$/.exec(k); if (m) (beams[+m[1]] = beams[+m[1]] || {})[m[2]] = v; }
  if (beams.length) put('Lasers', 'Beams', beams.filter(Boolean).map(b => BEAM_COLUMNS.map(c => num(b[c] || 0)).join(' ')).join('; '));
  // the phase matrix: a row per frame of the cycle by ';', the beam pairs' phase differences by spaces; after the
  // Pattern, PatternPhases / PatternAngles (each clears it) and the Beams (its pairs). Only whole (the viewer writes
  // every entry; Micro-Manager refuses another shape).
  const phases = [];
  for (const [k, v] of spec) { const m = /^sim-pair-phase\.(\d+)\.(\d+)$/.exec(k); if (m) (phases[+m[1]] = phases[+m[1]] || [])[+m[2]] = v; }
  if (phases.length) {
    const nP = Math.max(...phases.filter(Boolean).map(r => r.length));
    const whole = Array.from(phases).every(r => r && r.length === nP && Array.from(r).every(v => v !== undefined));
    if (whole) put('Lasers', 'PatternPhaseMatrix', phases.map(r => r.map(num).join(' ')).join('; '));
    else warnings.push('Lasers.PatternPhaseMatrix: the phase matrix is not whole (left out)');
  }
  if (typeof spec.get('bfp-maps') === 'string' && spec.get('bfp-maps').trim()) {
    const t = bfpOneLine(spec.get('bfp-maps'));
    put('Lasers', 'BfpMaps', t);
    if (/\bimg\s*\(/.test(t)) warnings.push('The BFP maps use map images (img("...")): put those files where Micro-Manager finds them (a TIFF by its path, relative to Micro-Manager\'s folder, or an absolute path).');
  }
  // the acquisition: exposure and camera size
  if (spec.has('exposure-ms')) put('Camera', 'Exposure', num(spec.get('exposure-ms')));
  if (spec.has('size')) { put('Camera', 'SensorWidthPx', num(spec.get('size'))); put('Camera', 'SensorHeightPx', num(spec.get('height') || spec.get('size'))); }
  // the light: the Core-Shutter the autoshutter opens (the lamp for BrightField, both through the Multi Shutter)
  const lamp = get('modality') === 1, both = !lamp && get('light-trans') === 1;
  const shutter = lamp ? 'TransmittedLamp' : both ? 'Multi Shutter' : 'Lasers';
  const seed = spec.has('seed') ? Math.round(spec.get('seed')) : null;

  // the shipped Expert configuration with these settings as its Startup
  const L = cfg.template.split(/\r?\n/), out = [];
  const head = [`# inSiliScope: "${name}", written by the viewer's Micro-Manager .cfg export${date ? ' on ' + date : ''} from the shipped`,
    '# Expert configuration (tools/gen_mm_configs.py). The Startup preset sets every property to the viewer\'s value; the',
    '# viewer-only settings are not in it (the stage position, the movie\'s length and start, the viewer\'s 3 um PSF kernel).'];
  for (const w of warnings) head.push('# Note: ' + w);
  let inStartup = false;
  for (const line of L) {
    if (/^# inSiliScope, Detail = /.test(line)) { out.push(...head); continue; }
    if (/^Property,Hub,RandomSeed,/.test(line) && seed !== null) { out.push('Property,Hub,RandomSeed,' + seed); continue; }
    const m = /^ConfigGroup,System,Startup,([^,]+),([^,]+),(.*)$/.exec(line);
    if (m) {
      // the Core's roles and the Multi Shutter's sources stay; the device settings are the viewer's
      if (m[1] === 'Core' && m[2] === 'Shutter') continue;
      if (m[1] === 'Core' || m[1] === 'Multi Shutter') out.push(line);
      if (!inStartup) inStartup = true;
      continue;
    }
    if (inStartup) {   // the preset's end: the viewer's settings, the shutters closed, the light's shutter
      for (const [d, p, v] of settings) out.push(`ConfigGroup,System,Startup,${d},${p},${v}`);
      out.push('ConfigGroup,System,Startup,Lasers,State,0', 'ConfigGroup,System,Startup,TransmittedLamp,State,0',
               'ConfigGroup,System,Startup,Core,Shutter,' + shutter);
      inStartup = false;
    }
    out.push(line);
  }
  return { text: out.join('\n'), settings, warnings, shutter };
}

// The other way (2026-11-23, the multi-D plan): a setting by the Micro-Manager name its help shows (`Lasers.Pattern`,
// `CellField.NuclearPores_Mode`, `Lasers.PatternPhaseMatrix`) with a value as that property takes it -> the spec's
// overrides. A choice gives its option's value and what the choice sets (the row's `sets`, as Micro-Manager applies it);
// the specials take their whole value: the Zernike modes (up to 28 numbers), the Beams and the phase matrix (rows by ';'
// or a new line, numbers by spaces or ','), the BFP maps (the text), the Zernike preset (its name). An engine key (no
// property) is still taken as itself. -> { nameOf, resolve(name, v) (null: no such setting; throws on a bad value),
// whole(name) (a value that may hold ';' and ','), names() }.
const SPECIAL_NAMES = { 'Camera.Exposure': 'exposure-ms', 'Camera.SensorWidthPx': 'size', 'Camera.SensorHeightPx': 'height' };
const MATRIX = ['Objective.ZernikeCoefficients', 'Lasers.Beams', 'Lasers.PatternPhaseMatrix', 'Lasers.BfpMaps'];
function matrixRows(v, what) {
  const rows = String(v).split(/[;\n]/).map(r => r.trim()).filter(Boolean).map(r => r.split(/[\s,]+/).map(Number));
  if (!rows.length || !rows.every(r => r.every(Number.isFinite))) throw new Error(`${what}: numbers, rows separated by ; (${v}?)`);
  return rows;
}
function planKeys({ cfg = self.ISC_MM_CFG, engine = self.ISC_ENGINE_OPTIONS, known = null } = {}) {
  const byName = new Map(), byOption = new Map();
  // (the viewer's key of an option: an alias where the viewer writes it under another name, p.chunkSize = chunk-um)
  const viewerKey = o => Object.keys(cfg.aliases || {}).find(k => cfg.aliases[k] === o) || o;
  for (const r of cfg.rows) { const n = r.dev + '.' + r.prop; byName.set(n, r); for (const o of Object.keys(r.opts)) byOption.set(o, n); }
  const special = new Map();   // name -> option (one key; the wildcards are the matrices)
  for (const [o, n] of Object.entries(cfg.special || {})) if (/^[A-Z]\w*\.\w+$/.test(n) && !/\*/.test(o)) special.set(n, o);
  for (const [n, o] of Object.entries(SPECIAL_NAMES)) special.set(n, o);
  const nameOf = o => byOption.get(o) || (o === 'bfp-maps' ? 'Lasers.BfpMaps' : [...special].find(([, x]) => x === o)?.[0]) || o;
  const num = (n, v) => { const x = typeof v === 'number' ? v : String(v).trim() === '' ? NaN : +v; if (!Number.isFinite(x)) throw new Error(`${n} = ${v}? (a number)`); return x; };
  function resolve(n, v) {
    if (n === 'Objective.ZernikeCoefficients') {
      const z = matrixRows(v, n).flat();
      if (z.length > 28) throw new Error(`${n}: at most 28 modes (${z.length})`);
      return Object.fromEntries(Array.from({ length: 28 }, (_, j) => ['zern.' + j, z[j] || 0]));
    }
    if (n === 'Lasers.Beams') {
      const out = {};
      matrixRows(v, n).forEach((r, j) => {
        if (r.length > BEAM_COLUMNS.length) throw new Error(`${n}: row ${j + 1} has ${r.length} columns (at most ${BEAM_COLUMNS.length}: ${BEAM_COLUMNS.join(' ')})`);
        r.forEach((x, c) => { out['beam.' + j + '.' + BEAM_COLUMNS[c]] = x; });
      });
      return out;
    }
    if (n === 'Lasers.PatternPhaseMatrix') {
      const rows = matrixRows(v, n);
      if (rows.some(r => r.length !== rows[0].length)) throw new Error(`${n}: every row needs as many values (beam pairs)`);
      const out = {};
      rows.forEach((r, s) => r.forEach((x, p) => { out['sim-pair-phase.' + s + '.' + p] = x; }));
      return out;
    }
    if (n === 'Lasers.BfpMaps') return { 'bfp-maps': String(v) };
    if (n === 'Objective.ZernikePreset') {
      const i = typeof v === 'number' ? v : (cfg.zernike || []).indexOf(String(v).trim());
      if (!(i >= 0 && i < (cfg.zernike || []).length)) throw new Error(`${n} = ${v}? (one of ${(cfg.zernike || []).join(', ')})`);
      return { 'psf-zernike-preset': i };
    }
    const r = byName.get(n);
    if (r) {
      const [o, map] = Object.entries(r.opts)[0];
      if (r.kind === 'num') return { [viewerKey(o)]: num(n, v) };
      const t = String(v).trim();
      let val = Object.keys(map).find(x => map[x] === t);
      if (val === undefined) val = Object.keys(map).find(x => map[x].toLowerCase() === t.toLowerCase());
      if (val === undefined && map[String(v)] !== undefined) val = String(v);   // (the option's number)
      if (val === undefined) throw new Error(`${n} = ${v}? (one of ${[...new Set(Object.values(map))].join(', ')})`);
      const out = {};
      for (const [k, x] of Object.entries((r.sets && r.sets[map[val]]) || {})) out[viewerKey(k)] = +x;   // (what the choice sets)
      out[viewerKey(o)] = +val;
      return out;
    }
    if (special.has(n)) return { [special.get(n)]: num(n, v) };
    // an engine key as itself (no property): its names (iscSettingsCore.fromName), a text option's text
    if (engine && engine.has(n) || (known && known(n))) {
      const e = engine && engine.get(n);
      if (e && e.text) return { [n]: String(v) };
      const C = self.iscSettingsCore, x = typeof v === 'number' ? v : engine && C ? C.fromName(engine, n, v) : +v;
      if (!Number.isFinite(x)) throw new Error(`${n} = ${v}?` + (e && e.names ? ` (one of ${e.names.join(', ')})` : ' (a number)'));
      return { [n]: x };
    }
    return null;
  }
  const whole = n => MATRIX.includes(n) || !!(engine && engine.get(n) && engine.get(n).text);
  // A setting's value now, as resolve takes it (spec: Map, the viewer's movieSpec() parsed): a choice's name, a number,
  // the matrices' text when the spec has their entries (else undefined: the viewer reads its box), the maps' text.
  function current(n, spec) {
    const g = k => spec.get(k);
    if (n === 'Objective.ZernikeCoefficients') {
      if (![...spec.keys()].some(k => /^zern\.\d+$/.test(k))) return undefined;
      return Array.from({ length: 28 }, (_, j) => num0(g('zern.' + j))).join(' ');
    }
    const rowsOf = re => { const out = []; for (const [k, v] of spec) { const m = re.exec(k); if (m) (out[+m[1]] = out[+m[1]] || [])[m[2]] = v; } return out; };
    if (n === 'Lasers.Beams') {
      const b = rowsOf(/^beam\.(\d+)\.([A-Za-z]+)$/);
      return b.length ? b.filter(Boolean).map(r => BEAM_COLUMNS.map(c => num0(r[c])).join(' ')).join('; ') : undefined;
    }
    if (n === 'Lasers.PatternPhaseMatrix') {
      const p = rowsOf(/^sim-pair-phase\.(\d+)\.(\d+)$/);
      return p.length ? p.map(r => Array.from(r || []).map(num0).join(' ')).join('; ') : undefined;
    }
    if (n === 'Lasers.BfpMaps') return typeof g('bfp-maps') === 'string' ? g('bfp-maps') : undefined;
    if (n === 'Objective.ZernikePreset') return (cfg.zernike || [])[g('psf-zernike-preset')];
    const r = byName.get(n);
    if (r) {
      const [o, map] = Object.entries(r.opts)[0], v = g(viewerKey(o));
      if (v === undefined) return undefined;
      return r.kind === 'num' || map[String(+v)] === undefined ? v : map[String(+v)];
    }
    if (special.has(n)) return g(special.get(n));
    const v = g(n), e = engine && engine.get(n);
    return e && e.names && Number.isInteger(v) && v >= 0 && v < e.names.length ? e.names[v] : v;
  }
  const num0 = v => String(+(v || 0));
  // every name: the properties (a choice with its choices), the specials, then the engine's options no property sets
  function names() {
    const out = [];
    for (const [n, r] of byName) out.push({ name: n, option: viewerKey(Object.keys(r.opts)[0]), choices: r.kind === 'choice' ? [...new Set(Object.values(Object.values(r.opts)[0]))] : null });
    for (const n of MATRIX) out.push({ name: n, option: null, whole: true });
    out.push({ name: 'Objective.ZernikePreset', option: 'psf-zernike-preset', choices: cfg.zernike || [] });
    if (engine) for (const [k, e] of engine) if (!byOption.has(k) && nameOf(k) === k) out.push({ name: k, option: k, choices: e.names || null, whole: !!e.text });
    return out;
  }
  return { nameOf, resolve, whole, names, current };
}

self.iscMmCfg = { write, uncovered, bfpOneLine, BEAM_COLUMNS, planKeys, matrixRows };
})();
