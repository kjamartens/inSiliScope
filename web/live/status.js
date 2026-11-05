// SPDX-License-Identifier: BSD-3-Clause
// The live strip's Setup box (beside the stage and the display, #lv_statBox): the lasers as buttons (a click turns a
// line off, or on again at its last power), the setup in four short rows (labels, light, optics, camera), and a line
// that shows only when something is off -- the most severe first; a click on it lists each with what to do.
//
// Loaded after live.js; uses the main script's globals movieSpec, setupJob (the C++ setup readout, isc_scope_setup =
// cli/scope_setup.cpp ViewerSetupJson: the lines on, the excitation filter's T, the regime and depth per line, each
// label's spectra and detected fraction), packCache and cameraRectUm (the cells under the camera), and live.js's
// self.liveState (the frame, its time, an error).
// The checks (docs/try-viewer.md "Setup"): live failed; no light; nothing labelled; a label no laser excites; a label
// whose light the filters block; saturated pixels; the focus below the coverslip, above the cells, off the nanorulers
// or the beads on the glass, above the TIRF light; no cell in view; a laser that excites no label; slow frames.
// (Drift is left out: the user's choice.)
'use strict';
(function () {
const $ = id => document.getElementById(id);
const DD = self.ISC_DYE_DATA, L = self.liveState;
const box = $('lv_statBox');
if (!box || !DD || !L) return;
const chips = $('lv_laserChips'), facts = $('lv_facts'), warnBtn = $('lv_warn'), pop = $('lv_warnPop');
const store = {
  get(k, d) { try { const v = localStorage.getItem('isc.ui.' + k); return v === null ? d : v; } catch (e) { return d; } },
  set(k, v) { try { localStorage.setItem('isc.ui.' + k, v); } catch (e) { /* private window */ } },
};
const esc = s => String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;');
const fire = el => { el.dispatchEvent(new Event('input', { bubbles: true })); el.dispatchEvent(new Event('change', { bubbles: true })); };
const BF = () => !!$('mv_modality') && $('mv_modality').value === '1';
const specimen = () => +($('mv_specimen') ? $('mv_specimen').value : 0);   // 0 cell field, 1 nanorulers, 2 beads, 3 slide
const targetName = (() => {
  const m = new Map();
  for (const sp of DD.specimens || []) for (const t of sp.targets || []) m.set(t.structure, t.name);
  return s => m.get(s) || 'label ' + s;
})();
const waveColour = nm => (self.ISC_SCOPE_PANELS ? self.ISC_SCOPE_PANELS.waveColour(nm, !!(self.iscTheme && self.iscTheme.isLight())) : '');
const exAt = (spec, nm) => (spec && spec.length ? spec[Math.max(0, Math.min(spec.length - 1, Math.round(nm) - 300))] || 0 : 0);   // 300..900 nm, 1 nm
const peakOf = spec => { let b = 0; for (let i = 1; i < spec.length; i++) if (spec[i] > spec[b]) b = i; return 300 + b; };
// a spectrum's value at nm as a fraction of its peak
const relAt = (spec, nm) => { const p = spec && spec.length ? exAt(spec, peakOf(spec)) : 0; return p > 0 ? exAt(spec, nm) / p : 0; };

// ---- the lasers: one button per line; off = 0 kW/cm², on = the line's last power (1 kW/cm² if it never had one) ----
// A button for each line whose row shows at the level (at Basic the lines the labels use, and any line that is on).
const rowShown = el => { const o = el.closest('.opt'); return !o || getComputedStyle(o).display !== 'none'; };
const lines = () => {
  const out = DD.lasers.map(nm => ({ nm, el: $('mv_laser-' + nm) }));
  const cnm = $('mv_laser-custom-nm');
  if (cnm && +cnm.value > 0) out.push({ nm: +cnm.value, el: $('mv_laser-custom'), custom: true });
  return out.filter(l => l.el);
};
let last = {};
try { last = JSON.parse(store.get('laserLast', '{}')) || {}; } catch (e) { last = {}; }
const keyOf = l => (l.custom ? 'custom' : String(l.nm));
function remember() {
  let changed = false;
  for (const l of lines()) { const v = +l.el.value; if (v > 0 && last[keyOf(l)] !== v) { last[keyOf(l)] = v; changed = true; } }
  if (changed) store.set('laserLast', JSON.stringify(last));
}
function drawChips() {
  const ls = lines().filter(l => +l.el.value > 0 || rowShown(l.el)), bf = BF();
  const sig = ls.map(l => l.nm + ':' + (+l.el.value)).join(',') + (bf ? ' bf' : '');
  if (chips.dataset.sig === sig) return;
  chips.dataset.sig = sig;
  chips.textContent = '';
  for (const l of ls) {
    const on = +l.el.value > 0, b = document.createElement('button');
    b.type = 'button';
    b.className = 'chip' + (on ? ' on' : '');
    b.dataset.nm = l.nm;
    b.textContent = l.nm;
    b.setAttribute('aria-pressed', on);
    b.setAttribute('aria-label', l.nm + ' nm laser');
    b.disabled = bf;
    b.title = bf ? 'BrightField: the lamp lights the sample, not the lasers (Modality)'
      : `${l.nm} nm laser${l.custom ? ' (the custom line)' : ''}: ${on ? 'on at ' + (+l.el.value) + ' kW/cm²; click: off' : 'off; click: on at ' + (last[keyOf(l)] || 1) + ' kW/cm²'}`;
    if (on) b.style.setProperty('--chip-c', waveColour(l.nm));
    b.addEventListener('click', () => {
      const now = +l.el.value;
      if (now > 0) { last[keyOf(l)] = now; store.set('laserLast', JSON.stringify(last)); l.el.value = 0; }
      else l.el.value = last[keyOf(l)] || 1;
      fire(l.el);
      drawChips();
      soon();
    });
    chips.appendChild(b);
  }
}

// ---- the setup readout (the spec less the stage position, so moving the stage asks for nothing new) ----
let S = null, specKey = '', asked = '';
const setupSpec = () => movieSpec().split(' ').filter(t => !/^(x|y)=/.test(t)).join(' ');
function refreshSetup() {
  let spec;
  try { spec = setupSpec(); } catch (e) { return; }
  if (spec === specKey || spec === asked) return;
  asked = spec;
  setupJob(spec).then(s => { if (asked === spec) { S = s; specKey = spec; update(); } }, () => { if (asked === spec) asked = ''; });
}
// (the viewer's readout: a label's spectra in dye_main / dye_pre, its physics in main / pre, null when it emits
// nothing; the lines on with their power, the excitation filter's T per nm; the depth per line)
const onLabels = () => (S ? S.labels.filter(l => !l.off && l.main) : []);
const exMain = l => (l.dye_main && l.dye_main.ex) || null, exPre = l => (l.pre && l.dye_pre && l.dye_pre.ex) || null;
const litLines = () => {
  if (!S) return [];
  const T = S.light_path.excitation_T;
  return S.light_path.lasers.map(l => ({ nm: l.nm, kw: l.kw_per_cm2 * (T && T.length ? exAt(T, l.nm) : 1) })).filter(l => l.kw > 1e-6);
};
const depthOf = (ex, lit) => { const d = ex && ex.depth_nm ? lit.map(l => ex.depth_nm[String(l.nm)]).filter(v => v > 0) : []; return d.length ? Math.max(...d) : 0; };
const camera = () => (DD.cameras && $('mv_camera-preset') ? DD.cameras[+$('mv_camera-preset').value] || null : null);
const fmtUm = nm => (nm >= 1000 ? (nm / 1000).toFixed(1) + ' µm' : Math.round(nm) + ' nm');

// ---- the four rows ----
function drawFacts() {
  const rows = [];
  const exp = +$('mv_exposure-ms').value || 50, px = +$('mv_pixel-nm').value || 100, na = +$('mv_na').value;
  if (BF()) {
    rows.push(['Labels', 'BrightField', 'Transmitted light: the cells\' refractive index, no labels']);
    const cna = $('mv_bf-condenser-na') ? +$('mv_bf-condenser-na').value : NaN;
    rows.push(['Light', 'lamp' + (isFinite(cna) ? ' · NA ' + cna : ''), 'The transmitted lamp through the condenser' + (isFinite(cna) ? ' (condenser NA ' + cna + ')' : '')]);
  } else {
    const ls = onLabels(), modes = [...new Set(ls.map(l => l.mode))];
    rows.push(['Labels', !S ? '…' : ls.length ? modes.join(' + ') + (ls.length > 1 ? ' ×' + ls.length : '') : 'none',
      ls.length ? ls.map(l => `${targetName(l.structure)}: ${l.dye_name}, ${l.mode} (${Math.round(100 * (l.main.detected_fraction || 0))} % of its light detected)`).join('\n') : 'No target is labelled']);
    const ex = S && S.excitation, lit = litLines(), depth = depthOf(ex, lit);
    let light = '…', tip = '';
    if (ex) {
      // (HILO whose light reaches over 100 µm deep -- a beam close to the axis -- reads as Epi: it lights the whole sample)
      const regime = ex.regime === 'HILO' && depth > 100000 ? 'Epi' : ex.regime;
      light = regime + (regime === 'TIRF' && depth > 0 ? ' · ' + fmtUm(depth) : '');   // (HILO's depth: in the tooltip)
      const pat = ex.pattern && ex.pattern.type && ex.pattern.type !== 'Off' ? ex.pattern.type : '';
      if (pat) light += ' · ' + pat;
      tip = `${regime === ex.regime ? regime : 'Epi (technically HILO)'} (BFP position ${(+ex.u).toFixed(2)})` + (depth > 0 ? `; the light reaches ${fmtUm(depth)} into the sample` : '') + (pat ? `; pattern ${pat}` : '') +
        (lit.length ? '\n' + lit.map(l => `${l.nm} nm: ${+l.kw.toFixed(3)} kW/cm²`).join('\n') : '\nno laser reaches the sample');
    }
    rows.push(['Light', light, tip]);
  }
  rows.push(['Optics', `NA ${na} · ${px.toFixed(px < 10 ? 1 : 0)} nm px`, `Objective NA ${na}; ${px.toFixed(1)} nm per camera pixel at the sample` + (S ? `\n${S.light_path.dichroic}\n${S.light_path.emission_filter}` : '')]);
  const cam = camera();
  rows.push(['Camera', exp + ' ms', (cam ? cam.name + '; ' : '') + `${exp} ms exposure`]);   // (its name: in the tooltip)
  // (a value breaks only between its parts)
  const html = rows.map(([k, v, t]) => `<div class="f" title="${esc(k + ': ' + t)}"><span class="k">${k}</span><span class="v">${v.split(' · ').map(x => `<span>${esc(x)}</span>`).join(' · ')}</span></div>`).join('');
  if (facts.innerHTML !== html) facts.innerHTML = html;
}

// ---- the checks ----
const SEV = { error: 0, warn: 1, info: 2 };
// a line exciting a dye at under 5 % of its peak absorption does not count; under 20 % it is weak (the library's
// spectra are not all scaled to a peak of 1: relAt divides by the peak)
const EX_MIN = 0.05, EX_WEAK = 0.2;
function checks() {
  const W = [];
  const add = (id, sev, text, fix) => W.push({ id, sev, text, fix });
  if (L.err) add('live', 'error', 'Live failed', `${L.err}. A smaller image (Image size) or fewer labels need less memory; reload the page if it keeps failing.`);
  const z = +$('mv_z').value, sp = specimen();
  if (S && !BF()) {
    const ls = onLabels(), lit = litLines();
    const anyOn = lines().some(l => +l.el.value > 0);
    const beads = !!$('mv_fid-density') && +$('mv_fid-density').value > 0;
    if (!anyOn) add('nolight', 'error', 'All lasers are off', 'Nothing lights the sample, so the frames hold only camera noise. Click a laser above to turn it on, or pick a light preset (Illumination).');
    else if (!lit.length) add('nolight', 'error', 'No laser reaches the sample', 'The excitation filter (Light path) blocks every line that is on: pick one that passes them, or None.');
    if (!ls.length && !beads && sp !== 3) add('nolabel', 'error', 'Nothing is labelled', 'No target carries a dye, so nothing glows. Pick a dye for a target (Sample, its Label), or mount another specimen.');
    const blinkers = ls.some(l => l.mode === 'dSTORM' || l.mode === 'PALM');
    for (const l of ls) {
      const name = `${l.dye_name} (${targetName(l.structure)})`;
      const em = exMain(l), ep = exPre(l);
      if (lit.length && em) {
        const best = Math.max(...lit.map(x => relAt(em, x.nm)), ...(ep ? lit.map(x => relAt(ep, x.nm)) : [0]));
        const peak = peakOf(em), line = DD.lasers.reduce((a, b) => (exAt(em, b) > exAt(em, a) ? b : a));
        if (best < EX_MIN)
          add('unexcited:' + l.structure, 'warn', `No laser excites ${l.dye_name}`,
            `${name} absorbs best near ${peak} nm, and the lines that are on reach under ${Math.round(100 * EX_MIN)} % of that. Turn on the ${line} nm line (or pick its light preset).`);
        else if (best < EX_WEAK && !lit.some(x => x.nm === line))
          add('weak:' + l.structure, 'info', `${l.dye_name} is only weakly excited`,
            `The lines that are on excite ${name} at ${Math.round(100 * best)} % of its peak absorption (near ${peak} nm), so it is dim. The ${line} nm line excites it best.`);
      }
      const det = l.main.detected_fraction;
      if (det !== undefined && det < 0.1)
        add('blocked:' + l.structure, 'warn', `${l.dye_name}'s light is blocked`,
          `Only ${(100 * det).toFixed(det < 0.01 ? 1 : 0)} % of ${name}'s emission (around ${Math.round(l.main.lambda_nm || 0)} nm) passes the dichroic, the emission filter and the camera. Pick a filter set that passes it (Light path).`);
    }
    // a line on that excites none of the labels; 405 and 730 count as activation lines with dSTORM or PALM (not with
    // beads or the slide on: their spectra are not in the readout)
    if (ls.length && !beads && sp !== 3) for (const x of lit) {
      if (blinkers && (x.nm < 420 || x.nm > 700)) continue;
      const used = ls.some(l => (exMain(l) && relAt(exMain(l), x.nm) >= EX_MIN) || (exPre(l) && relAt(exPre(l), x.nm) >= EX_MIN));
      if (!used) add('unused:' + x.nm, 'info', `The ${x.nm} nm line excites no label`, `None of the labelled dyes absorbs at ${x.nm} nm, so it adds no signal here. Turn it off (its button above) unless you want it on.`);
    }
    // TIRF: the light reaches only a short way up
    const ex = S.excitation, depth = depthOf(ex, lit);
    if (ex && ex.regime === 'TIRF' && depth > 0 && z * 1000 > Math.max(500, 3 * depth))
      add('tirf', 'warn', 'The focus is above the TIRF light', `TIRF lights only about ${fmtUm(depth)} above the coverslip, and the focus is at ${z} µm. Focus near 0 µm, or lower the BFP position (Illumination) for HILO.`);
  }
  // the focus
  if (z < -0.25) add('below', 'warn', 'The focus is below the coverslip', `The focus is at ${z} µm, under the glass, where there is nothing to see. Focus up (the wheel, or PageUp) to 0–1 µm.`);
  else if (sp === 1 && Math.abs(z) > 1.5) add('rulers', 'warn', 'The nanorulers are out of focus', `They lie on the coverslip; the focus is at ${z} µm. Focus to about 0 µm.`);
  else if (sp === 2 && $('mv_fid-height') && $('mv_fid-height').value !== '2' && Math.abs(z) > 1.5) add('beads', 'warn', 'The beads are out of focus', `They lie on the coverslip; the focus is at ${z} µm. Focus to about 0 µm.`);
  else if (sp === 0 && typeof packCache !== 'undefined' && packCache && packCache.map && packCache.map.size) {
    const [x0, y0, x1, y1] = cameraRectUm(), cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
    let near = Infinity, top = -1;
    for (const c of packCache.map.values()) {
      near = Math.min(near, Math.hypot(c.x - cx, c.y - cy));
      const dx = Math.max(x0 - c.x, 0, c.x - x1), dy = Math.max(y0 - c.y, 0, c.y - y1);
      if (Math.hypot(dx, dy) < c.rOuter) top = Math.max(top, c.height);
    }
    if (top < 0 && near < 80) add('nocell', 'info', 'No cell in view', 'The camera looks at bare coverslip. Move the stage onto a cell (the joystick, or drag the world view).');
    else if (top > 0 && z > top + 1) add('above', 'warn', 'The focus is above the cells', `The tallest cell in view is ${top.toFixed(1)} µm high and the focus is at ${z} µm. Focus down below ${top.toFixed(1)} µm.`);
  }
  // the frame: saturation, speed
  const fr = L.frame;
  if (fr && L.frames) {
    const max = 65535;   // the 16-bit camera
    let n = 0;
    for (let i = 0; i < fr.length; i++) if (fr[i] >= max) n++;
    if (n) add('saturated', 'warn', `${n} pixel${n > 1 ? 's' : ''} saturated`, `The brightest pixels reach the camera's maximum (${max} ADU), so their values are clipped. Lower the laser power or the exposure.`);
  }
  const expMs = +$('mv_exposure-ms').value || 50;
  if (L.on && L.frames > 3 && L.ms > Math.max(250, 2 * expMs))
    add('slow', 'info', `Frames take ${Math.round(L.ms)} ms`, `Live runs at ${(1000 / L.ms).toFixed(1)} fps, slower than the exposure (${expMs} ms). A smaller image (Image size), Quality Fast or fewer labels make frames faster.`);
  return W.sort((a, b) => SEV[a.sev] - SEV[b.sev]);
}

// ---- the warning line and its list ----
let warnings = [];
const ICON = { error: '✕', warn: '⚠', info: 'ℹ' };
function drawWarnings() {
  warnings = checks();
  box.dataset.warnings = warnings.map(w => w.id).join(' ');
  if (!warnings.length) { warnBtn.hidden = true; pop.hidden = true; return; }
  const w = warnings[0];
  warnBtn.hidden = false;
  warnBtn.dataset.sev = w.sev;
  const html = `<span class="i">${ICON[w.sev]}</span><span class="t">${esc(w.text)}</span>` + (warnings.length > 1 ? `<span class="n">+${warnings.length - 1}</span>` : '');
  if (warnBtn.innerHTML !== html) warnBtn.innerHTML = html;
  warnBtn.title = 'Click: what to do';
  if (!pop.hidden) drawList();
}
function drawList() {
  const html = warnings.map(w => `<div class="w" data-sev="${w.sev}" data-id="${esc(w.id)}"><b><span class="i">${ICON[w.sev]}</span>${esc(w.text)}</b><p>${esc(w.fix)}</p></div>`).join('');
  if (pop.innerHTML !== html) pop.innerHTML = html;
}
const closePop = () => { pop.hidden = true; warnBtn.setAttribute('aria-expanded', 'false'); };
warnBtn.addEventListener('click', ev => {
  ev.stopPropagation();
  if (!pop.hidden) { closePop(); return; }
  drawList();
  pop.hidden = false;
  warnBtn.setAttribute('aria-expanded', 'true');
  const r = warnBtn.getBoundingClientRect(), vw = document.documentElement.clientWidth;
  pop.style.left = '0px'; pop.style.top = '0px';
  const o = pop.getBoundingClientRect();
  pop.style.left = Math.max(8, Math.min(vw - o.width - 8, r.left)) - o.left + 'px';
  pop.style.top = (r.bottom + 4 + o.height < innerHeight - 8 ? r.bottom + 4 : Math.max(8, r.top - o.height - 4)) - o.top + 'px';
});
document.addEventListener('pointerdown', ev => { if (!pop.hidden && !pop.contains(ev.target) && !warnBtn.contains(ev.target)) closePop(); }, true);
document.addEventListener('keydown', ev => { if (ev.key === 'Escape' && !pop.hidden) { closePop(); warnBtn.focus(); } });

// ---- when: any settings change (soon), and twice a second (the frame, the stage, a new setup readout) ----
function update() {
  remember();
  drawChips();
  drawFacts();
  drawWarnings();
}
let timer = 0;
function soon() { clearTimeout(timer); timer = setTimeout(() => { refreshSetup(); update(); }, 150); }
document.addEventListener('change', soon);
document.addEventListener('input', soon);
window.addEventListener('isc-theme', () => { chips.dataset.sig = ''; drawChips(); });
setInterval(() => { refreshSetup(); update(); }, 500);
soon();
self.ISC_STATUS = { warnings: () => warnings.slice(), update: () => { refreshSetup(); update(); } };
})();
