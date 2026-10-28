// SPDX-License-Identifier: BSD-3-Clause
// The live panel's Configuration: the config groups of the shipped Micro-Manager configurations
// (adapter/inSiliScope/config/inSiliScope_<tier>.cfg, the settings level's: Basic, Advanced, Expert), from
// web/live/mm_config.js (self.ISC_MM_CONFIG, written by tools/gen_mm_configs.py with the files: per preset the engine
// options the adapter renders with, as numbers, null = the engine's default). A preset sets the settings panel's
// controls (mv_<option>); an option the panel has no control for (the objective's NA, ...) goes into every movie and
// live spec (self.ISC_MM_EXTRA, movieSpec), if the module knows it. Loaded after the main script and web/live/live.js.
'use strict';
(function () {
const $ = id => document.getElementById(id);
const CFG = self.ISC_MM_CONFIG, DD = self.ISC_DYE_DATA, host = $('cfgPanel');
if (!CFG || !host) return;
const MODES = ['dSTORM', 'PALM', 'DNA-PAINT', 'WideField'];
const extra = self.ISC_MM_EXTRA = {};
let optics = null;   // the objective's magnification as last picked (a preset's "optics"; from the tier's Startup)
// The optics of a camera preset (sensor pixel, emission magnification), from the configurations that pick it.
function cameraOptics(cam) {
  const o = {};
  for (const T of Object.values(CFG)) for (const g of T.groups) for (const p of g.presets || [])
    if (p.optics && p.set['camera-preset'] === cam) Object.assign(o, p.optics);
  return o;
}
// The optics now: the objective's as picked, the sensor pixel of the camera preset the panel is at (a camera picked in
// the settings counts too) and the Magnification row (Camera box; a camera preset sets it to its own).
function opticsNow() {
  const cp = $('mv_camera-preset'), o = { ...optics, ...(cp ? cameraOptics(+cp.value) : {}) }, em = $('emissionMag');
  if (em && +em.value > 0) o.emissionMag = +em.value;
  return o;
}
// A camera preset brings its own magnification to the camera (EmissionPath.EmissionMagnification goes with the camera).
function takeCameraMag(o) {
  const em = $('emissionMag');
  if (em && o && o.emissionMag && Math.abs(+em.value - o.emissionMag) > 1e-9) { em.value = o.emissionMag; fire(em); }
}
let engine = null;   // option name -> default (isc_scope_options), once a worker has answered
self.onEngineOptions = (names, defaults) => {
  engine = new Map(names.map((n, i) => [n, defaults ? defaults[i] : NaN]));
  pruneExtra();
  markCurrent();
};

// A preset's value of an option: null = the engine's default; a label's dye -1 = Typical, the target's typical dye in
// the mode (data/dyes/library.json typicalLabels), as the adapter resolves it.
function valueOf(key, set) {
  let v = set[key];
  if (v === null) v = engine && engine.has(key) ? engine.get(key) : NaN;
  const m = /^([a-z]+)-dye$/.exec(key);
  if (m && v === -1) {
    const t = DD.specimens.flatMap(sp => sp.targets).find(t => t.prefix === m[1]);
    const mk = m[1] + '-mode', el = $('mv_' + mk);
    const mode = MODES[set[mk] !== undefined && set[mk] !== null ? set[mk] : el ? +el.value : -1];
    const typ = t && DD.typicalLabels[t.id] && DD.typicalLabels[t.id][mode];
    const i = typ ? DD.dyes.findIndex(d => d.id === typ.dye) : -1;
    return i >= 0 ? i : NaN;
  }
  return v;
}
// The value a control shows for an option value (NaN: it cannot).
function controlValue(el, v) {
  if (!Number.isFinite(v)) return null;
  if (el.tagName === 'SELECT') return [...el.options].some(o => o.value === String(v)) ? String(v) : null;
  return v;
}
const fmt3 = v => (+v).toFixed(3);
const fire = el => { el.dispatchEvent(new Event('input', { bubbles: true })); el.dispatchEvent(new Event('change', { bubbles: true })); };
// Options whose -1 is "the camera preset's" (the adapter writes them resolved; tests/web/mm_config_check.mjs checks that
// they are the library camera's): a preset with a camera-preset leaves them following it, so a camera change in the
// panel later still moves them.
const FOLLOW = new Set(['camera-type', 'em-gain', 'qe-curve']);
const follows = (k, set) => FOLLOW.has(k) && set['camera-preset'] !== undefined;
// Labels first (picking a dye or mode loads its light preset and labelling), the light path and numbers after.
const order = k => (/-mode$/.test(k) ? 0 : /-dye$/.test(k) ? 1 : /^specimen$|^camera-preset$/.test(k) ? 2 : 3);
function apply(preset) {
  const set = preset.set || {}, missing = [];
  for (const k of Object.keys(set).sort((a, b) => order(a) - order(b))) {
    const el = $('mv_' + k), v = follows(k, set) ? -1 : valueOf(k, set);
    if (v === -1 && follows(k, set) && !el) { delete extra[k]; continue; }
    if (el) {
      const cv = controlValue(el, v);
      if (cv === null) { missing.push(k + '=' + set[k]); continue; }
      if (el.tagName === 'SELECT' ? el.value !== cv : +el.value !== cv) { el.value = cv; fire(el); }
    } else if (!engine || engine.has(k)) {   // not known yet: checked when the list arrives (pruneExtra)
      if (set[k] === null) delete extra[k]; else extra[k] = v;
    } else missing.push(k);
  }
  if (preset.optics) {
    // the pixel size, as Micro-Manager works it out: the sensor pixel / (objective x emission magnification)
    if (preset.optics.objectiveMag) optics.objectiveMag = preset.optics.objectiveMag;
    takeCameraMag(preset.optics);
    setPixelSize(opticsNow());
  }
  if (set['camera-preset'] !== undefined && $('qeFlat') && $('qeFlat').checked) { $('qeFlat').checked = false; fire($('qeFlat')); }
  if (preset.light) {
    delete extra['light-epi']; delete extra['light-trans'];
    const mod = $('mv_modality'), want = preset.light === 'trans' ? '1' : '0';
    if (mod.value !== want) { mod.value = want; fire(mod); }
    if (preset.light === 'both') { extra['light-epi'] = 1; extra['light-trans'] = 1; }
  }
  if (missing.length) console.info('Configuration ' + preset.name + ': not in the viewer: ' + missing.join(', '));
  if (window.uiSync) uiSync();
  markCurrent();
}
// The pixel size as Micro-Manager works it out: the sensor pixel / (objective x emission magnification).
let settingPx = false;   // (the pixel size set from the optics, not typed)
function setPixelSize(o) {
  const px = o.sensorUm * 1000 / (o.objectiveMag * o.emissionMag), el = $('mv_pixel-nm'), rd = $('magReadout');
  if (el && Number.isFinite(px) && px > 0 && Math.abs(+el.value - px) > 1e-9) {
    el.value = px; settingPx = true;
    try { fire(el); } finally { settingPx = false; }
  }
  if (rd && Number.isFinite(px) && px > 0)
    rd.textContent = `total ${+(o.objectiveMag * o.emissionMag).toFixed(1)}× (objective ${o.objectiveMag}×), sensor pixel ${o.sensorUm} µm`;
}
function pruneExtra() { if (engine) for (const k in extra) if (!engine.has(k) && !/^light-(epi|trans)$/.test(k)) delete extra[k]; }

// Which preset of a group the panel is at now (else blank: "custom"). An option is compared where the viewer has it: a
// control, or an extra a preset set (else the engine's default).
function matches(preset) {
  const set = preset.set || {};
  for (const k in set) {
    const el = $('mv_' + k), v = valueOf(k, set);
    if (follows(k, set) && +(el ? el.value : k in extra ? extra[k] : -1) === -1) continue;
    if (el) {
      const cv = controlValue(el, v);
      if (cv === null) return false;
      if (el.tagName === 'SELECT' ? el.value !== cv : Math.abs(+el.value - cv) > 1e-6 * Math.max(1, Math.abs(cv))) return false;
    } else if (engine && engine.has(k)) {
      const cur = k in extra ? extra[k] : engine.get(k);
      if (!(Math.abs(cur - v) <= 1e-9 * Math.max(1, Math.abs(v)))) return false;
    }
  }
  if (preset.optics && optics) { const o = opticsNow(); for (const k in preset.optics) if (o[k] !== preset.optics[k]) return false; }
  if (preset.light) {
    const bf = $('mv_modality').value === '1', both = extra['light-trans'] === 1 && extra['light-epi'] === 1;
    if ((preset.light === 'trans') !== bf || (preset.light === 'both') !== both) return false;
  }
  return true;
}
let rows = [];
function markCurrent() {
  for (const r of rows) {
    if (r.slider) { const el = $('mv_' + r.slider.key); if (el) { r.range.value = el.value; r.num.value = fmt3(el.value); } continue; }
    const hit = r.group.presets.find(matches);
    r.sel.value = hit ? hit.name : '';
  }
}
// The Exposure row of web/index.html (label, field; live.js mirrors the field) moves into this grid, kept by reference
// (a re-render empties the grid).
const expCells = $('lv_expRow') ? [...$('lv_expRow').children] : [];
if ($('lv_expRow')) $('lv_expRow').remove();
function render() {
  const tier = { advanced: 'Advanced', expert: 'Expert' }[document.body.dataset.level] || 'Basic';
  const T = CFG[tier] || CFG.Basic;
  if (!optics) optics = { ...T.optics };
  host.innerHTML = '';
  rows = [];
  const first = ['Specimen', 'Channel', 'Objective', 'Camera'];
  const groups = T.groups.slice().sort((a, b) => ((first.indexOf(a.name) + 1 || 99) - (first.indexOf(b.name) + 1 || 99)));
  for (const g of groups) {
    // the laser powers are set in Illumination & light path (sliders there), not here
    if (g.slider && /^laser-/.test(g.slider.key)) continue;
    // the viewer calls the Channel group "Mode" and leaves the dye names out of its presets (the dye is in the label
    // settings); the presets and what they set are Micro-Manager's
    const mode = g.name === 'Channel';
    const lab = document.createElement('span'), name = document.createElement('label');
    lab.className = 'lab';
    name.textContent = g.slider ? g.name.replace(/\s*\(.*\)$/, '').replace(/^Laser /, '') : mode ? 'Mode' : g.name;
    name.dataset.tip = mode ? 'The imaging mode: a preset of the Micro-Manager config group <code>Channel</code> (each one also picks its dye and light path)'
      : `A preset of the Micro-Manager config group <code>${g.name}</code>`;
    lab.append(name);
    host.append(lab);
    if (g.slider) {
      const s = g.slider, el = $('mv_' + s.key), wrap = document.createElement('span');
      wrap.style.display = 'flex'; wrap.style.gap = '4px'; wrap.style.alignItems = 'center';
      // three decimals (a PALM 405 nm line runs at a few W/cm^2 = 0.00x kW/cm^2)
      const range = Object.assign(document.createElement('input'), { type: 'range', min: s.lo, max: s.hi, step: 0.001 });
      const num = Object.assign(document.createElement('input'), { type: 'number', min: s.lo, max: s.hi, step: 0.001 });
      num.style.width = '62px';
      range.value = el ? el.value : s.value;
      num.value = fmt3(range.value);
      const set = (v, fromNum) => { if (!el) return; el.value = v; fire(el); range.value = v; if (!fromNum) num.value = fmt3(v); };
      range.addEventListener('input', () => set(range.value));
      num.addEventListener('input', () => set(num.value, true));
      num.addEventListener('change', () => { num.value = fmt3(num.value); });
      wrap.append(range, num);
      host.append(wrap);
      rows.push({ slider: s, range, num });
      continue;
    }
    const sel = document.createElement('select');
    sel.title = g.name;
    sel.dataset.pick = mode ? 'mode' : g.name.toLowerCase();   // its choices are counted (usage.js)
    sel.append(Object.assign(document.createElement('option'), { value: '', textContent: '—', disabled: true }));
    const shown = p => mode ? p.name.replace(/^\S+\s+(?=(dSTORM|DNA-PAINT|PALM|WideField)\b)/, '') : p.name;
    for (const p of g.presets) sel.append(Object.assign(document.createElement('option'), { value: p.name, textContent: shown(p) }));
    sel.addEventListener('change', () => { const p = g.presets.find(q => q.name === sel.value); if (p) apply(p); });
    host.append(sel);
    rows.push({ group: g, sel });
  }
  host.append(...expCells);   // the Exposure row last, in the same grid as the others
  markCurrent();
}
render();
if (optics) { const cp = $('mv_camera-preset'); if (cp) takeCameraMag(cameraOptics(+cp.value)); setPixelSize(opticsNow()); }   // from the start
new MutationObserver(render).observe(document.body, { attributes: true, attributeFilter: ['data-level'] });
let markTimer = 0;
$('panel').addEventListener('change', () => { clearTimeout(markTimer); markTimer = setTimeout(markCurrent, 50); });
$('panel').addEventListener('input', () => { clearTimeout(markTimer); markTimer = setTimeout(markCurrent, 200); });
// A camera picked in the settings changes the pixel size too (its sensor pixel and magnification, the objective as
// picked); so does the Magnification row.
if ($('mv_camera-preset')) $('mv_camera-preset').addEventListener('change', () => {
  if (optics) { takeCameraMag(cameraOptics(+$('mv_camera-preset').value)); setPixelSize(opticsNow()); }
});
if ($('emissionMag')) $('emissionMag').addEventListener('input', () => { if (optics && +$('emissionMag').value > 0) setPixelSize(opticsNow()); });
// A pixel size typed in: as in Micro-Manager it follows from the optics, so the Magnification moves to give it
// (sensor pixel / (objective x pixel size)); the camera and objective stay.
if ($('mv_pixel-nm')) $('mv_pixel-nm').addEventListener('input', () => {   // (a typed value arrives settled, settle.js)
  const px = +$('mv_pixel-nm').value, em = $('emissionMag');
  if (settingPx || !optics || !em || !(px > 0)) return;
  const o = opticsNow(), mag = o.sensorUm * 1000 / (o.objectiveMag * px);
  if (Number.isFinite(mag) && mag > 0 && Math.abs(+em.value - mag) > 1e-9) { em.value = mag; fire(em); }
});
// The objective's magnification (web/live/settings_file.js: an instrument saved and loaded with it).
const objectiveMag = () => (optics ? optics.objectiveMag : undefined);
function setObjectiveMag(m) {
  if (!optics || !(m > 0)) return;
  optics.objectiveMag = m;
  setPixelSize(opticsNow());
  markCurrent();
}
self.mmConfig = { apply, render, markCurrent, pruneExtra, matches, valueOf, extra, config: CFG, objectiveMag, setObjectiveMag };
})();
