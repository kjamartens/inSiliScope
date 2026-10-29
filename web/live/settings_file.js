// SPDX-License-Identifier: BSD-3-Clause
// Save and load settings (docs/dev/SAVE_SETTINGS.md): the viewer's setup as a file, <name>.isc.json, in five parts that
// can be mixed -- the instrument (hardware), the settings (how it is driven, the render quality), the sample, the
// measurement (where and how long it is imaged, and the sample's light history: what has been done to it) and the
// view (the viewer's own looks, optional). The four engine parts hold every engine option of their part as the engine
// got it (the parsed movieSpec(), the engine's default for the rest; enumerations by name), so insiliscope_cli
// --settings renders the same; which part an option is in is the engine's (ScopeOptionPart, isc_scope_options).
// Loading applies the parts with the panel's couplings held (self.ISC_QUIET: a loaded sample does not load its dye's
// light preset over the settings), replays the light history on the live worker, then compares what the viewer has
// with the file and lists what it could not take. The ☰ menu's first section names each part's source, a short id of
// its values and a one-line summary, with Save... / Load...; two icons beside the settings level open the same dialogs,
// and a .isc.json dropped on the page loads too. Loaded after web/live/live.js and mm_config_ui.js.
'use strict';
(function () {
const $ = id => document.getElementById(id);
const L = self.liveState, store = uiKit.store, DD = self.ISC_DYE_DATA;
const PARTS = ['instrument', 'settings', 'sample', 'measurement', 'view'];
const TITLE = { instrument: 'Instrument', settings: 'Settings', sample: 'Sample', measurement: 'Measurement', view: 'View' };
const HELP = {
  instrument: 'The hardware: objective (NA, immersion, PSF and its aberrations), camera and its noise, pixel size, the beam\'s shape, a custom laser line or filter edges.',
  settings: 'How the instrument is driven: laser powers, the filters and dichroic in place, the BFP position (epi / HILO / TIRF), the light paths, the camera size and EM gain, the render quality.',
  sample: 'The specimen: its world (seed, cells, nanorulers), every target\'s label, dye, mode and imager, the custom dyes, beads, the medium and chamber.',
  measurement: 'Where and how long: stage position and focus, exposure, frames, drift, and the sample\'s light history (what has been done to it: the live frames that lit it).',
  view: 'The viewer\'s looks: the world view\'s camera, layers and colours, the settings level and style, the live display range.',
};
const ENG = () => self.ISC_ENGINE_OPTIONS || new Map();
const fire = el => { el.dispatchEvent(new Event('input', { bubbles: true })); el.dispatchEvent(new Event('change', { bubbles: true })); };
const esc = s => String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/"/g, '&quot;');

// ---- keys, parts, names, the history (web/live/settings_core.js) ----
const C = self.iscSettingsCore, { FORMAT, VERSION, parseSpec, idOf } = C;
const partOf = k => C.partOf(ENG(), k), toName = (k, v) => C.toName(ENG(), k, v), fromName = (k, v) => C.fromName(ENG(), k, v);

// ---- what the viewer has now ----
// The view part: the View box's controls, the world view's zoom, the settings level and style, the live display.
const viewControls = () => [...$('viewPanel').querySelectorAll('input[id], select[id]')]
  .filter(el => !['button', 'file', 'radio'].includes(el.type) && !el.disabled);
const LIVE_VIEW_IDS = ['lv_auto', 'lv_lo', 'lv_hi', 'lv_zStep'];
function viewValues() {
  const v = {};
  for (const el of [...viewControls(), ...LIVE_VIEW_IDS.map($).filter(Boolean)])
    v[el.id] = el.type === 'checkbox' ? el.checked : el.tagName === 'SELECT' || el.value === '' || !Number.isFinite(+el.value) ? el.value : +el.value;
  v['view.scale'] = view.scale;
  v['ui.settingsLevel'] = document.body.dataset.level;
  v['ui.style'] = document.body.dataset.uiStyle;
  return v;
}
// The four engine parts: every engine option of each (the spec's value, else the engine's default), the keys the spec
// has besides (p.*, zern.*, dye fields), names for enumerations; and what the viewer keeps of each beside them.
function current() {
  const spec = parseSpec(movieSpec());
  const values = { instrument: {}, settings: {}, sample: {}, measurement: {} };
  for (const [k, o] of ENG()) if (values[o.part]) values[o.part][k] = spec.has(k) ? spec.get(k) : o.def;
  for (const [k, v] of spec) { const p = partOf(k); if (!ENG().has(k) && values[p]) values[p][k] = v; }
  for (const p in values) for (const k in values[p]) values[p][k] = toName(k, values[p][k]);
  values.view = viewValues();
  const viewer = {
    instrument: { objectiveMag: self.mmConfig ? self.mmConfig.objectiveMag() : undefined, emissionMag: +$('emissionMag').value },
    sample: { userDyes: [1, 2, 3].filter(n => USER_DYES.added[n - 1]).map(n => ({ slot: n, name: $('ud' + n + '-name').value, mode: +$('ud' + n + '-mode').value })) },
    measurement: { recordFrames: +$('lv_recN').value || 100 },
  };
  return { values, viewer };
}
const historyOut = () => C.historyOut(ENG(), L.log, L.t);
const replaySpecs = records => C.replaySpecs(ENG(), records, movieSpec({ frames: 1 }));

// ---- one line per part ----
const f = (v, d = 2) => { const t = (+v).toFixed(d); return t.includes('.') ? t.replace(/\.?0+$/, '') : t; };
function summary(part, values, viewer, history) {
  const v = values || {};
  if (part === 'instrument') {
    const mag = viewer && viewer.objectiveMag ? viewer.objectiveMag + '× ' : '';
    const preset = v['psf-zernike-preset'] && v['psf-zernike-preset'] !== 'None' ? ' (' + v['psf-zernike-preset'] + ')' : '';
    return `${mag}NA ${f(v.na)} · ${v['psf-model']}${preset} · ${v['camera-preset']} · ${f(v['pixel-nm'], 1)} nm/px`;
  }
  if (part === 'settings') {
    const lasers = Object.keys(v).filter(k => /^laser-\d+$/.test(k) && +v[k] > 0).map(k => k.slice(6) + ' ' + f(v[k], 3)).join(', ');
    const light = v['light-trans'] === 1 && v['light-epi'] === 1 ? 'Fluorescence + BrightField' : v.modality;
    return `${light} · ${lasers ? lasers + ' kW/cm²' : 'lasers off'} · BFP ${f(v['illum-bfp'])} · ${v.size} px`;
  }
  if (part === 'sample') {
    const sp = DD.specimens.find(s => s.id === v.specimen) || DD.specimens[0];
    const labels = sp.targets.map(t => {
      const d = v[t.prefix + '-dye'];
      if (d === 'None') return null;
      const m = v[t.prefix + '-mode'], mode = typeof m === 'string' && m !== 'DyeDefault' ? ' ' + m : '';
      return t.name + ' ' + (d === -1 || d === 'Typical' ? 'typical dye' : d) + mode;
    }).filter(Boolean);
    return `${sp.name || sp.id} · ${labels.join(', ') || 'unlabelled'} · seed ${v.seed}`;
  }
  if (part === 'measurement') {
    const h = history && history.records && history.records.length
      ? `${f(history.tSec, 1)} s of history (${history.records.length} run${history.records.length > 1 ? 's' : ''})` : 'a fresh sample';
    return `x ${f(v.x, 1)} y ${f(v.y, 1)} z ${f(v.z, 2)} µm · ${f(v['exposure-ms'], 1)} ms · ${h}`;
  }
  return `${v['ui.settingsLevel'] || ''} · tilt ${f(v.tiltDeg || 0, 0)}° · zoom ${f(v['view.scale'] || 0, 1)} px/µm`;
}

// ---- the file ----
function makeFile(parts, name) {
  const now = current(), out = { format: FORMAT, version: VERSION,
    made: { app: 'viewer', release: self.ISC_VERSION || '', worldVersion: self.ISC_WORLD_VERSION || '', date: new Date().toISOString() },
    parts: {} };
  for (const p of parts) {
    const part = { name, id: idOf(now.values[p]), summary: summary(p, now.values[p], now.viewer[p], p === 'measurement' ? historyOut() : null),
      values: now.values[p] };
    if (now.viewer[p]) part.viewer = now.viewer[p];
    if (p === 'measurement') part.history = historyOut();
    out.parts[p] = part;
  }
  return out;
}
const readFile = text => C.readFile(text, { release: self.ISC_VERSION, worldVersion: self.ISC_WORLD_VERSION });

// ---- applying a file ----
const CUSTOM_CAMERA = () => DD.cameras.findIndex(c => c.id === 'Custom');
// A control's value; a select only to one of its options. Notes what it cannot take.
function setControl(el, v, note) {
  if (el.type === 'checkbox') { const c = !!v; if (el.checked !== c) { el.checked = c; fire(el); } return; }
  if (el.tagName === 'SELECT') {
    const s = String(v);
    if (![...el.options].some(o => o.value === s)) { if (note) note(`${el.id}: ${v} is not one of its choices`); return; }
    if (el.value !== s) { el.value = s; fire(el); }
    return;
  }
  if (el.value === '' || +el.value !== +v) { el.value = v; fire(el); }
}
// The control of a key (null: none): mv_<option>, a world parameter p.<name> by its control (the density sliders are
// the engine's npc-density / lamin-density; the specimen is mv_specimen).
function controlOf(k) {
  if (/^p\./.test(k)) {
    const n = k.slice(2);
    if (n === 'specimen') return undefined;   // (mv_specimen sets it)
    return { npcDensity: $('mv_npc-density'), laminDensity: $('mv_lamin-density') }[n] || $(n);
  }
  return $('mv_' + k);
}
// One engine key of a file onto the viewer.
function setKey(k, raw, note) {
  const extra = self.ISC_MM_EXTRA || {}, v = fromName(k, raw);
  if (!Number.isFinite(v)) { if (note) note(`${k}: ${raw} is not known here`); return; }
  if (k === 'x') { view.cx = v; return; }
  if (k === 'y') { view.cy = v; return; }
  if (k === 'seed') { setControl($('seed'), v, note); return; }
  if (k === 'world-seed' || k === 'psf-kernel-half-width-nm') return;
  const zj = /^zern\.(\d+)$/.exec(k);
  if (zj && self.ISC_ZERN) { self.ISC_ZERN.set(+zj[1], v); return; }   // (the seed's; the viewer's own 3 um: checked after)
  if (k === 'qe-curve') {
    const flat = v === CUSTOM_CAMERA();
    setControl($('qeFlat'), flat, note);
    if (flat || v === -1) delete extra[k]; else extra[k] = v;
    return;
  }
  const el = controlOf(k);
  if (el === undefined) return;
  if (el) { setControl(el, v, note); return; }
  const o = ENG().get(k);
  if (o) { if (v === o.def) delete extra[k]; else extra[k] = v; return; }
  if (note) note(`${k}: not a setting of this viewer`);
}
// The order a part's keys are set in: the specimen and the camera first (they move other rows), the pixel size last
// (it sets the Magnification that gives it).
const rank = k => (k === 'specimen' ? 0 : k === 'camera-preset' ? 1 : k === 'pixel-nm' ? 3 : 2);
function applyView(values, note) {
  for (const [k, v] of Object.entries(values || {})) {
    if (k === 'view.scale') { if (+v > 0) view.scale = +v; continue; }
    if (k === 'ui.settingsLevel') { const b = document.querySelector(`#levelSeg [data-level="${v}"]`); if (b) b.click(); continue; }
    if (k === 'ui.style') { const r = document.querySelector(`#menu [name=uiStyle][value="${v}"]`); if (r && !r.checked) { r.checked = true; fire(r); } continue; }
    const el = $(k);
    if (el && (el.closest('#viewPanel') || LIVE_VIEW_IDS.includes(k))) setControl(el, v, note);
    else if (note) note(`view ${k}: not in this viewer`);
  }
}
async function applyFile(file, parts) {
  const notes = [], note = s => notes.push(s);
  const want = PARTS.filter(p => parts.includes(p) && file.parts[p]);
  const wasOn = L.on;
  if (wasOn) L.setLive(false);
  self.ISC_QUIET = true;
  try {
    // the viewer's own first: the custom dye slots (their rows must exist), the objective
    const sv = want.includes('sample') && file.parts.sample.viewer;
    if (sv && Array.isArray(sv.userDyes)) {
      for (let n = 1; n <= 3; n++) {
        const u = sv.userDyes.find(d => d.slot === n);
        USER_DYES.added[n - 1] = !!u;
        if (u) { setControl($('ud' + n + '-name'), u.name || 'My dye ' + n); setControl($('ud' + n + '-mode'), u.mode); }
      }
      DYE_UI.refreshUserDyes();
    }
    if (want.includes('instrument') && self.ISC_ZERN) self.ISC_ZERN.clear();   // (the file's zern.<j>, if any, follow)
    const iv = want.includes('instrument') && file.parts.instrument.viewer;
    if (iv && self.mmConfig && iv.objectiveMag) self.mmConfig.setObjectiveMag(+iv.objectiveMag);
    if (iv && iv.emissionMag) setControl($('emissionMag'), +iv.emissionMag, note);
    // twice: a handler the first pass fires may move a value set before it
    for (let pass = 0; pass < 2; pass++)
      for (const p of want) {
        if (p === 'view') continue;
        const keys = Object.keys(file.parts[p].values || {}).sort((a, b) => rank(a) - rank(b));
        for (const k of keys) setKey(k, file.parts[p].values[k], pass ? null : note);
      }
    const mv = want.includes('measurement') && file.parts.measurement.viewer;
    if (mv && mv.recordFrames) { $('lv_recN').value = mv.recordFrames; fire($('lv_recN')); }
    if (want.includes('view')) applyView(file.parts.view.values, note);
  } finally { self.ISC_QUIET = false; }
  uiSync();
  if (self.mmConfig) { self.mmConfig.pruneExtra(); self.mmConfig.markCurrent(); }
  requestDraw();
  // the sample's light history: replayed on the live worker (none: a fresh sample)
  let replay = '';
  if (want.includes('measurement')) {
    const h = file.parts.measurement.history, recs = h && Array.isArray(h.records) ? h.records : [];
    if (recs.length) {
      try { const t = await L.replay(replaySpecs(recs)); replay = `the sample's light history replayed (${recs.length} runs, ${t.toFixed(1)} s)`; }
      catch (e) { note('the light history could not be replayed: ' + e.message); L.freshSample(); }
    } else L.freshSample();
  }
  if (wasOn && !L.on) L.setLive(true);
  // what the viewer has now against the file
  const now = current(), differ = [];
  for (const p of want) {
    if (p === 'view') continue;
    for (const [k, raw] of Object.entries(file.parts[p].values || {})) {
      if (now.values[p][k] === undefined) continue;   // (a key this viewer does not know: in the notes)
      const a = fromName(k, raw), b = fromName(k, now.values[p][k]);
      if (!(Math.abs(a - b) <= 1e-9 * Math.max(1, Math.abs(a)))) differ.push(`${k} ${raw} → ${now.values[p][k]}`);
    }
  }
  for (const p of want) setSource(p, file.parts[p].name || file.name || 'file', idOf(now.values[p]));
  showSetup();
  return { notes, differ, replay };
}

// ---- the menu's section: each part's source, id and summary ----
const source = (() => { try { return JSON.parse(store.get('setup', '{}')) || {}; } catch (e) { return {}; } })();
function setSource(p, name, id) { source[p] = { name, id }; store.set('setup', JSON.stringify(source)); }
// The ☰ menu's first section: a row per part, Save... / Load...; two icons beside the settings level do the same.
const menu = $('menu'), menuBtn = $('menuBtn');
const sec = document.createElement('div');
sec.id = 'menuSetup';
sec.innerHTML = '<h4 title="A setup in five parts, each saved and loaded on its own; a .isc.json dropped on the page loads too">Save / load settings</h4>' +
  '<div id="lv_setupRows"></div><div class="btns"><button id="lv_setSave" title="Save the setup (or some of its parts) as a .isc.json file">Save…</button>' +
  '<button id="lv_setLoad" title="Load a .isc.json file: all of it, or some of its parts over what is here">Load…</button></div>';
menu.prepend(sec);
// standard icons: a floppy disk (save) and an open folder (load)
const ICON = {
  save: '<path d="M3 2.5h8.2L13.5 4.8V13a.5.5 0 0 1-.5.5H3a.5.5 0 0 1-.5-.5V3a.5.5 0 0 1 .5-.5Z"/><path d="M5 2.5v3.5h5V2.5M4.8 13.5V9.3h6.4v4.2"/>',
  load: '<path d="M1.8 12.8V3.5a.5.5 0 0 1 .5-.5h3.6l1.4 1.5h5.2a.5.5 0 0 1 .5.5v1.6"/><path d="M1.8 12.8 3.8 7a.6.6 0 0 1 .6-.4h10a.4.4 0 0 1 .4.5l-1.9 5.7Z"/>',
};
const icons = document.createElement('div');
icons.id = 'setIcons';
icons.innerHTML = ['save', 'load'].map(k => `<button id="setIco${k === 'save' ? 'Save' : 'Load'}" title="${k === 'save' ? 'Save settings… (a .isc.json file of the setup or some of its parts)' : 'Load settings… (a .isc.json file, all of it or some of its parts)'}" ` +
  `aria-label="${k === 'save' ? 'Save settings' : 'Load settings'}"><svg viewBox="0 0 16 16" width="14" height="14" fill="none" stroke="currentColor" stroke-width="1.3" stroke-linejoin="round">${ICON[k]}</svg></button>`).join('');
menuBtn.before(icons);
const closeMenu = () => { menu.classList.remove('open'); menuBtn.setAttribute('aria-expanded', 'false'); };
const css = document.createElement('style');
css.textContent = `
  #menuSetup{width:300px;max-width:100%} #menuSetup h4{margin-top:0}
  #setIcons{display:flex;gap:2px;flex:none} #setIcons button{padding:3px 5px;line-height:0}
  body.mobile #setIcons{display:none}   /* (no room in the phone's bar: ☰ has Save / Load) */
  #lv_setupRows{display:grid;grid-template-columns:auto 1fr auto;gap:1px 6px;font-size:11px;align-items:baseline}
  #lv_setupRows .p{color:var(--muted)} #lv_setupRows .n{overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
  #lv_setupRows .i{font-family:ui-monospace,Consolas,monospace;color:var(--muted)} #lv_setupRows .i.ed::after{content:' edited';color:var(--accent)}
  #lv_setupRows .s{grid-column:1/-1;color:var(--muted);font-size:10px;margin:-1px 0 2px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
  #setDlg{position:fixed;inset:0;z-index:50;background:rgba(0,0,0,.45);display:flex;align-items:center;justify-content:center}
  #setDlg[hidden]{display:none}
  #setDlg .box{background:var(--panel);color:var(--text);border:1px solid var(--border);border-radius:8px;padding:12px 14px;width:min(560px,calc(100vw - 32px));
    max-height:calc(100vh - 48px);overflow:auto;box-shadow:0 8px 28px rgba(0,0,0,.45);font-size:12px}
  #setDlg h3{margin:0 0 8px;font-size:14px} #setDlg .part{display:grid;grid-template-columns:auto 1fr;gap:0 8px;margin:6px 0}
  #setDlg .part .s{grid-column:2;color:var(--muted);font-size:11px} #setDlg .warn{color:#e0a64a;margin:6px 0}
  #setDlg .msg{color:var(--muted);margin:6px 0;white-space:pre-wrap;max-height:180px;overflow:auto;font-size:11px}
  #setDlg .row{display:flex;gap:8px;align-items:center;margin-top:10px} #setDlg .row .sp{flex:1}
  #setDlg input[type=text]{flex:1;min-width:0}`;
document.head.append(css);
let setupTimer = 0;
function showSetup() {
  if (!ENG().size) return;
  const now = current(), rows = [];
  for (const p of PARTS) {
    const id = idOf(now.values[p]), s = source[p], edited = s && s.id !== id;
    const name = s ? s.name : p === 'view' ? 'this browser' : 'defaults';
    const sum = summary(p, now.values[p], now.viewer[p], p === 'measurement' ? historyOut() : null);
    rows.push(`<span class="p" title="${esc(HELP[p])}">${TITLE[p]}</span><span class="n" title="${esc(name)}">${esc(name)}</span>` +
      `<span class="i${edited ? ' ed' : ''}" title="A short id of this part's values: the same values, the same id${edited ? '; changed since it was loaded or saved' : ''}">${id}</span>` +
      `<span class="s" title="${esc(sum)}">${esc(sum)}</span>`);
  }
  $('lv_setupRows').innerHTML = rows.join('');
}
const menuOpen = () => menu.classList.contains('open');
const showSetupSoon = () => { clearTimeout(setupTimer); if (menuOpen()) setupTimer = setTimeout(showSetup, 300); };
menuBtn.addEventListener('click', () => setTimeout(() => { if (menuOpen()) showSetup(); }));
document.addEventListener('change', showSetupSoon);
document.addEventListener('input', showSetupSoon);
window.addEventListener('isc-reset', () => { for (const p of PARTS) delete source[p]; store.set('setup', '{}'); showSetupSoon(); });
setInterval(() => { if (menuOpen()) showSetup(); }, 2000);   // (the history grows while live runs)

// ---- the dialog ----
const dlg = document.createElement('div');
dlg.id = 'setDlg';
dlg.hidden = true;
dlg.setAttribute('role', 'dialog');
dlg.innerHTML = '<div class="box"></div>';
document.body.append(dlg);
const dbox = dlg.querySelector('.box');
const closeDlg = () => { dlg.hidden = true; };
dlg.addEventListener('pointerdown', ev => { if (ev.target === dlg) closeDlg(); });
document.addEventListener('keydown', ev => { if (ev.key === 'Escape' && !dlg.hidden) closeDlg(); });
const partRows = (have, checked, sums) => PARTS.filter(p => have.includes(p)).map(p =>
  `<label class="part" title="${esc(HELP[p])}"><input type="checkbox" data-part="${p}"${checked(p) ? ' checked' : ''}> <b>${TITLE[p]}</b>` +
  `<span class="s">${esc(sums[p] || '')}</span></label>`).join('');
const chosen = () => [...dbox.querySelectorAll('[data-part]')].filter(i => i.checked).map(i => i.dataset.part);

function openSave() {
  if (!ENG().size) return;
  const now = current(), sums = {};
  for (const p of PARTS) sums[p] = summary(p, now.values[p], now.viewer[p], p === 'measurement' ? historyOut() : null);
  dbox.innerHTML = '<h3>Save settings</h3><div class="row"><label for="setName">Name</label><input type="text" id="setName" maxlength="60"></div>' +
    partRows(PARTS, p => p !== 'view', sums) +
    '<div class="msg">A .isc.json file: every value of each part (defaults too) by its engine name, so it means the same in a later release; insiliscope_cli --settings renders it.</div>' +
    '<div class="row"><span class="sp"></span><button id="setCancel">Cancel</button><button id="setGo">Save</button></div>';
  $('setName').value = store.get('setupSaveName', 'insiliscope-setup');
  dlg.hidden = false;
  $('setName').focus(); $('setName').select();
  $('setCancel').onclick = closeDlg;
  $('setGo').onclick = () => {
    const parts = chosen(), name = ($('setName').value.trim() || 'insiliscope-setup').replace(/[\\/:*?"<>|]+/g, '_');
    if (!parts.length) return;
    store.set('setupSaveName', name);
    const file = makeFile(parts, name), text = JSON.stringify(file, null, 1);
    for (const p of parts) setSource(p, name, file.parts[p].id);
    const a = document.createElement('a');
    a.href = URL.createObjectURL(new Blob([text], { type: 'application/json' }));
    a.download = name + '.isc.json';
    a.click();
    setTimeout(() => URL.revokeObjectURL(a.href), 30000);
    self.iscLastSavedSettings = file;   // (the browser tests read it)
    closeDlg();
    showSetup();
  };
}
function openLoad(text, fileName) {
  let read;
  try { read = readFile(text); } catch (e) {
    dbox.innerHTML = `<h3>Load settings</h3><div class="warn">${esc(fileName)}: ${esc(e.message)}</div><div class="row"><span class="sp"></span><button id="setCancel">Close</button></div>`;
    dlg.hidden = false; $('setCancel').onclick = closeDlg; return;
  }
  const file = read.file, have = PARTS.filter(p => file.parts[p]), sums = {};
  file.name = fileName.replace(/\.isc\.json$|\.json$/i, '');
  for (const p of have) sums[p] = file.parts[p].summary || summary(p, file.parts[p].values, file.parts[p].viewer, file.parts[p].history);
  dbox.innerHTML = `<h3>Load settings: ${esc(file.name)}</h3>` + read.warnings.map(w => `<div class="warn">${esc(w)}</div>`).join('') +
    (have.length ? partRows(have, () => true, sums) : '<div class="warn">The file has no parts.</div>') +
    '<div class="msg">The parts ticked replace what is here; the others stay. A measurement with a light history replays it on the live sample.</div>' +
    '<div class="msg" id="setOut" hidden></div>' +
    '<div class="row"><span class="sp"></span><button id="setCancel">Cancel</button><button id="setGo">Load</button></div>';
  dlg.hidden = false;
  $('setCancel').onclick = closeDlg;
  $('setGo').onclick = async () => {
    const parts = chosen();
    if (!parts.length) return;
    $('setGo').disabled = true;
    const r = await applyFile(file, parts);
    self.iscLastLoad = r;   // (the browser tests read it)
    const out = $('setOut'), lines = [];
    lines.push('Loaded: ' + parts.map(p => TITLE[p]).join(', ') + '.' + (r.replay ? ' ' + r.replay[0].toUpperCase() + r.replay.slice(1) + '.' : ''));
    if (r.differ.length) lines.push(`Not taken (this viewer's value kept): ${r.differ.join('; ')}`);
    if (r.notes.length) lines.push(r.notes.join('\n'));
    out.textContent = lines.join('\n');
    out.hidden = false;
    $('setGo').hidden = true;
    $('setCancel').textContent = 'Close';
  };
}
function pickFile() {
  const inp = document.createElement('input');
  inp.type = 'file';
  inp.accept = '.json,application/json';
  inp.onchange = () => { const fl = inp.files[0]; if (fl) fl.text().then(t => openLoad(t, fl.name)); };
  inp.click();
}
for (const [id, go] of [['lv_setSave', openSave], ['lv_setLoad', pickFile], ['setIcoSave', openSave], ['setIcoLoad', pickFile]])
  $(id).addEventListener('click', ev => { ev.stopPropagation(); closeMenu(); go(); });
// a .isc.json dropped on the page loads it
document.addEventListener('dragover', ev => { if ([...(ev.dataTransfer.items || [])].some(i => i.kind === 'file')) ev.preventDefault(); });
document.addEventListener('drop', ev => {
  const fl = ev.dataTransfer.files && ev.dataTransfer.files[0];
  if (!fl || !/\.json$/i.test(fl.name)) return;
  ev.preventDefault();
  fl.text().then(t => openLoad(t, fl.name));
});

// for the browser tests and the console: the file of the current setup, and loading one
self.iscSettings = { makeFile, readFile, applyFile, current, partOf, idOf, open: openLoad, PARTS };
})();
