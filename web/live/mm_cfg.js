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
const isSpecial = (cfg, k) => k in cfg.special || /^zern\.\d+$/.test(k) || /^beam\.\d+\.[A-Za-z]+$/.test(k);

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

self.iscMmCfg = { write, uncovered, bfpOneLine, BEAM_COLUMNS };
})();
